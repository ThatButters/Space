#include "audio/AudioCapture.h"
#include "core/Log.h"

#ifdef _WIN32
#include <windows.h>
#include <initguid.h>
#include <propidl.h>
#include <propkeydef.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#endif

#include <algorithm>
#include <cstring>

namespace space::audio {

namespace {
constexpr size_t kRingSamples = 1 << 16; // ~1.4 s at 48 kHz
}

AudioCapture::~AudioCapture() { stop(); }

bool AudioCapture::start() {
#ifdef _WIN32
    if (m_running) return true;
    if (m_thread.joinable()) m_thread.join(); // an earlier thread that failed or lost its device
    m_ring.assign(kRingSamples, 0.f);
    m_stop = false;
    m_thread = std::thread([this] { threadMain(); });
    return true;
#else
    return false;
#endif
}

void AudioCapture::stop() {
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
    m_running = false;
}

void AudioCapture::latest(float* out, size_t count) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const size_t n = m_ring.size();
    if (n == 0) {
        std::fill(out, out + count, 0.f);
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        size_t idx = (m_write + n - count + i) % n;
        out[i] = m_ring[idx];
    }
}

void AudioCapture::push(const float* mono, size_t count) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const size_t n = m_ring.size();
    for (size_t i = 0; i < count; ++i) {
        m_ring[m_write] = mono[i];
        m_write = (m_write + 1) % n;
    }
}

#ifdef _WIN32
void AudioCapture::threadMain() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED(hr);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* fmt = nullptr;

    auto cleanup = [&] {
        if (client) client->Stop();
        if (capture) capture->Release();
        if (fmt) CoTaskMemFree(fmt);
        if (client) client->Release();
        if (device) device->Release();
        if (enumerator) enumerator->Release();
        if (comOwned) CoUninitialize();
        m_running = false;
        std::lock_guard<std::mutex> lock(m_mutex); // no frozen spectrum from the last packets heard
        std::fill(m_ring.begin(), m_ring.end(), 0.f);
    };

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          (void**)&enumerator);
    if (FAILED(hr)) { LOG_WARN("Audio: no device enumerator ({:#x})", (unsigned)hr); cleanup(); return; }
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) { LOG_WARN("Audio: no default render device ({:#x})", (unsigned)hr); cleanup(); return; }

    {
        IPropertyStore* props = nullptr;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
                int len = WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                std::string name(len > 0 ? len - 1 : 0, '\0');
                if (len > 1) WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, name.data(), len, nullptr, nullptr);
                m_deviceName = name;
            }
            PropVariantClear(&v);
            props->Release();
        }
    }

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (FAILED(hr)) { LOG_WARN("Audio: cannot activate client ({:#x})", (unsigned)hr); cleanup(); return; }
    hr = client->GetMixFormat(&fmt);
    if (FAILED(hr)) { cleanup(); return; }

    // Work out how to read samples: 32-bit float or 16-bit PCM, N channels.
    bool isFloat = false;
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) isFloat = true;
    else if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(fmt);
        isFloat = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    }
    const unsigned channels = fmt->nChannels;
    const unsigned bits = fmt->wBitsPerSample;
    m_sampleRate = fmt->nSamplesPerSec;
    if (!isFloat && bits != 16) {
        LOG_WARN("Audio: unsupported mix format ({} bits)", bits);
        cleanup();
        return;
    }

    const REFERENCE_TIME bufferDuration = 2'000'000; // 200 ms
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, bufferDuration, 0, fmt, nullptr);
    if (FAILED(hr)) { LOG_WARN("Audio: loopback init failed ({:#x})", (unsigned)hr); cleanup(); return; }
    hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);
    if (FAILED(hr)) { cleanup(); return; }
    hr = client->Start();
    if (FAILED(hr)) { cleanup(); return; }

    m_running = true;
    LOG_INFO("Audio: loopback capture on '{}' ({} Hz, {} ch, {})", m_deviceName, m_sampleRate.load(), channels,
             isFloat ? "float" : "pcm16");

    std::vector<float> mono;
    // When nothing is playing, loopback often delivers no packets at all rather than silent ones: feed the
    // ring silence for the gap, or the analyser would hold the last notes it heard for ever.
    ULONGLONG lastPacket = GetTickCount64();
    while (!m_stop) {
        UINT32 packet = 0;
        if (FAILED(capture->GetNextPacketSize(&packet))) break;
        if (packet == 0) {
            const ULONGLONG now = GetTickCount64();
            if (now - lastPacket > 50) {
                mono.assign((size_t)((now - lastPacket) * m_sampleRate / 1000), 0.f);
                push(mono.data(), std::min(mono.size(), kRingSamples));
                lastPacket = now;
            }
            Sleep(4);
            continue;
        }
        lastPacket = GetTickCount64();
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
        mono.resize(frames);
        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            std::fill(mono.begin(), mono.end(), 0.f);
        } else if (isFloat) {
            const float* f = reinterpret_cast<const float*>(data);
            for (UINT32 i = 0; i < frames; ++i) {
                float sum = 0.f;
                for (unsigned c = 0; c < channels; ++c) sum += f[i * channels + c];
                mono[i] = sum / (float)channels;
            }
        } else {
            const int16_t* s = reinterpret_cast<const int16_t*>(data);
            for (UINT32 i = 0; i < frames; ++i) {
                float sum = 0.f;
                for (unsigned c = 0; c < channels; ++c) sum += s[i * channels + c] / 32768.f;
                mono[i] = sum / (float)channels;
            }
        }
        capture->ReleaseBuffer(frames);
        push(mono.data(), mono.size());
    }
    cleanup();
}
#else
void AudioCapture::threadMain() {}
#endif

} // namespace space::audio
