#include "gfx/Buffer.h"
#include "gfx/Context.h"
#include "gfx/VkCheck.h"

#include <cstring>

namespace space::gfx {

void Buffer::create(const Context& ctx, VkDeviceSize bytes, VkBufferUsageFlags usage, VmaMemoryUsage memUsage,
                    VmaAllocationCreateFlags flags) {
    size = bytes;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = bytes;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo ai{};
    ai.usage = memUsage;
    ai.flags = flags;
    VK_CHECK(vmaCreateBuffer(ctx.allocator(), &ci, &ai, &buffer, &allocation, nullptr));
}

void Buffer::destroy(const Context& ctx) {
    if (buffer) vmaDestroyBuffer(ctx.allocator(), buffer, allocation);
    *this = Buffer{};
}

Buffer createDeviceLocalBuffer(Context& ctx, const void* data, VkDeviceSize bytes, VkBufferUsageFlags usage) {
    Buffer staging;
    staging.create(ctx, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    void* mapped = nullptr;
    VK_CHECK(vmaMapMemory(ctx.allocator(), staging.allocation, &mapped));
    std::memcpy(mapped, data, (size_t)bytes);
    vmaFlushAllocation(ctx.allocator(), staging.allocation, 0, VK_WHOLE_SIZE); // no-op on coherent memory
    vmaUnmapMemory(ctx.allocator(), staging.allocation);

    Buffer result;
    result.create(ctx, bytes, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE);

    VkCommandBuffer cmd = ctx.beginOneShot();
    VkBufferCopy region{0, 0, bytes};
    vkCmdCopyBuffer(cmd, staging.buffer, result.buffer, 1, &region);
    ctx.endOneShot(cmd);

    staging.destroy(ctx);
    return result;
}

} // namespace space::gfx
