#include "core/Input.h"

namespace space {

void Input::beginFrame() {
    m_prevKeys = m_keys;
    m_pressed = {};
    m_prevMouse = m_mouse;
    m_mouseDelta = {};
    m_scroll = 0.f;
}

void Input::onKey(int key, bool down) {
    if (key < 0 || key >= (int)m_keys.size()) return;
    if (down && !m_keys[key]) m_pressed[key] = true;
    m_keys[key] = down;
}

void Input::onMouseButton(int button, bool down) {
    if (button >= 0 && button < (int)m_mouse.size()) m_mouse[button] = down;
}

void Input::onMouseMove(double x, double y) {
    if (m_haveLastMouse) m_mouseDelta += glm::vec2(x - m_lastMouse.x, y - m_lastMouse.y);
    m_lastMouse = {x, y};
    m_haveLastMouse = true;
}

void Input::onScroll(double, double dy) { m_scroll += (float)dy; }

} // namespace space
