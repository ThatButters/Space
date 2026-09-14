#pragma once
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace space::gfx {

class Context;

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceSize size = 0;

    void create(const Context& ctx, VkDeviceSize bytes, VkBufferUsageFlags usage, VmaMemoryUsage memUsage,
                VmaAllocationCreateFlags flags = 0);
    void destroy(const Context& ctx);
};

// Creates a device-local buffer and fills it from host memory through a staging buffer.
Buffer createDeviceLocalBuffer(Context& ctx, const void* data, VkDeviceSize bytes, VkBufferUsageFlags usage);

} // namespace space::gfx
