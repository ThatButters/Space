#pragma once
#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace space::audio {

// Captures whatever the system is playing (WASAPI loopback on the default render device) into a
// mono float ring buffer. Everything device-related lives on the capture thread.
class AudioCapture {
public:
    ~AudioCapture();

    bool start();
    void stop();
    bool running() const { return m_running; }
    unsigned sampleRate() const { return m_sampleRate; }
    const std::string& deviceName() const { return m_deviceName; }

    // Copies the most recent `count` mono samples (zero-padded if fewer are available).
    void latest(float* out, size_t count) const;

private:
    void threadMain();
    void push(const float* mono, size_t count);

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stop{false};
    unsigned m_sampleRate = 48000;
    std::string m_deviceName;

    mutable std::mutex m_mutex;
    std::vector<float> m_ring;
    size_t m_write = 0;
};

} // namespace space::audio
