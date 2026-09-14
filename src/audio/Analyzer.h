#pragma once
#include <array>
#include <cstddef>
#include <vector>

namespace space::audio {

class AudioCapture;

// Turns the capture ring buffer into a handful of smoothed, auto-gained music features per frame.
class Analyzer {
public:
    static constexpr size_t kFft = 2048;
    static constexpr int kBands = 32;

    void update(const AudioCapture& capture, float dt);

    // All in [0, 1] after auto-gain, smoothed with fast attack / slow release.
    float level() const { return m_level; }
    float bass() const { return m_bass; }
    float mid() const { return m_mid; }
    float treble() const { return m_treble; }
    // Beat envelope: jumps to 1 on a detected kick and decays over ~0.3 s.
    float beat() const { return m_beatEnv; }
    bool silent() const { return m_silent; }
    unsigned beatCount() const { return m_beatCount; }
    // Spectral centroid over the log bands, 0 = all bass, 1 = all treble; slow-smoothed.
    float centroid() const { return m_centroid; }
    // Phrase-scale energy: how the last few seconds compare with the last half minute, 0..1 (0.5 = typical).
    float section() const { return m_section; }
    // Level smoothed over ~0.6 s and ~8 s, for rare "swell" detection.
    float levelFast() const { return m_levelFast; }
    float levelSlow() const { return m_levelSlow; }
    const std::array<float, kBands>& bands() const { return m_bands; }

private:
    std::vector<float> m_samples, m_re, m_im, m_window;
    std::array<float, kBands> m_bands{};
    std::array<float, kBands> m_bandPeak{}; // running peak per band for auto-gain
    float m_level = 0.f, m_bass = 0.f, m_mid = 0.f, m_treble = 0.f, m_beatEnv = 0.f;
    float m_bassAvg = 0.f, m_beatCooldown = 0.f;
    unsigned m_beatCount = 0;
    float m_centroid = 0.5f;
    float m_section = 0.5f, m_levelFast = 0.f, m_levelSlow = 0.f, m_levelPhrase = 0.f;
    bool m_silent = true;
};

} // namespace space::audio
