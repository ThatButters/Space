#pragma once
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace space::gfx {

class Context;

// A 2D GPU image + view allocated through VMA. Used for HDR render targets and textures.
struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};

    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;

    void create(const Context& ctx, uint32_t width, uint32_t height, VkFormat fmt, VkImageUsageFlags usage,
                VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT);
    void destroy(const Context& ctx);
};

// synchronization2 image layout transition helper. Stage/access masks are conservative.
void transitionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                     VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                     VkAccessFlags2 dstAccess, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT);

} // namespace space::gfx
