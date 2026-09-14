#pragma once
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>
#include <cstdint>
#include <string>

namespace space {
class Window;
}

namespace space::gfx {

// Owns the Vulkan instance, physical/logical device, single graphics+present queue and the VMA allocator.
// Targets Vulkan 1.3 with dynamic rendering + synchronization2 (no render passes anywhere in the engine).
class Context {
public:
    void init(const Window& window, bool enableValidation);
    void shutdown();

    VkInstance instance() const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physicalDevice; }
    VkDevice device() const { return m_device; }
    VkSurfaceKHR surface() const { return m_surface; }
    VkQueue queue() const { return m_queue; }
    uint32_t queueFamily() const { return m_queueFamily; }
    VmaAllocator allocator() const { return m_allocator; }
    const VkPhysicalDeviceProperties& properties() const { return m_props; }
    std::string gpuName() const { return m_props.deviceName; }

    void waitIdle() const;

    // Immediate one-shot command buffer (uploads, layout transitions outside the frame loop).
    VkCommandBuffer beginOneShot();
    void endOneShot(VkCommandBuffer cmd);

private:
    void createInstance(const Window& window, bool enableValidation);
    void pickPhysicalDevice();
    void createDevice();

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties m_props{};
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VmaAllocator m_allocator = VK_NULL_HANDLE;
    VkCommandPool m_oneShotPool = VK_NULL_HANDLE;
};

} // namespace space::gfx
