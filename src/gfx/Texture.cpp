#include "gfx/Texture.h"
#include "gfx/Buffer.h"
#include "gfx/Context.h"
#include "gfx/VkCheck.h"
#include "core/Log.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

namespace space::gfx {

DecodedImage decodeImage(const std::filesystem::path& path) {
    DecodedImage out;
    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load(path.string().c_str(), &w, &h, &comp, 4);
    if (!pixels) {
        LOG_WARN("Texture {} failed to load: {}", path.string(), stbi_failure_reason());
        return out;
    }
    out.width = w;
    out.height = h;
    out.rgba.assign(pixels, pixels + (size_t)w * h * 4);
    stbi_image_free(pixels);
    return out;
}

DdsImage loadDds(const std::filesystem::path& path) {
    DdsImage out;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        LOG_WARN("Texture {} not found", path.string());
        return out;
    }
    uint8_t header[4 + 124 + 20];
    f.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!f || std::memcmp(header, "DDS ", 4) != 0) {
        LOG_WARN("Texture {}: not a DDS file", path.string());
        return out;
    }
    auto u32 = [&](size_t off) { uint32_t v; std::memcpy(&v, header + off, 4); return v; };
    const uint32_t height = u32(4 + 8), width = u32(4 + 12), mipCount = std::max(1u, u32(4 + 24));
    const uint32_t pfFlags = u32(4 + 76); // DDS_PIXELFORMAT starts 72 bytes into the header
    const bool dx10 = (pfFlags & 0x4) && std::memcmp(header + 4 + 80, "DX10", 4) == 0;
    if (!dx10) {
        LOG_WARN("Texture {}: only DX10-header DDS is supported (bake with TexBake)", path.string());
        return out;
    }
    const uint32_t dxgi = u32(4 + 124);
    switch (dxgi) {
    case 71: out.format = VK_FORMAT_BC1_RGB_UNORM_BLOCK; out.blockBytes = 8; break;
    case 72: out.format = VK_FORMAT_BC1_RGB_SRGB_BLOCK; out.blockBytes = 8; break;
    case 77: out.format = VK_FORMAT_BC3_UNORM_BLOCK; out.blockBytes = 16; break;
    case 78: out.format = VK_FORMAT_BC3_SRGB_BLOCK; out.blockBytes = 16; break;
    case 80: out.format = VK_FORMAT_BC4_UNORM_BLOCK; out.blockBytes = 8; break;
    case 83: out.format = VK_FORMAT_BC5_UNORM_BLOCK; out.blockBytes = 16; break;
    case 98: out.format = VK_FORMAT_BC7_UNORM_BLOCK; out.blockBytes = 16; break;
    case 99: out.format = VK_FORMAT_BC7_SRGB_BLOCK; out.blockBytes = 16; break;
    case 56: out.format = VK_FORMAT_R16_UNORM; out.texelBytes = 2; break;
    case 61: out.format = VK_FORMAT_R8_UNORM; out.texelBytes = 1; break;
    default:
        LOG_WARN("Texture {}: unsupported DXGI format {}", path.string(), dxgi);
        return out;
    }
    size_t total = 0;
    uint32_t w = width, h = height;
    for (uint32_t i = 0; i < mipCount; ++i) {
        out.mipOffsets.push_back(total);
        total += out.texelBytes ? (size_t)w * h * out.texelBytes : (size_t)((w + 3) / 4) * ((h + 3) / 4) * out.blockBytes;
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
    }
    out.data.resize(total);
    f.read(reinterpret_cast<char*>(out.data.data()), (std::streamsize)total);
    if (!f) {
        LOG_WARN("Texture {}: truncated ({} bytes expected)", path.string(), total);
        return DdsImage{};
    }
    out.width = (int)width;
    out.height = (int)height;
    out.mipLevels = mipCount;
    return out;
}

bool Texture::uploadDds(Context& ctx, const DdsImage& dds, const char* debugName) {
    if (!dds.ok()) return false;
    mipLevels = dds.mipLevels;
    image.format = dds.format;
    image.extent = {(uint32_t)dds.width, (uint32_t)dds.height};
    image.aspect = VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = image.format;
    ci.extent = {(uint32_t)dds.width, (uint32_t)dds.height, 1};
    ci.mipLevels = mipLevels;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(vmaCreateImage(ctx.allocator(), &ci, &ai, &image.image, &image.allocation, nullptr));

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = image.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};
    VK_CHECK(vkCreateImageView(ctx.device(), &vi, nullptr, &image.view));

    Buffer staging;
    staging.create(ctx, dds.data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    void* mapped = nullptr;
    VK_CHECK(vmaMapMemory(ctx.allocator(), staging.allocation, &mapped));
    std::memcpy(mapped, dds.data.data(), dds.data.size());
    vmaFlushAllocation(ctx.allocator(), staging.allocation, 0, VK_WHOLE_SIZE);
    vmaUnmapMemory(ctx.allocator(), staging.allocation);

    VkCommandBuffer cmd = ctx.beginOneShot();
    auto barrier = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                       VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    };
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    std::vector<VkBufferImageCopy> regions;
    uint32_t w = (uint32_t)dds.width, h = (uint32_t)dds.height;
    for (uint32_t i = 0; i < mipLevels; ++i) {
        VkBufferImageCopy r{};
        r.bufferOffset = dds.mipOffsets[i];
        r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        r.imageExtent = {w, h, 1};
        regions.push_back(r);
        w = std::max(w / 2, 1u);
        h = std::max(h / 2, 1u);
    }
    vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           (uint32_t)regions.size(), regions.data());
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    ctx.endOneShot(cmd);
    staging.destroy(ctx);
    LOG_INFO("Texture {} {}x{} ({} mips, compressed {:.0f} MB)", debugName, dds.width, dds.height, mipLevels,
             dds.data.size() / 1048576.0);
    return true;
}

bool Texture::upload(Context& ctx, const DecodedImage& decoded, bool srgb, const char* debugName) {
    if (!decoded.ok()) return false;
    const int w = decoded.width, h = decoded.height;
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    mipLevels = 1 + (uint32_t)std::floor(std::log2((double)std::max(w, h)));

    // Image with the full chain; blits generate the mips on the GPU.
    image.format = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    image.extent = {(uint32_t)w, (uint32_t)h};
    image.aspect = VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = image.format;
    ci.extent = {(uint32_t)w, (uint32_t)h, 1};
    ci.mipLevels = mipLevels;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VK_CHECK(vmaCreateImage(ctx.allocator(), &ci, &ai, &image.image, &image.allocation, nullptr));

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = image.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, 1};
    VK_CHECK(vkCreateImageView(ctx.device(), &vi, nullptr, &image.view));

    Buffer staging;
    staging.create(ctx, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_AUTO,
                   VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
    void* mapped = nullptr;
    VK_CHECK(vmaMapMemory(ctx.allocator(), staging.allocation, &mapped));
    std::memcpy(mapped, decoded.rgba.data(), (size_t)bytes);
    vmaFlushAllocation(ctx.allocator(), staging.allocation, 0, VK_WHOLE_SIZE);
    vmaUnmapMemory(ctx.allocator(), staging.allocation);

    VkCommandBuffer cmd = ctx.beginOneShot();

    auto barrier = [&](uint32_t mip, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
                       VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage;
        b.srcAccessMask = srcAccess;
        b.dstStageMask = dstStage;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd, &dep);
    };

    // Level 0: upload.
    barrier(0, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
            VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Levels 1..n: blit down from the previous level.
    int32_t mw = w, mh = h;
    for (uint32_t i = 1; i < mipLevels; ++i) {
        barrier(i - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        barrier(i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
                VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        int32_t nw = std::max(mw / 2, 1), nh = std::max(mh / 2, 1);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
        blit.srcOffsets[1] = {mw, mh, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        blit.dstOffsets[1] = {nw, nh, 1};
        vkCmdBlitImage(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        barrier(i - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        mw = nw;
        mh = nh;
    }
    barrier(mipLevels - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    ctx.endOneShot(cmd);
    staging.destroy(ctx);
    LOG_INFO("Texture {} {}x{} ({} mips)", debugName, w, h, mipLevels);
    return true;
}

} // namespace space::gfx
