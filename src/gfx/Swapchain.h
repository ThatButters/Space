#pragma once
#include <vulkan/vulkan.h>
#include <vector>

namespace space::gfx {

class Context;

class Swapchain {
public:
    // preferHdr: pick a scRGB (RGBA16F, extended linear sRGB) surface when the display offers one.
    void create(const Context& ctx, uint32_t width, uint32_t height, bool vsync, bool preferHdr);
    void destroy(const Context& ctx);

    // True if the current surface is scRGB linear: shaders write linear light where 1.0 = 80 nits.
    bool isHdr() const { return m_colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT; }
    // True if the display exposes an HDR surface at all (checked at creation).
    bool hdrAvailable() const { return m_hdrAvailable; }

    VkSwapchainKHR handle() const { return m_swapchain; }
    VkFormat format() const { return m_format; }
    VkColorSpaceKHR colorSpace() const { return m_colorSpace; }
    VkExtent2D extent() const { return m_extent; }
    uint32_t imageCount() const { return (uint32_t)m_images.size(); }
    VkImage image(uint32_t i) const { return m_images[i]; }
    VkImageView view(uint32_t i) const { return m_views[i]; }
    bool vsync() const { return m_vsync; }

private:
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR m_colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    bool m_hdrAvailable = false;
    VkExtent2D m_extent{};
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_views;
    bool m_vsync = true;
};

} // namespace space::gfx
