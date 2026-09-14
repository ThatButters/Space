#pragma once
#include <vulkan/vulkan.h>
#include <filesystem>
#include <vector>

namespace space::gfx {

class Context;

// Loads a SPIR-V blob from disk (paths are resolved relative to the executable's shaders/ directory).
VkShaderModule loadShaderModule(const Context& ctx, const std::filesystem::path& spvPath);

// Directory the compiled .spv files live in, next to the executable.
std::filesystem::path shaderDirectory();

enum class BlendMode {
    Opaque,
    Additive,          // dst = src + dst              (emissive point sprites)
    PremultipliedOver, // dst = src.rgb + src.a * dst  (volumetrics: rgb = in-scatter, a = transmittance)
};

struct GraphicsPipelineDesc {
    const char* vertexShader = "fullscreen.vert.spv"; // file name inside shaders/
    const char* fragmentShader = nullptr;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED; // must match the pass's depth attachment if it has one
    VkPipelineLayout layout = VK_NULL_HANDLE;
    BlendMode blend = BlendMode::Opaque;
    bool vertexInputVec4 = false; // binding 0: one vec4 per vertex (mesh passes); otherwise no vertex input
    bool vertexInputModel = false; // binding 0: ModelVertex (vec3 pos, vec3 normal, vec2 uv)
    bool depthTest = false;       // reversed-Z: GREATER_OR_EQUAL
    bool depthWrite = false;
    bool cullBack = false;
    bool cullFront = false;       // proxy geometry for ray-traced surfaces: draw the far side only
    bool lines = false;           // LINE_LIST topology instead of triangles
    bool depthOnly = false;       // no colour attachment (shadow maps)
};

// Graphics pipeline with dynamic viewport + scissor and dynamic rendering.
VkPipeline createGraphicsPipeline(const Context& ctx, const GraphicsPipelineDesc& desc);

struct FullscreenPipelineDesc {
    const char* fragmentShader = nullptr; // file name inside shaders/, e.g. "sky.frag.spv"
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    VkPipelineLayout layout = VK_NULL_HANDLE;
};

// Graphics pipeline that draws one fullscreen triangle (no vertex buffers) with dynamic rendering.
VkPipeline createFullscreenPipeline(const Context& ctx, const FullscreenPipelineDesc& desc);

VkPipeline createComputePipeline(const Context& ctx, const char* computeShader, VkPipelineLayout layout);

VkPipelineLayout createPipelineLayout(const Context& ctx, const std::vector<VkDescriptorSetLayout>& sets,
                                      uint32_t pushConstantBytes, VkShaderStageFlags pushStages);

} // namespace space::gfx
