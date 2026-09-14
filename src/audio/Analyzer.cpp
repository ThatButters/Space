#include "audio/Analyzer.h"
#include "audio/AudioCapture.h"

#include <algorithm>
#include <cmath>

namespace space::audio {

namespace {

// In-place iterative radix-2 FFT.
void fft(std::vector<float>& re, std::vector<float>& im) {
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.f * 3.14159265f / (float)len;
        const float wr = std::cos(ang), wi = std::sin(ang);
        for (size_t i = 0; i < n; i += len) {
            float cr = 1.f, ci = 0.f;
            for (size_t k = 0; k < len / 2; ++k) {
                const float ur = re[i + k], ui = im[i + k];
                const float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                const float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr;
                im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr;
                im[i + k + len / 2] = ui - vi;
                const float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

// Smooth toward target with different rates up and down (per-second time constants).
float follow(float current, float target, float dt, float attack, float release) {
    const float rate = target > current ? attack : release;
    return current + (target - current) * (1.f - std::exp(-dt * rate));
}

} // namespace

void Analyzer::update(const AudioCapture& capture, float dt) {
    if (m_samples.size() != kFft) {
        m_samples.assign(kFft, 0.f);
        m_re.assign(kFft, 0.f);
        m_im.assign(kFft, 0.f);
        m_window.resize(kFft);
        for (size_t i = 0; i < kFft; ++i)
            m_window[i] = 0.5f - 0.5f * std::cos(2.f * 3.14159265f * (float)i / (float)(kFft - 1));
    }
    capture.latest(m_samples.data(), kFft);

    float rms = 0.f;
    for (size_t i = 0; i < kFft; ++i) {
        m_re[i] = m_samples[i] * m_window[i];
        m_im[i] = 0.f;
        rms += m_samples[i] * m_samples[i];
    }
    rms = std::sqrt(rms / (float)kFft);
    m_silent = rms < 1e-4f;

    fft(m_re, m_im);

    // Log-spaced bands from 40 Hz to 16 kHz.
    const float sr = (float)std::max(capture.sampleRate(), 8000u);
    const float binHz = sr / (float)kFft;
    const float fLo = 40.f, fHi = 16000.f;
    std::array<float, kBands> raw{};
    for (int b = 0; b < kBands; ++b) {
        const float f0 = fLo * std::pow(fHi / fLo, (float)b / kBands);
        const float f1 = fLo * std::pow(fHi / fLo, (float)(b + 1) / kBands);
        size_t k0 = (size_t)std::max(1.f, f0 / binHz), k1 = (size_t)std::max((float)k0 + 1.f, f1 / binHz);
        k1 = std::min(k1, kFft / 2);
        float sum = 0.f;
        for (size_t k = k0; k < k1; ++k) sum += std::sqrt(m_re[k] * m_re[k] + m_im[k] * m_im[k]);
        raw[b] = sum / (float)(k1 - k0) / (float)kFft * 4.f;
    }

    // Auto-gain: each band is normalised by a slowly decaying running peak so quiet tracks still move.
    for (int b = 0; b < kBands; ++b) {
        m_bandPeak[b] = std::max(raw[b], m_bandPeak[b] * std::exp(-dt * 0.15f));
        const float norm = m_bandPeak[b] > 1e-6f ? raw[b] / m_bandPeak[b] : 0.f;
        m_bands[b] = follow(m_bands[b], std::clamp(norm, 0.f, 1.f), dt, 40.f, 6.f);
    }

    auto avg = [&](int from, int to) {
        float s = 0.f;
        for (int b = from; b < to; ++b) s += m_bands[b];
        return s / (float)(to - from);
    };
    const float bassNow = avg(0, 8), midNow = avg(8, 20), trebleNow = avg(20, kBands);
    {
        float num = 0.f, den = 0.f;
        for (int b = 0; b < kBands; ++b) {
            num += raw[b] * (float)b;
            den += raw[b];
        }
        if (den > 1e-6f && !m_silent)
            m_centroid = follow(m_centroid, num / den / (float)(kBands - 1), dt, 1.2f, 1.2f);
    }
    m_bass = follow(m_bass, bassNow, dt, 30.f, 5.f);
    m_mid = follow(m_mid, midNow, dt, 30.f, 5.f);
    m_treble = follow(m_treble, trebleNow, dt, 30.f, 5.f);
    m_level = follow(m_level, std::clamp(rms * 4.f, 0.f, 1.f), dt, 30.f, 3.f);
    if (!m_silent) {
        const float e = std::clamp(rms * 4.f, 0.f, 1.f);
        m_levelFast = follow(m_levelFast, e, dt, 1.6f, 1.6f);
        m_levelSlow = follow(m_levelSlow, e, dt, 0.12f, 0.12f);
        m_levelPhrase = follow(m_levelPhrase, e, dt, 0.33f, 0.33f); // ~3 s
        // Section energy: phrase average relative to the long-term average, squashed to 0..1.
        const float ratio = m_levelPhrase / std::max(m_levelSlow, 1e-3f);
        m_section = follow(m_section, std::clamp(0.5f + 0.5f * (ratio - 1.f), 0.f, 1.f), dt, 0.5f, 0.5f);
    }

    // Beat: bass energy jumping well above its recent average.
    m_bassAvg = follow(m_bassAvg, bassNow, dt, 2.f, 2.f);
    m_beatCooldown = std::max(0.f, m_beatCooldown - dt);
    if (!m_silent && m_beatCooldown <= 0.f && bassNow > 0.35f && bassNow > m_bassAvg * 1.35f) {
        m_beatEnv = 1.f;
        m_beatCooldown = 0.18f;
        ++m_beatCount;
    }
    m_beatEnv *= std::exp(-dt * 4.f);
    if (m_silent) {
        m_bass = follow(m_bass, 0.f, dt, 0.f, 3.f);
        m_mid = follow(m_mid, 0.f, dt, 0.f, 3.f);
        m_treble = follow(m_treble, 0.f, dt, 0.f, 3.f);
    }
}

} // namespace space::audio
