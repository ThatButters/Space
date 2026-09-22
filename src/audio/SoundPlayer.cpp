#include "audio/SoundPlayer.h"
#include "core/Log.h"

#ifdef _WIN32
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

namespace space::audio {

SoundPlayer::~SoundPlayer() { stop(); }

bool SoundPlayer::start() {
#ifdef _WIN32
    if (m_thread.joinable()) {
        if (m_running || !m_ready) return m_running; // running, or still opening the device
        m_thread.join();                             // the device failed or went away: open it again
    }
    m_stop = false;
    m_ready = false;
    m_thread = std::thread([this] { threadMain(); });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!m_ready && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return m_running;
#else
    return false;
#endif
}

void SoundPlayer::stop() {
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
    m_running = false;
}

void SoundPlayer::play(const Clip& clip, float gain, float pitch) {
    if (!clip || clip->empty() || !m_running) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_voices.size() >= 24) m_voices.erase(m_voices.begin());
    m_voices.push_back({clip, 0.0, gain, pitch});
}

#ifdef _WIN32
void SoundPlayer::threadMain() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED(hr);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    WAVEFORMATEX* fmt = nullptr;
    auto cleanup = [&] {
        if (client) client->Stop();
        if (render) render->Release();
        if (fmt) CoTaskMemFree(fmt);
        if (client) client->Release();
        if (device) device->Release();
        if (enumerator) enumerator->Release();
        if (comOwned) CoUninitialize();
        m_running = false;
        m_ready = true;
    };

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                          (void**)&enumerator);
    if (FAILED(hr) || FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) ||
        FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client)) ||
        FAILED(client->GetMixFormat(&fmt))) {
        LOG_WARN("Sound: no output device");
        cleanup();
        return;
    }
    bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        isFloat = IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(fmt)->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != 0;
    const unsigned channels = fmt->nChannels;
    if (!isFloat && fmt->wBitsPerSample != 16) {
        LOG_WARN("Sound: unsupported mix format");
        cleanup();
        return;
    }
    if (FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1'000'000, 0, fmt, nullptr)) ||
        FAILED(client->GetService(__uuidof(IAudioRenderClient), (void**)&render))) {
        LOG_WARN("Sound: output init failed");
        cleanup();
        return;
    }
    UINT32 bufferFrames = 0;
    client->GetBufferSize(&bufferFrames);
    m_sampleRate = fmt->nSamplesPerSec;
    const float rate = (float)fmt->nSamplesPerSec;
    client->Start();
    m_running = true;
    m_ready = true;
    LOG_INFO("Sound: output at {} Hz, {} ch", fmt->nSamplesPerSec, channels);

    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> uni(-1.f, 1.f);
    float trickleGain = 0.f, lp1 = 0.f, lp2 = 0.f, hp = 0.f, gurgle = 0.f, drop = 0.f, dropPhase = 0.f;
    std::vector<float> mono;
    while (!m_stop) {
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;
        const UINT32 frames = std::min<UINT32>(bufferFrames - padding, (UINT32)(rate * 0.03f));
        if (frames < 64) {
            Sleep(4);
            continue;
        }
        mono.assign(frames, 0.f);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (Voice& v : m_voices) {
                const auto& s = *v.clip;
                for (UINT32 i = 0; i < frames; ++i) {
                    const size_t k = (size_t)v.pos;
                    if (k + 1 >= s.size()) {
                        v.pos = (double)s.size();
                        break;
                    }
                    const float f = (float)(v.pos - (double)k);
                    mono[i] += (s[k] + (s[k + 1] - s[k]) * f) * v.gain;
                    v.pos += v.pitch;
                }
            }
            m_voices.erase(std::remove_if(m_voices.begin(), m_voices.end(),
                                          [](const Voice& v) { return v.pos + 1.0 >= (double)v.clip->size(); }),
                           m_voices.end());
        }
        // Trickle: band-limited noise with a slow gurgle and the odd droplet.
        const float target = m_trickle ? 1.f : 0.f;
        for (UINT32 i = 0; i < frames; ++i) {
            trickleGain += (target - trickleGain) * (target > trickleGain ? 0.002f : 0.0006f);
            if (trickleGain < 1e-4f) continue;
            const float n = uni(rng);
            lp1 += (n - lp1) * 0.35f;
            lp2 += (lp1 - lp2) * 0.35f;
            hp += (lp2 - hp) * 0.04f;
            gurgle += (uni(rng) - gurgle) * 0.0004f;
            float sample = (lp2 - hp) * (0.55f + 3.f * std::abs(gurgle));
            if (drop <= 0.f && uni(rng) > 0.9993f) {
                drop = 1.f;
                dropPhase = 0.f;
            }
            if (drop > 0.f) {
                dropPhase += (900.f + 1400.f * drop) / rate;
                sample += std::sin(dropPhase * 6.2831853f) * drop * 0.35f;
                drop -= 25.f / rate;
            }
            mono[i] += sample * 0.5f * trickleGain;
        }
        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(frames, &data))) break;
        for (UINT32 i = 0; i < frames; ++i) {
            const float s = std::tanh(mono[i] * 0.9f); // soft clip when several gags pile up
            for (unsigned c = 0; c < channels; ++c) {
                if (isFloat) reinterpret_cast<float*>(data)[i * channels + c] = s;
                else reinterpret_cast<int16_t*>(data)[i * channels + c] = (int16_t)(s * 32000.f);
            }
        }
        render->ReleaseBuffer(frames, 0);
    }
    cleanup();
}
#else
void SoundPlayer::threadMain() { m_ready = true; }
#endif

} // namespace space::audio
