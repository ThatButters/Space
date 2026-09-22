#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace space::audio {

// Plays short mono clips and one continuous trickle through the default output device (WASAPI shared
// mode), mixed on its own thread. Clips are made at sampleRate(), which is known once start() returns.
class SoundPlayer {
public:
    using Clip = std::shared_ptr<const std::vector<float>>;

    ~SoundPlayer();
    bool start();
    void stop();
    bool running() const { return m_running; }
    unsigned sampleRate() const { return m_sampleRate; }

    void play(const Clip& clip, float gain = 1.f, float pitch = 1.f);
    // A running stream of water (fades in and out, never clicks).
    void setTrickle(bool on) { m_trickle = on; }

private:
    void threadMain();
    struct Voice {
        Clip clip;
        double pos = 0.0;
        float gain = 1.f, pitch = 1.f;
    };
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_trickle{false};
    std::atomic<unsigned> m_sampleRate{48000};
    std::mutex m_mutex;
    std::vector<Voice> m_voices;
    std::atomic<bool> m_ready{false};
};

} // namespace space::audio
