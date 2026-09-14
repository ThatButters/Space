#pragma once
#include "gfx/Texture.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace space::gfx {

// CPU-side glTF model: flattened into one vertex/index list with per-primitive materials.
struct ModelVertex {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};

// Image indices point into ModelData::images; -1 = none. All textures use TEXCOORD_0 (ModelVertex::uv).
struct ModelPrimitive {
    uint32_t firstIndex = 0, indexCount = 0;
    glm::vec4 baseColor{1.f};
    int imageIndex = -1; // base colour texture (sRGB)
    float metallic = 0.f, roughness = 0.7f;
    int normalImageIndex = -1;    // tangent-space normal map (linear)
    int mrImageIndex = -1;        // metallic (B) / roughness (G) map (linear), multiplied by the factors above
    int occlusionImageIndex = -1; // ambient occlusion in R (linear)
    int emissiveImageIndex = -1;  // emissive colour (sRGB), multiplied by emissiveFactor
    float normalScale = 1.f;
    float occlusionStrength = 1.f;
    glm::vec3 emissiveFactor{0.f}; // includes KHR_materials_emissive_strength
    glm::vec4 uvTransform{0.f, 0.f, 1.f, 1.f}; // KHR_texture_transform of the base colour texture: offset.xy, scale.xy
};

struct ModelData {
    std::vector<ModelVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<ModelPrimitive> primitives;
    std::vector<DecodedImage> images; // textures; DecodedImage::srgb tells colour from linear data
    glm::vec3 boundsMin{0.f}, boundsMax{0.f};
    float extent() const { return glm::length(boundsMax - boundsMin); }
};

// Loads a .glb or .gltf (external .bin / image files resolved relative to the .gltf, or data: URIs) with the
// full node hierarchy baked into world space (glTF: +Y up, metres usually).
bool loadModel(const std::filesystem::path& path, ModelData& out);

inline bool loadGlb(const std::filesystem::path& path, ModelData& out) { return loadModel(path, out); }

} // namespace space::gfx
