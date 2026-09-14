// TexBake: offline texture baker for the Space explorer.
//   TexBake <in.jpg|png|raw> <out.dds> [--width W] [--height H] [--linear] [--bc1|--bc4|--bc5] [--cpu]
//           [--raw W H C]
// Decodes the image (or reads raw interleaved 8-bit samples with C = 1..4 channels), resamples it
// (sRGB-correct), builds a full mip chain and block-compresses it with DirectXTex: BC7 by default
// (GPU when a D3D11 device is available), BC1, BC4 (single channel: heights) or BC5 (two channels:
// normal maps). The output is a DX10 DDS the engine uploads directly.

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#include <DirectXTex.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: TexBake <in> <out.dds> [--width W] [--height H] [--linear] [--bc1|--bc4|--bc5] "
                             "[--cpu] [--raw W H C]\n");
        return 2;
    }
    const char* inPath = argv[1];
    const char* outPath = argv[2];
    int outW = 0, outH = 0, rawW = 0, rawH = 0, rawC = 0;
    bool srgb = true, cpu = false;
    enum class Fmt { BC7, BC1, BC4, BC5 } fmt = Fmt::BC7;
    for (int i = 3; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--width") && i + 1 < argc) outW = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--height") && i + 1 < argc) outH = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--linear")) srgb = false;
        else if (!std::strcmp(argv[i], "--bc1")) fmt = Fmt::BC1;
        else if (!std::strcmp(argv[i], "--bc4")) fmt = Fmt::BC4, srgb = false;
        else if (!std::strcmp(argv[i], "--bc5")) fmt = Fmt::BC5, srgb = false;
        else if (!std::strcmp(argv[i], "--cpu")) cpu = true;
        else if (!std::strcmp(argv[i], "--raw") && i + 3 < argc) {
            rawW = std::atoi(argv[++i]);
            rawH = std::atoi(argv[++i]);
            rawC = std::atoi(argv[++i]);
        }
    }

    const auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };

    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (rawW > 0) {
        if (rawC < 1 || rawC > 4) {
            std::fprintf(stderr, "--raw channels must be 1..4\n");
            return 2;
        }
        w = rawW;
        h = rawH;
        const size_t count = (size_t)w * h;
        std::vector<uint8_t> raw(count * rawC);
        std::ifstream f(inPath, std::ios::binary);
        f.read(reinterpret_cast<char*>(raw.data()), (std::streamsize)raw.size());
        if (!f) {
            std::fprintf(stderr, "raw read failed: %s (expected %zu bytes)\n", inPath, raw.size());
            return 1;
        }
        rgba.resize(count * 4);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* s = raw.data() + i * rawC;
            uint8_t* d = rgba.data() + i * 4;
            d[0] = s[0];
            d[1] = rawC >= 2 ? s[1] : s[0];
            d[2] = rawC >= 3 ? s[2] : (rawC == 2 ? 0 : s[0]);
            d[3] = rawC == 4 ? s[3] : 255;
        }
        std::printf("[%.1fs] read raw %s %dx%d x%d\n", elapsed(), inPath, w, h, rawC);
    } else {
        int comp = 0;
        stbi_uc* pixels = stbi_load(inPath, &w, &h, &comp, 4);
        if (!pixels) {
            std::fprintf(stderr, "decode failed: %s (%s)\n", inPath, stbi_failure_reason());
            return 1;
        }
        rgba.assign(pixels, pixels + (size_t)w * h * 4);
        stbi_image_free(pixels);
        std::printf("[%.1fs] decoded %s %dx%d\n", elapsed(), inPath, w, h);
    }

    if (outW <= 0) outW = w;
    if (outH <= 0) outH = h;
    if (outW != w || outH != h) {
        std::vector<uint8_t> resized((size_t)outW * outH * 4);
        const bool ok = srgb ? stbir_resize_uint8_srgb(rgba.data(), w, h, 0, resized.data(), outW, outH, 0, STBIR_RGBA) != nullptr
                             : stbir_resize_uint8_linear(rgba.data(), w, h, 0, resized.data(), outW, outH, 0, STBIR_RGBA) != nullptr;
        if (!ok) {
            std::fprintf(stderr, "resize failed\n");
            return 1;
        }
        rgba.swap(resized);
        std::printf("[%.1fs] resized to %dx%d\n", elapsed(), outW, outH);
    }

    using namespace DirectX;
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return 1;

    Image top{};
    top.width = (size_t)outW;
    top.height = (size_t)outH;
    top.format = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    top.rowPitch = (size_t)outW * 4;
    top.slicePitch = top.rowPitch * outH;
    top.pixels = rgba.data();

    // Own 2x2 box mip chain (DirectXTex's GenerateMipMaps refuses images wider than 16384).
    size_t mipCount = 1;
    for (size_t mw = (size_t)outW, mh = (size_t)outH; mw > 1 || mh > 1; mw = std::max<size_t>(mw / 2, 1), mh = std::max<size_t>(mh / 2, 1))
        ++mipCount;
    ScratchImage chain;
    hr = chain.Initialize2D(top.format, top.width, top.height, 1, mipCount);
    if (FAILED(hr)) {
        std::fprintf(stderr, "mip allocation failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    std::memcpy(chain.GetImage(0, 0, 0)->pixels, rgba.data(), rgba.size());
    std::vector<uint8_t>().swap(rgba);
    float toLinear[256];
    for (int i = 0; i < 256; ++i) {
        const float c = i / 255.f;
        toLinear[i] = srgb ? (c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f)) : c;
    }
    auto encode = [&](float lin) {
        float c = srgb ? (lin <= 0.0031308f ? lin * 12.92f : 1.055f * std::pow(lin, 1.f / 2.4f) - 0.055f) : lin;
        return (uint8_t)std::clamp(c * 255.f + 0.5f, 0.f, 255.f);
    };
    for (size_t level = 1; level < mipCount; ++level) {
        const Image* src = chain.GetImage(level - 1, 0, 0);
        const Image* dst = chain.GetImage(level, 0, 0);
        const size_t sw = src->width, sh = src->height;
        for (size_t y = 0; y < dst->height; ++y) {
            const size_t y0 = std::min(y * 2, sh - 1), y1 = std::min(y * 2 + 1, sh - 1);
            for (size_t x = 0; x < dst->width; ++x) {
                const size_t x0 = std::min(x * 2, sw - 1), x1 = std::min(x * 2 + 1, sw - 1);
                const uint8_t* a = src->pixels + y0 * src->rowPitch + x0 * 4;
                const uint8_t* b = src->pixels + y0 * src->rowPitch + x1 * 4;
                const uint8_t* c = src->pixels + y1 * src->rowPitch + x0 * 4;
                const uint8_t* d = src->pixels + y1 * src->rowPitch + x1 * 4;
                uint8_t* o = dst->pixels + y * dst->rowPitch + x * 4;
                for (int k = 0; k < 3; ++k)
                    o[k] = encode(0.25f * (toLinear[a[k]] + toLinear[b[k]] + toLinear[c[k]] + toLinear[d[k]]));
                o[3] = (uint8_t)((a[3] + b[3] + c[3] + d[3] + 2) / 4);
            }
        }
    }
    std::printf("[%.1fs] %zu mips\n", elapsed(), chain.GetImageCount());

    DXGI_FORMAT target = DXGI_FORMAT_BC7_UNORM;
    const char* label = "BC7";
    switch (fmt) {
    case Fmt::BC7: target = srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM; label = "BC7"; break;
    case Fmt::BC1: target = srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM; label = "BC1"; break;
    case Fmt::BC4: target = DXGI_FORMAT_BC4_UNORM; label = "BC4"; break;
    case Fmt::BC5: target = DXGI_FORMAT_BC5_UNORM; label = "BC5"; break;
    }
    ScratchImage compressed;
    ComPtr<ID3D11Device> device;
    if (!cpu && fmt == Fmt::BC7) { // DirectCompute only accelerates BC6H/BC7
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1, D3D11_SDK_VERSION,
                               device.GetAddressOf(), nullptr, nullptr);
        if (FAILED(hr)) device.Reset();
    }
    bool gpuOk = false;
    if (device) {
        hr = Compress(device.Get(), chain.GetImages(), chain.GetImageCount(), chain.GetMetadata(), target,
                      TEX_COMPRESS_DEFAULT, 1.0f, compressed);
        gpuOk = SUCCEEDED(hr);
        if (!gpuOk) std::fprintf(stderr, "GPU compress failed (0x%08lx), falling back to CPU\n", (unsigned long)hr);
    }
    if (!gpuOk) {
        hr = Compress(chain.GetImages(), chain.GetImageCount(), chain.GetMetadata(), target,
                      TEX_COMPRESS_PARALLEL, TEX_THRESHOLD_DEFAULT, compressed);
        if (FAILED(hr)) {
            std::fprintf(stderr, "compress failed: 0x%08lx\n", (unsigned long)hr);
            return 1;
        }
    }
    std::printf("[%.1fs] compressed to %s (%s)\n", elapsed(), label, gpuOk ? "GPU" : "CPU");

    const std::wstring wout(outPath, outPath + std::strlen(outPath));
    hr = SaveToDDSFile(compressed.GetImages(), compressed.GetImageCount(), compressed.GetMetadata(),
                       DDS_FLAGS_FORCE_DX10_EXT, wout.c_str());
    if (FAILED(hr)) {
        std::fprintf(stderr, "save failed: 0x%08lx\n", (unsigned long)hr);
        return 1;
    }
    std::printf("[%.1fs] wrote %s (%.1f MB)\n", elapsed(), outPath, compressed.GetPixelsSize() / 1048576.0);
    return 0;
}
