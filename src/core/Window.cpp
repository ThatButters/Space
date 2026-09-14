#include "core/Window.h"
#include "core/Input.h"
#include "core/Log.h"

#include <GLFW/glfw3.h>
#include <stdexcept>

namespace space {

Window::Window(int width, int height, const std::string& title, Input& input) : m_input(input) {
    glfwSetErrorCallback([](int code, const char* msg) { LOG_ERROR("GLFW error {}: {}", code, msg); });
    if (!glfwInit()) throw std::runtime_error("glfwInit failed");

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
    m_window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
    if (!m_window) throw std::runtime_error("glfwCreateWindow failed");

    glfwSetWindowUserPointer(m_window, this);
    glfwSetFramebufferSizeCallback(m_window, [](GLFWwindow* w, int, int) {
        static_cast<Window*>(glfwGetWindowUserPointer(w))->m_resized = true;
    });
    glfwSetKeyCallback(m_window, [](GLFWwindow* w, int key, int, int action, int) {
        if (action == GLFW_REPEAT) return;
        static_cast<Window*>(glfwGetWindowUserPointer(w))->m_input.onKey(key, action == GLFW_PRESS);
    });
    glfwSetMouseButtonCallback(m_window, [](GLFWwindow* w, int button, int action, int) {
        static_cast<Window*>(glfwGetWindowUserPointer(w))->m_input.onMouseButton(button, action == GLFW_PRESS);
    });
    glfwSetCursorPosCallback(m_window, [](GLFWwindow* w, double x, double y) {
        static_cast<Window*>(glfwGetWindowUserPointer(w))->m_input.onMouseMove(x, y);
    });
    glfwSetScrollCallback(m_window, [](GLFWwindow* w, double dx, double dy) {
        static_cast<Window*>(glfwGetWindowUserPointer(w))->m_input.onScroll(dx, dy);
    });
}

Window::~Window() {
    if (m_window) glfwDestroyWindow(m_window);
    glfwTerminate();
}

bool Window::shouldClose() const { return glfwWindowShouldClose(m_window); }
void Window::pollEvents() { glfwPollEvents(); }
void Window::waitEvents() { glfwWaitEvents(); }

void Window::framebufferSize(int& w, int& h) const { glfwGetFramebufferSize(m_window, &w, &h); }

void Window::setCursorCaptured(bool captured) {
    if (captured == m_captured) return;
    m_captured = captured;
    glfwSetInputMode(m_window, GLFW_CURSOR, captured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    if (glfwRawMouseMotionSupported())
        glfwSetInputMode(m_window, GLFW_RAW_MOUSE_MOTION, captured ? GLFW_TRUE : GLFW_FALSE);
    m_input.resetMouseDelta();
}

std::vector<const char*> Window::requiredInstanceExtensions() const {
    uint32_t count = 0;
    const char** exts = glfwGetRequiredInstanceExtensions(&count);
    return {exts, exts + count};
}

VkSurfaceKHR Window::createSurface(VkInstance instance) const {
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (glfwCreateWindowSurface(instance, m_window, nullptr, &surface) != VK_SUCCESS)
        throw std::runtime_error("glfwCreateWindowSurface failed");
    return surface;
}

} // namespace space
