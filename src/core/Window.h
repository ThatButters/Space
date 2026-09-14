#pragma once
#include <vulkan/vulkan.h>
#include <string>
#include <vector>

struct GLFWwindow;

namespace space {

class Input;

class Window {
public:
    Window(int width, int height, const std::string& title, Input& input);
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    GLFWwindow* handle() const { return m_window; }
    bool shouldClose() const;
    void pollEvents();
    void waitEvents();

    // Framebuffer size in pixels (may be 0x0 when minimised).
    void framebufferSize(int& w, int& h) const;
    bool wasResized() const { return m_resized; }
    void clearResized() { m_resized = false; }

    void setCursorCaptured(bool captured);
    bool cursorCaptured() const { return m_captured; }

    std::vector<const char*> requiredInstanceExtensions() const;
    VkSurfaceKHR createSurface(VkInstance instance) const;

private:
    GLFWwindow* m_window = nullptr;
    Input& m_input;
    bool m_resized = false;
    bool m_captured = false;
};

} // namespace space
