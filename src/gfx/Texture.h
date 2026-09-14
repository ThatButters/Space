#pragma once
#include "gfx/Image.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace space::gfx {

class Context;

// CPU-side decoded RGBA8 image (decode is thread-safe; upload must happen on the main thread).
struct DecodedImage {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    bool srgb = true; // colour data; false for linear data (normal / metallic-roughness / occlusion maps)
    bool ok() const { return width > 0 && !rgba.empty(); }
};

DecodedImage decodeImage(const std::filesystem::path& path);

// Pre-baked block-compressed image (DX10 DDS from tools/texbake): every mip level in one blob.
struct DdsImage {
    int width = 0, height = 0;
    uint32_t mipLevels = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t blockBytes = 0;   // block-compressed formats
    uint32_t texelBytes = 0;   // uncompressed formats (R8, R16)
    std::vector<uint8_t> data;      // mip 0 first, tightly packed
    std::vector<size_t> mipOffsets; // byte offset of each level in data
    bool ok() const { return width > 0 && mipLevels > 0 && !data.empty(); }
};

// Parses a DX10 DDS holding BC1/BC3/BC4/BC5/BC7, R8 or R16 data (thread-safe; upload on the main thread).
DdsImage loadDds(const std::filesystem::path& path);

// 2D texture with a full mip chain.
struct Texture {
    Image image;
    uint32_t mipLevels = 1;

    // srgb: colour data (decoded by the sampler); otherwise linear (masks, normals, alpha).
    bool upload(Context& ctx, const DecodedImage& decoded, bool srgb, const char* debugName = "");
    // Uploads a pre-compressed, pre-mipped image as-is.
    bool uploadDds(Context& ctx, const DdsImage& dds, const char* debugName = "");
    bool load(Context& ctx, const std::filesystem::path& path, bool srgb) {
        return upload(ctx, decodeImage(path), srgb, path.filename().string().c_str());
    }
    void destroy(const Context& ctx) { image.destroy(ctx); }
};

} // namespace space::gfx
