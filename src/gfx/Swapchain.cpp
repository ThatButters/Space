#include "gfx/Swapchain.h"
#include "gfx/Context.h"
#include "gfx/VkCheck.h"
#include "core/Log.h"

#include <algorithm>

namespace space::gfx {

void Swapchain::create(const Context& ctx, uint32_t width, uint32_t height, bool vsync, bool preferHdr) {
    VkPhysicalDevice pd = ctx.physicalDevice();
    VkSurfaceKHR surface = ctx.surface();

    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps));

    uint32_t fcount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &fcount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fcount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &fcount, formats.data());

    // HDR displays expose scRGB (RGBA16F + extended linear sRGB). Presenting to it directly gives real
    // highlights on the panel and, importantly, stops Windows Auto HDR from re-tonemapping our SDR output.
    // Otherwise prefer an sRGB swapchain so shaders write linear light and the hardware encodes.
    m_hdrAvailable = false;
    for (auto& f : formats)
        if (f.format == VK_FORMAT_R16G16B16A16_SFLOAT && f.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT)
            m_hdrAvailable = true;

    VkSurfaceFormatKHR chosen = formats[0];
    if (preferHdr && m_hdrAvailable) {
        chosen = {VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT};
    } else {
        for (auto& f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_SRGB && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) chosen = f;
    }
    m_colorSpace = chosen.colorSpace;

    uint32_t pcount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &pcount, nullptr);
    std::vector<VkPresentModeKHR> modes(pcount);
    vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &pcount, modes.data());

    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR; // always available, vsync
    if (!vsync) {
        for (auto m : modes)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) mode = m;
        if (mode == VK_PRESENT_MODE_FIFO_KHR)
            for (auto m : modes)
                if (m == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = m;
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    uint32_t imageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0) imageCount = std::min(imageCount, caps.maxImageCount);

    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = imageCount;
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT; // readback for screenshots / diagnostics
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSwapchainKHR(ctx.device(), &ci, nullptr, &m_swapchain));

    m_format = chosen.format;
    m_extent = extent;
    m_vsync = vsync;

    uint32_t count = 0;
    vkGetSwapchainImagesKHR(ctx.device(), m_swapchain, &count, nullptr);
    m_images.resize(count);
    vkGetSwapchainImagesKHR(ctx.device(), m_swapchain, &count, m_images.data());

    m_views.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = m_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = m_format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(ctx.device(), &vi, nullptr, &m_views[i]));
    }

    const char* modeName = mode == VK_PRESENT_MODE_FIFO_KHR      ? "vsync"
                           : mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox"
                                                                 : "immediate";
    LOG_INFO("Swapchain {}x{} x{} images, {}, {} output{}", extent.width, extent.height, count, modeName,
             isHdr() ? "HDR scRGB" : "SDR sRGB", m_hdrAvailable && !isHdr() ? " (HDR available)" : "");
}

void Swapchain::destroy(const Context& ctx) {
    for (auto v : m_views) vkDestroyImageView(ctx.device(), v, nullptr);
    m_views.clear();
    m_images.clear();
    if (m_swapchain) vkDestroySwapchainKHR(ctx.device(), m_swapchain, nullptr);
    m_swapchain = VK_NULL_HANDLE;
}

} // namespace space::gfx
