#pragma once
#include <array>
#include <glm/glm.hpp>

namespace space {

// Polled keyboard/mouse state for the current frame. Fed by Window callbacks.
class Input {
public:
    void beginFrame();

    void onKey(int key, bool down);
    void onMouseButton(int button, bool down);
    void onMouseMove(double x, double y);
    void onScroll(double dx, double dy);

    bool keyDown(int key) const { return m_keys[key]; }
    bool keyPressed(int key) const { return m_keys[key] && !m_prevKeys[key]; }
    bool mouseDown(int button) const { return m_mouse[button]; }
    bool mousePressed(int button) const { return m_mouse[button] && !m_prevMouse[button]; }
    bool mouseReleased(int button) const { return !m_mouse[button] && m_prevMouse[button]; }

    glm::vec2 mouseDelta() const { return m_mouseDelta; }
    float scrollDelta() const { return m_scroll; }

    // Discard the next delta so a capture toggle does not produce a jump.
    void resetMouseDelta() { m_haveLastMouse = false; m_mouseDelta = {}; }

private:
    std::array<bool, 512> m_keys{}, m_prevKeys{};
    std::array<bool, 8> m_mouse{}, m_prevMouse{};
    glm::vec2 m_mouseDelta{};
    glm::dvec2 m_lastMouse{};
    bool m_haveLastMouse = false;
    float m_scroll = 0.f;
};

} // namespace space
