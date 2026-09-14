#include "gfx/Pipeline.h"
#include "gfx/Context.h"
#include "gfx/VkCheck.h"
#include "core/Log.h"

#include <fstream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#endif

namespace space::gfx {

std::filesystem::path shaderDirectory() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path() / "shaders";
#else
    return std::filesystem::current_path() / "shaders";
#endif
}

VkShaderModule loadShaderModule(const Context& ctx, const std::filesystem::path& spvPath) {
    std::ifstream file(spvPath, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot open shader " + spvPath.string());
    size_t size = (size_t)file.tellg();
    std::vector<uint32_t> code((size + 3) / 4);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(code.data()), size);

    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size;
    ci.pCode = code.data();
    VkShaderModule mod;
    VK_CHECK(vkCreateShaderModule(ctx.device(), &ci, nullptr, &mod));
    return mod;
}

VkPipelineLayout createPipelineLayout(const Context& ctx, const std::vector<VkDescriptorSetLayout>& sets,
                                      uint32_t pushConstantBytes, VkShaderStageFlags pushStages) {
    VkPushConstantRange range{pushStages, 0, pushConstantBytes};
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = (uint32_t)sets.size();
    ci.pSetLayouts = sets.data();
    ci.pushConstantRangeCount = pushConstantBytes ? 1 : 0;
    ci.pPushConstantRanges = &range;
    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &ci, nullptr, &layout));
    return layout;
}

VkPipeline createComputePipeline(const Context& ctx, const char* computeShader, VkPipelineLayout layout) {
    VkShaderModule mod = loadShaderModule(ctx, shaderDirectory() / computeShader);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = mod;
    ci.stage.pName = "main";
    ci.layout = layout;
    VkPipeline pipeline;
    VK_CHECK(vkCreateComputePipelines(ctx.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline));
    vkDestroyShaderModule(ctx.device(), mod, nullptr);
    return pipeline;
}

VkPipeline createFullscreenPipeline(const Context& ctx, const FullscreenPipelineDesc& desc) {
    GraphicsPipelineDesc d;
    d.fragmentShader = desc.fragmentShader;
    d.colorFormat = desc.colorFormat;
    d.layout = desc.layout;
    return createGraphicsPipeline(ctx, d);
}

VkPipeline createGraphicsPipeline(const Context& ctx, const GraphicsPipelineDesc& desc) {
    auto dir = shaderDirectory();
    VkShaderModule vert = loadShaderModule(ctx, dir / desc.vertexShader);
    VkShaderModule frag = loadShaderModule(ctx, dir / desc.fragmentShader);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkVertexInputBindingDescription vbind{0, sizeof(float) * 4, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription vattr{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkVertexInputBindingDescription mbind{0, sizeof(float) * 8, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription mattr[3] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                  {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
                                                  {2, 0, VK_FORMAT_R32G32_SFLOAT, 24}};
    if (desc.vertexInputVec4) {
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &vbind;
        vertexInput.vertexAttributeDescriptionCount = 1;
        vertexInput.pVertexAttributeDescriptions = &vattr;
    } else if (desc.vertexInputModel) {
        vertexInput.vertexBindingDescriptionCount = 1;
        vertexInput.pVertexBindingDescriptions = &mbind;
        vertexInput.vertexAttributeDescriptionCount = 3;
        vertexInput.pVertexAttributeDescriptions = mattr;
    }

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = desc.lines ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = desc.cullBack ? VK_CULL_MODE_BACK_BIT : (desc.cullFront ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE);
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.f;

    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = desc.depthTest ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = desc.depthWrite ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // reversed-Z

    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (desc.blend != BlendMode::Opaque) {
        blendAttachment.blendEnable = VK_TRUE;
        blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstColorBlendFactor =
            desc.blend == BlendMode::Additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
        blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = desc.depthOnly ? 0 : 1;
    blend.pAttachments = &blendAttachment;

    VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamics;

    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = desc.depthOnly ? 0 : 1;
    rendering.pColorAttachmentFormats = &desc.colorFormat;
    rendering.depthAttachmentFormat = desc.depthFormat;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rendering;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vertexInput;
    ci.pInputAssemblyState = &inputAssembly;
    ci.pViewportState = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &multisample;
    ci.pDepthStencilState = &depth;
    ci.pColorBlendState = &blend;
    ci.pDynamicState = &dynamic;
    ci.layout = desc.layout;

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline));

    vkDestroyShaderModule(ctx.device(), vert, nullptr);
    vkDestroyShaderModule(ctx.device(), frag, nullptr);
    return pipeline;
}

} // namespace space::gfx
