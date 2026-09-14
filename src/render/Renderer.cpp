#include "render/Renderer.h"
#include "core/Log.h"
#include "core/Window.h"
#include "gfx/Context.h"
#include "gfx/Pipeline.h"
#include "gfx/VkCheck.h"
#include "render/Meshes.h"
#include "scene/Camera.h"
#include "scene/Constellations.h"
#include "scene/StarField.h"
#include "scene/Volumes.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace space::render {

namespace {

struct ViewPushConstants { // sky + volumes (fragment)
    glm::vec4 camRight, camUp, camForward, camPos;
    glm::vec4 params; // tan(fovY/2), aspect, time, radians per pixel
    glm::vec4 look;   // pass-specific
};
static_assert(sizeof(ViewPushConstants) == 96);

struct TonemapPushConstants {
    float params[4]; // exposure, bloom strength, hdr output flag
    float hdr[4];    // paper white nits, peak nits
};

struct BloomPushConstants {
    float params[4]; // 1/srcW, 1/srcH, knee or radius, first-level flag
};

struct StarPushConstants {
    glm::mat4 viewProj; // rotation-only view * projection
    glm::vec4 camPos;
    glm::vec4 params; // width, height, brightness scale, max radius px
};
static_assert(sizeof(StarPushConstants) <= 128, "push constant budget");

struct DustPushConstants {
    glm::mat4 viewProj;
    glm::vec4 phase;    // xyz camera phase within the field cell (0..1), w extent
    glm::vec4 velocity; // xyz camera velocity (world units / s), w time
    glm::vec4 params;   // viewport w, h, brightness, unused
};

struct CraftPushConstants {
    glm::mat4 viewProj;
    glm::vec4 baseColor;
    glm::ivec4 ids;       // craft index, base colour, normal map, metallic-roughness map
    glm::vec4 material;   // metallic, roughness, normal scale, occlusion map
    glm::mat4 shadowMat;
    glm::vec4 shadowInfo; // shadow texture, 1 / resolution, depth bias, metres per world unit
    glm::vec4 uvTransform;
    glm::vec4 emissive;   // rgb factor, emissive map
};
static_assert(sizeof(CraftPushConstants) == 224);

struct BodyPushConstants {
    glm::mat4 viewProj;
    glm::vec4 sunPos;
    glm::vec4 params;      // time, sun radiance, shell scale, aurora strength
    glm::mat4 shadowMat;
    glm::vec4 shadowInfo;  // shadow texture, 1 / resolution, depth bias, metres per world unit
    glm::vec4 anchorWorld; // xyz camera-relative, w body index (-1 = none)
    glm::vec4 anchorLocal; // xyz body-local metres modulo 4096
    glm::vec4 patchAnchor; // xy anchor uv in the active patch, z du per metre east, w dv per metre north
    glm::vec4 patchEast;   // xyz body-local east at the anchor
    glm::vec4 patchNorth;  // xyz body-local north at the anchor
};
static_assert(sizeof(BodyPushConstants) == 256);

struct PostPushConstants {
    glm::mat4 prevViewProj, viewProj;
    glm::vec4 camRight, camUp, camForward;
    glm::vec4 params;   // tanHalf, aspect, near, history blend
    glm::vec4 camDelta;
    glm::vec4 sun;      // camera-relative Sun, radius
    glm::vec4 glare;    // strength, jitter u, jitter v
};
static_assert(sizeof(PostPushConstants) == 240);

struct GlintPushConstants {
    glm::mat4 viewProj;
    glm::vec4 params; // viewport w, h, radians per pixel, visibility floor
};
constexpr float kMetersPerParsecF = 3.0856775814913673e16f;

constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr float kNearPlane = 1e-17f; // parsecs (~0.3 m): close enough to stand beside a lander

// Reversed-Z infinite perspective for Vulkan clip space (depth 1 at near, -> 0 at infinity).
glm::mat4 reversedInfinitePerspective(float fovY, float aspect, float near) {
    const float f = 1.f / std::tan(fovY * 0.5f);
    glm::mat4 P(0.f);
    P[0][0] = f / aspect;
    P[1][1] = -f; // Vulkan Y down
    P[2][3] = -1.f;
    P[3][2] = near;
    return P;
}

} // namespace

glm::mat4 makeViewProj(const Camera& camera, float aspect) {
    return reversedInfinitePerspective(camera.fovY, aspect, kNearPlane) *
           glm::mat4_cast(glm::conjugate(camera.orientation));
}

namespace {

VkDescriptorSetLayout makeSsboLayout(VkDevice dev, VkShaderStageFlags stages) {
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, stages, nullptr};
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = 1;
    ci.pBindings = &binding;
    VkDescriptorSetLayout layout;
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &ci, nullptr, &layout));
    return layout;
}

void writeSsbo(VkDevice dev, VkDescriptorSet set, VkBuffer buffer) {
    VkDescriptorBufferInfo info{buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &info;
    vkUpdateDescriptorSets(dev, 1, &write, 0, nullptr);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------

void Renderer::init(gfx::Context& ctx, Window& window) {
    m_ctx = &ctx;
    m_window = &window;
    VkDevice dev = ctx.device();

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(dev, &sci, nullptr, &m_linearSampler));

    // Surface map sampler: wrap in longitude, clamp at the poles, anisotropic, trilinear.
    VkSamplerCreateInfo tsi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    tsi.magFilter = VK_FILTER_LINEAR;
    tsi.minFilter = VK_FILTER_LINEAR;
    tsi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    tsi.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    tsi.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    tsi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    tsi.anisotropyEnable = VK_TRUE;
    tsi.maxAnisotropy = 16.f;
    tsi.maxLod = VK_LOD_CLAMP_NONE;
    VK_CHECK(vkCreateSampler(dev, &tsi, nullptr, &m_textureSampler));
    tsi.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(dev, &tsi, nullptr, &m_tileSampler));
    tsi.addressModeU = tsi.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VK_CHECK(vkCreateSampler(dev, &tsi, nullptr, &m_modelSampler));
    {
        VkSamplerCreateInfo ssi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ssi.magFilter = ssi.minFilter = VK_FILTER_NEAREST;
        ssi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        ssi.addressModeU = ssi.addressModeV = ssi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VK_CHECK(vkCreateSampler(dev, &ssi, nullptr, &m_shadowSampler));
    }
    if (ctx.properties().limits.maxPushConstantsSize < sizeof(CraftPushConstants))
        LOG_ERROR("Push constant budget {} < {} bytes: spacecraft and planet passes will fail",
                  ctx.properties().limits.maxPushConstantsSize, sizeof(CraftPushConstants));

    // Bindless-style texture array (partially bound, update-after-bind so textures can stream in).
    {
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures,
                                             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                         VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
        VkDescriptorSetLayoutBindingFlagsCreateInfo bf{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        bf.bindingCount = 1;
        bf.pBindingFlags = &flags;
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.pNext = &bf;
        ci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        VK_CHECK(vkCreateDescriptorSetLayout(dev, &ci, nullptr, &m_textureSetLayout));

        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxTextures};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pi.maxSets = 1;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &size;
        VK_CHECK(vkCreateDescriptorPool(dev, &pi, nullptr, &m_texturePool));

        VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsa.descriptorPool = m_texturePool;
        dsa.descriptorSetCount = 1;
        dsa.pSetLayouts = &m_textureSetLayout;
        VK_CHECK(vkAllocateDescriptorSets(dev, &dsa, &m_textureSet));
        m_textures.reserve(kMaxTextures);
    }

    // Spacecraft shadow map, registered in the texture array so planets and craft can sample it.
    {
        gfx::Texture shadow;
        shadow.image.create(ctx, kShadowSize, kShadowSize, VK_FORMAT_D32_SFLOAT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_ASPECT_DEPTH_BIT);
        VkCommandBuffer cmd = ctx.beginOneShot();
        gfx::transitionImage(cmd, shadow.image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        ctx.endOneShot(cmd);
        m_shadowImage = shadow.image.image;
        m_shadowView = shadow.image.view;
        m_shadowTextureIndex = addTextureWith(std::move(shadow), m_shadowSampler);
    }

    // Descriptor set layouts.
    VkDescriptorSetLayoutBinding tonemapBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 2;
    dli.pBindings = tonemapBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &dli, nullptr, &m_tonemapSetLayout));

    VkDescriptorSetLayoutBinding bloomBindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo bli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    bli.bindingCount = 2;
    bli.pBindings = bloomBindings;
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &bli, nullptr, &m_bloomSetLayout));

    m_ssboFragSetLayout = makeSsboLayout(dev, VK_SHADER_STAGE_FRAGMENT_BIT);
    m_ssboVertexSetLayout = makeSsboLayout(dev, VK_SHADER_STAGE_VERTEX_BIT);
    m_ssboBothSetLayout = makeSsboLayout(dev, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

    VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 24;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(dev, &dpi, nullptr, &m_descriptorPool));

    auto allocSet = [&](VkDescriptorSetLayout layout) {
        VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsa.descriptorPool = m_descriptorPool;
        dsa.descriptorSetCount = 1;
        dsa.pSetLayouts = &layout;
        VkDescriptorSet set;
        VK_CHECK(vkAllocateDescriptorSets(dev, &dsa, &set));
        return set;
    };
    m_tonemapSet = allocSet(m_tonemapSetLayout);
    m_starSet = allocSet(m_ssboVertexSetLayout);
    m_volumeSet = allocSet(m_ssboFragSetLayout);

    for (auto& f : m_frames) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = ctx.queueFamily();
        VK_CHECK(vkCreateCommandPool(dev, &pi, nullptr, &f.pool));

        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(dev, &ai, &f.cmd));

        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(dev, &si, nullptr, &f.imageAvailable));

        VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(dev, &fi, nullptr, &f.inFlight));

        f.bodyBuffer.create(ctx, kMaxBodies * sizeof(BodyGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                            VMA_MEMORY_USAGE_AUTO,
                            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        VmaAllocationInfo info{};
        vmaGetAllocationInfo(ctx.allocator(), f.bodyBuffer.allocation, &info);
        f.bodyMapped = info.pMappedData;
        f.bodySet = allocSet(m_ssboBothSetLayout);
        writeSsbo(dev, f.bodySet, f.bodyBuffer.buffer);

        f.craftBuffer.create(ctx, kMaxCrafts * sizeof(CraftGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             VMA_MEMORY_USAGE_AUTO,
                             VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        vmaGetAllocationInfo(ctx.allocator(), f.craftBuffer.allocation, &info);
        f.craftMapped = info.pMappedData;
        f.craftSet = allocSet(m_ssboBothSetLayout);
        writeSsbo(dev, f.craftSet, f.craftBuffer.buffer);

        f.frameBuffer.create(ctx, sizeof(FrameGpu), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_AUTO,
                             VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
        vmaGetAllocationInfo(ctx.allocator(), f.frameBuffer.allocation, &info);
        f.frameMapped = info.pMappedData;
        f.frameSet = allocSet(m_ssboBothSetLayout);
        writeSsbo(dev, f.frameSet, f.frameBuffer.buffer);
    }

    VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpi.queryCount = kFramesInFlight * 2;
    VK_CHECK(vkCreateQueryPool(dev, &qpi, nullptr, &m_timestampPool));

    createSwapchainDependent();
    createMeshes();
    createPipelines();
    createSwapchainFormatDependent();
}

void Renderer::createSwapchainFormatDependent() {
    m_tonemapLayout = gfx::createPipelineLayout(*m_ctx, {m_tonemapSetLayout, m_ssboBothSetLayout},
                                                sizeof(TonemapPushConstants), VK_SHADER_STAGE_FRAGMENT_BIT);
    m_tonemapPipeline =
        gfx::createFullscreenPipeline(*m_ctx, {"tonemap.frag.spv", m_swapchain.format(), m_tonemapLayout});
    initImGui();
}

void Renderer::destroySwapchainFormatDependent() {
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    vkDestroyPipeline(m_ctx->device(), m_tonemapPipeline, nullptr);
    vkDestroyPipelineLayout(m_ctx->device(), m_tonemapLayout, nullptr);
    m_tonemapPipeline = VK_NULL_HANDLE;
    m_tonemapLayout = VK_NULL_HANDLE;
}

void Renderer::shutdown() {
    if (!m_ctx) return;
    VkDevice dev = m_ctx->device();
    m_ctx->waitIdle();

    destroySwapchainFormatDependent();

    if (m_postPipeline) vkDestroyPipeline(dev, m_postPipeline, nullptr);
    if (m_postLayout) vkDestroyPipelineLayout(dev, m_postLayout, nullptr);
    if (m_postSetLayout) vkDestroyDescriptorSetLayout(dev, m_postSetLayout, nullptr);
    for (VkPipeline p : {m_skyPipeline, m_starPipeline, m_volumePipeline, m_planetPipeline, m_ringPipeline,
                         m_bloomDownPipeline, m_bloomUpPipeline, m_linePipeline, m_dustPipeline, m_atmoPipeline,
                         m_cloudPipeline, m_craftPipeline, m_shadowPipeline, m_glintPipeline, m_plumePipeline})
        vkDestroyPipeline(dev, p, nullptr);
    for (VkPipelineLayout l : {m_skyLayout, m_starLayout, m_volumeLayout, m_bodyLayout, m_bloomLayout, m_lineLayout,
                               m_dustLayout, m_atmoLayout, m_cloudLayout, m_craftLayout, m_glintLayout})
        vkDestroyPipelineLayout(dev, l, nullptr);
    m_lineVB.destroy(*m_ctx);

    m_starBuffer.destroy(*m_ctx);
    m_volumeBuffer.destroy(*m_ctx);
    m_diagBuffer.destroy(*m_ctx);
    for (auto& t : m_textures) t.destroy(*m_ctx);
    m_textures.clear();
    for (auto& m : m_models) {
        m.vb.destroy(*m_ctx);
        m.ib.destroy(*m_ctx);
    }
    m_models.clear();
    vkDestroyDescriptorPool(dev, m_texturePool, nullptr);
    vkDestroyDescriptorSetLayout(dev, m_textureSetLayout, nullptr);
    vkDestroySampler(dev, m_textureSampler, nullptr);
    vkDestroySampler(dev, m_tileSampler, nullptr);
    vkDestroySampler(dev, m_modelSampler, nullptr);
    vkDestroySampler(dev, m_shadowSampler, nullptr);
    for (auto* b : {&m_sphereVB, &m_sphereIB, &m_ringVB, &m_ringIB}) b->destroy(*m_ctx);

    destroySwapchainDependent();

    vkDestroyQueryPool(dev, m_timestampPool, nullptr);
    vkDestroyDescriptorPool(dev, m_descriptorPool, nullptr);
    for (VkDescriptorSetLayout l : {m_tonemapSetLayout, m_bloomSetLayout, m_ssboFragSetLayout, m_ssboVertexSetLayout,
                                    m_ssboBothSetLayout})
        vkDestroyDescriptorSetLayout(dev, l, nullptr);
    vkDestroySampler(dev, m_linearSampler, nullptr);

    for (auto& f : m_frames) {
        f.bodyBuffer.destroy(*m_ctx);
        f.craftBuffer.destroy(*m_ctx);
        f.frameBuffer.destroy(*m_ctx);
        vkDestroyFence(dev, f.inFlight, nullptr);
        vkDestroySemaphore(dev, f.imageAvailable, nullptr);
        vkDestroyCommandPool(dev, f.pool, nullptr);
    }
    m_ctx = nullptr;
}

void Renderer::createSwapchainDependent() {
    int w = 0, h = 0;
    m_window->framebufferSize(w, h);
    m_swapchain.create(*m_ctx, (uint32_t)w, (uint32_t)h, m_vsync, m_preferHdr);

    m_renderFinished.resize(m_swapchain.imageCount());
    VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& s : m_renderFinished) VK_CHECK(vkCreateSemaphore(m_ctx->device(), &si, nullptr, &s));

    auto ext = m_swapchain.extent();
    m_hdr.create(*m_ctx, ext.width, ext.height, kHdrFormat,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    m_depth.create(*m_ctx, ext.width, ext.height, kDepthFormat,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    createPostResources();
    createBloomChain();
    updateTonemapDescriptor();
}

void Renderer::destroySwapchainDependent() {
    destroyBloomChain();
    destroyPostResources();
    m_depth.destroy(*m_ctx);
    m_hdr.destroy(*m_ctx);
    for (auto s : m_renderFinished) vkDestroySemaphore(m_ctx->device(), s, nullptr);
    m_renderFinished.clear();
    m_swapchain.destroy(*m_ctx);
}

void Renderer::recreateSwapchain() {
    int w = 0, h = 0;
    m_window->framebufferSize(w, h);
    if (w == 0 || h == 0) return; // minimised: keep the old one until we are visible again
    m_ctx->waitIdle();
    const VkFormat oldFormat = m_swapchain.format();
    destroySwapchainDependent();
    createSwapchainDependent();
    if (m_swapchain.format() != oldFormat) {
        // Tonemap pipeline and the ImGui backend were built against the old surface format.
        destroySwapchainFormatDependent();
        createSwapchainFormatDependent();
    } else {
        ImGui_ImplVulkan_SetMinImageCount(2);
    }
    m_recreateSwapchain = false;
}

void Renderer::updateTonemapDescriptor() {
    VkDescriptorImageInfo hdr{m_linearSampler, m_post.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo bloom{m_linearSampler, m_bloomMips[0].image.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[2]{};
    for (auto& w : writes) {
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = m_tonemapSet;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    writes[0].dstBinding = 0;
    writes[0].pImageInfo = &hdr;
    writes[1].dstBinding = 1;
    writes[1].pImageInfo = &bloom;
    vkUpdateDescriptorSets(m_ctx->device(), 2, writes, 0, nullptr);
}

void Renderer::createBloomChain() {
    VkDevice dev = m_ctx->device();
    uint32_t w = m_swapchain.extent().width / 2, h = m_swapchain.extent().height / 2;
    while (w >= 8 && h >= 8 && m_bloomMips.size() < 7) {
        BloomMip mip;
        mip.image.create(*m_ctx, w, h, kHdrFormat,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        m_bloomMips.push_back(mip);
        w /= 2;
        h /= 2;
    }
    const uint32_t n = (uint32_t)m_bloomMips.size();

    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, n * 2},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, n * 2}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = n * 2;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(dev, &dpi, nullptr, &m_bloomPool));

    std::vector<VkDescriptorSetLayout> layouts(n * 2, m_bloomSetLayout);
    std::vector<VkDescriptorSet> sets(n * 2);
    VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsa.descriptorPool = m_bloomPool;
    dsa.descriptorSetCount = n * 2;
    dsa.pSetLayouts = layouts.data();
    VK_CHECK(vkAllocateDescriptorSets(dev, &dsa, sets.data()));

    std::vector<VkDescriptorImageInfo> infos;
    infos.reserve(n * 4);
    std::vector<VkWriteDescriptorSet> writes;
    auto write = [&](VkDescriptorSet set, uint32_t binding, VkDescriptorType type, VkImageView view,
                     VkImageLayout layout) {
        infos.push_back({type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ? m_linearSampler : VK_NULL_HANDLE,
                         view, layout});
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = type;
        w.pImageInfo = &infos.back();
        writes.push_back(w);
    };
    for (uint32_t i = 0; i < n; ++i) {
        m_bloomMips[i].downSet = sets[i * 2];
        m_bloomMips[i].upSet = sets[i * 2 + 1];
        VkImageView srcDown = i == 0 ? m_post.view : m_bloomMips[i - 1].image.view;
        VkImageLayout srcDownLayout = VK_IMAGE_LAYOUT_GENERAL;
        write(m_bloomMips[i].downSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, srcDown, srcDownLayout);
        write(m_bloomMips[i].downSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, m_bloomMips[i].image.view,
              VK_IMAGE_LAYOUT_GENERAL);
        if (i + 1 < n) {
            write(m_bloomMips[i].upSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, m_bloomMips[i + 1].image.view,
                  VK_IMAGE_LAYOUT_GENERAL);
            write(m_bloomMips[i].upSet, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, m_bloomMips[i].image.view,
                  VK_IMAGE_LAYOUT_GENERAL);
        }
    }
    vkUpdateDescriptorSets(dev, (uint32_t)writes.size(), writes.data(), 0, nullptr);

    // Move every mip into GENERAL once; they stay there for their whole life.
    VkCommandBuffer cmd = m_ctx->beginOneShot();
    for (auto& mip : m_bloomMips)
        gfx::transitionImage(cmd, mip.image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    m_ctx->endOneShot(cmd);
}

void Renderer::createPostResources() {
    VkDevice dev = m_ctx->device();
    const VkExtent2D ext = m_swapchain.extent();
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    m_post.create(*m_ctx, ext.width, ext.height, kHdrFormat, usage);
    for (auto& h : m_history) h.create(*m_ctx, ext.width, ext.height, kHdrFormat, usage);
    {
        VkCommandBuffer cmd = m_ctx->beginOneShot();
        for (gfx::Image* img : {&m_post, &m_history[0], &m_history[1]})
            gfx::transitionImage(cmd, img->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                 VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        m_ctx->endOneShot(cmd);
    }
    m_historyValid = false;

    if (!m_postSetLayout) {
        VkDescriptorSetLayoutBinding b[5] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 5;
        ci.pBindings = b;
        VK_CHECK(vkCreateDescriptorSetLayout(dev, &ci, nullptr, &m_postSetLayout));
        m_postLayout = gfx::createPipelineLayout(*m_ctx, {m_postSetLayout}, sizeof(PostPushConstants),
                                                 VK_SHADER_STAGE_COMPUTE_BIT);
        m_postPipeline = gfx::createComputePipeline(*m_ctx, "post.comp.spv", m_postLayout);
    }
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 2;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = sizes;
    VK_CHECK(vkCreateDescriptorPool(dev, &dpi, nullptr, &m_postPool));
    VkDescriptorSetLayout layouts[2] = {m_postSetLayout, m_postSetLayout};
    VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsa.descriptorPool = m_postPool;
    dsa.descriptorSetCount = 2;
    dsa.pSetLayouts = layouts;
    VK_CHECK(vkAllocateDescriptorSets(dev, &dsa, m_postSets));
    for (int i = 0; i < 2; ++i) {
        VkDescriptorImageInfo infos[5] = {
            {m_linearSampler, m_hdr.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {m_shadowSampler, m_depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {m_linearSampler, m_history[1 - i].view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, m_post.view, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, m_history[i].view, VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[5];
        for (int k = 0; k < 5; ++k) {
            writes[k] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[k].dstSet = m_postSets[i];
            writes[k].dstBinding = k;
            writes[k].descriptorCount = 1;
            writes[k].descriptorType = k < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            writes[k].pImageInfo = &infos[k];
        }
        vkUpdateDescriptorSets(dev, 5, writes, 0, nullptr);
    }
}

void Renderer::destroyPostResources() {
    if (m_postPool) vkDestroyDescriptorPool(m_ctx->device(), m_postPool, nullptr);
    m_postPool = VK_NULL_HANDLE;
    m_post.destroy(*m_ctx);
    for (auto& h : m_history) h.destroy(*m_ctx);
}

void Renderer::destroyBloomChain() {
    for (auto& mip : m_bloomMips) mip.image.destroy(*m_ctx);
    m_bloomMips.clear();
    if (m_bloomPool) vkDestroyDescriptorPool(m_ctx->device(), m_bloomPool, nullptr);
    m_bloomPool = VK_NULL_HANDLE;
}

void Renderer::createMeshes() {
    MeshData sphere = makeUvSphere(48, 96);
    MeshData ring = makeRing(192);
    m_sphereVB = gfx::createDeviceLocalBuffer(*m_ctx, sphere.vertices.data(), sphere.vertices.size() * sizeof(glm::vec4),
                                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    m_sphereIB = gfx::createDeviceLocalBuffer(*m_ctx, sphere.indices.data(), sphere.indices.size() * sizeof(uint32_t),
                                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    m_sphereIndexCount = (uint32_t)sphere.indices.size();
    m_ringVB = gfx::createDeviceLocalBuffer(*m_ctx, ring.vertices.data(), ring.vertices.size() * sizeof(glm::vec4),
                                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    m_ringIB = gfx::createDeviceLocalBuffer(*m_ctx, ring.indices.data(), ring.indices.size() * sizeof(uint32_t),
                                            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    m_ringIndexCount = (uint32_t)ring.indices.size();
}

void Renderer::createPipelines() {
    const VkFormat hdr = kHdrFormat, depth = kDepthFormat;

    m_skyLayout = gfx::createPipelineLayout(*m_ctx, {}, sizeof(ViewPushConstants), VK_SHADER_STAGE_FRAGMENT_BIT);
    gfx::GraphicsPipelineDesc sky;
    sky.fragmentShader = "sky.frag.spv";
    sky.colorFormat = hdr;
    sky.depthFormat = depth;
    sky.layout = m_skyLayout;
    m_skyPipeline = gfx::createGraphicsPipeline(*m_ctx, sky);

    m_volumeLayout = gfx::createPipelineLayout(*m_ctx, {m_ssboFragSetLayout, m_ssboBothSetLayout},
                                               sizeof(ViewPushConstants), VK_SHADER_STAGE_FRAGMENT_BIT);
    gfx::GraphicsPipelineDesc vol;
    vol.fragmentShader = "nebula.frag.spv";
    vol.colorFormat = hdr;
    vol.depthFormat = depth;
    vol.layout = m_volumeLayout;
    vol.blend = gfx::BlendMode::PremultipliedOver;
    m_volumePipeline = gfx::createGraphicsPipeline(*m_ctx, vol);

    m_starLayout = gfx::createPipelineLayout(*m_ctx, {m_ssboVertexSetLayout, m_ssboBothSetLayout},
                                             sizeof(StarPushConstants), VK_SHADER_STAGE_VERTEX_BIT);
    gfx::GraphicsPipelineDesc stars;
    stars.vertexShader = "stars.vert.spv";
    stars.fragmentShader = "stars.frag.spv";
    stars.colorFormat = hdr;
    stars.depthFormat = depth;
    stars.layout = m_starLayout;
    stars.blend = gfx::BlendMode::Additive;
    stars.depthTest = true;
    m_starPipeline = gfx::createGraphicsPipeline(*m_ctx, stars);

    m_bodyLayout = gfx::createPipelineLayout(*m_ctx, {m_ssboBothSetLayout, m_textureSetLayout},
                                             sizeof(BodyPushConstants),
                                             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    gfx::GraphicsPipelineDesc planet;
    planet.vertexShader = "planet.vert.spv";
    planet.fragmentShader = "planet.frag.spv";
    planet.colorFormat = hdr;
    planet.depthFormat = depth;
    planet.layout = m_bodyLayout;
    planet.vertexInputVec4 = true;
    planet.depthTest = true;
    planet.depthWrite = true;
    planet.cullFront = true; // proxy sphere: far side only, the fragment shader finds the true surface
    m_planetPipeline = gfx::createGraphicsPipeline(*m_ctx, planet);

    gfx::GraphicsPipelineDesc ring = planet;
    ring.blend = gfx::BlendMode::PremultipliedOver;
    ring.depthWrite = false;
    ring.cullBack = false;
    ring.cullFront = false;
    m_ringPipeline = gfx::createGraphicsPipeline(*m_ctx, ring);

    m_atmoLayout = gfx::createPipelineLayout(*m_ctx, {m_ssboBothSetLayout, m_ssboBothSetLayout},
                                             sizeof(BodyPushConstants),
                                             VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    gfx::GraphicsPipelineDesc atmo = planet;
    atmo.fragmentShader = "atmo.frag.spv";
    atmo.layout = m_atmoLayout;
    atmo.blend = gfx::BlendMode::PremultipliedOver;
    atmo.depthWrite = false;
    atmo.cullBack = false; // both faces: the fragment shader keeps the near faces outside, the far faces inside
    atmo.cullFront = false;
    m_atmoPipeline = gfx::createGraphicsPipeline(*m_ctx, atmo);

    m_craftLayout = gfx::createPipelineLayout(*m_ctx, {m_ssboBothSetLayout, m_textureSetLayout},
                                              sizeof(CraftPushConstants),
                                              VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    gfx::GraphicsPipelineDesc craft;
    craft.vertexShader = "craft.vert.spv";
    craft.fragmentShader = "craft.frag.spv";
    craft.colorFormat = hdr;
    craft.depthFormat = depth;
    craft.layout = m_craftLayout;
    craft.vertexInputModel = true;
    craft.depthTest = true;
    craft.depthWrite = true;
    craft.cullBack = false; // NASA meshes have mixed winding
    m_craftPipeline = gfx::createGraphicsPipeline(*m_ctx, craft);

    gfx::GraphicsPipelineDesc shadowDesc = craft;
    shadowDesc.fragmentShader = "shadow.frag.spv";
    shadowDesc.colorFormat = VK_FORMAT_UNDEFINED;
    shadowDesc.depthFormat = VK_FORMAT_D32_SFLOAT;
    shadowDesc.depthOnly = true;
    m_shadowPipeline = gfx::createGraphicsPipeline(*m_ctx, shadowDesc);

    m_glintLayout = gfx::createPipelineLayout(*m_ctx, {m_ssboBothSetLayout}, sizeof(GlintPushConstants),
                                              VK_SHADER_STAGE_VERTEX_BIT);
    gfx::GraphicsPipelineDesc glint;
    glint.vertexShader = "glint.vert.spv";
    glint.fragmentShader = "glint.frag.spv";
    glint.colorFormat = hdr;
    glint.depthFormat = depth;
    glint.layout = m_glintLayout;
    glint.blend = gfx::BlendMode::Additive;
    glint.depthTest = true;
    m_glintPipeline = gfx::createGraphicsPipeline(*m_ctx, glint);

    gfx::GraphicsPipelineDesc plume = craft;
    plume.fragmentShader = "plume.frag.spv";
    plume.blend = gfx::BlendMode::PremultipliedOver;
    plume.depthWrite = false;
    plume.cullBack = false;
    m_plumePipeline = gfx::createGraphicsPipeline(*m_ctx, plume);

    m_cloudLayout = gfx::createPipelineLayout(*m_ctx, {m_ssboBothSetLayout}, sizeof(ViewPushConstants),
                                              VK_SHADER_STAGE_FRAGMENT_BIT);
    gfx::GraphicsPipelineDesc cloud;
    cloud.fragmentShader = "musiccloud.frag.spv";
    cloud.colorFormat = hdr;
    cloud.depthFormat = depth;
    cloud.layout = m_cloudLayout;
    cloud.blend = gfx::BlendMode::PremultipliedOver;
    m_cloudPipeline = gfx::createGraphicsPipeline(*m_ctx, cloud);

    m_lineLayout = gfx::createPipelineLayout(*m_ctx, {m_ssboBothSetLayout}, sizeof(StarPushConstants),
                                             VK_SHADER_STAGE_VERTEX_BIT);

    m_dustLayout = gfx::createPipelineLayout(*m_ctx, {m_ssboBothSetLayout}, sizeof(DustPushConstants),
                                             VK_SHADER_STAGE_VERTEX_BIT);
    gfx::GraphicsPipelineDesc dust;
    dust.vertexShader = "dust.vert.spv";
    dust.fragmentShader = "dust.frag.spv";
    dust.colorFormat = hdr;
    dust.depthFormat = depth;
    dust.layout = m_dustLayout;
    dust.blend = gfx::BlendMode::Additive;
    dust.depthTest = true;
    m_dustPipeline = gfx::createGraphicsPipeline(*m_ctx, dust);
    gfx::GraphicsPipelineDesc lines;
    lines.vertexShader = "lines.vert.spv";
    lines.fragmentShader = "lines.frag.spv";
    lines.colorFormat = hdr;
    lines.depthFormat = depth;
    lines.layout = m_lineLayout;
    lines.vertexInputVec4 = true;
    lines.blend = gfx::BlendMode::Additive;
    lines.depthTest = true;
    lines.lines = true;
    m_linePipeline = gfx::createGraphicsPipeline(*m_ctx, lines);

    m_bloomLayout = gfx::createPipelineLayout(*m_ctx, {m_bloomSetLayout}, sizeof(BloomPushConstants),
                                              VK_SHADER_STAGE_COMPUTE_BIT);
    m_bloomDownPipeline = gfx::createComputePipeline(*m_ctx, "bloom_down.comp.spv", m_bloomLayout);
    m_bloomUpPipeline = gfx::createComputePipeline(*m_ctx, "bloom_up.comp.spv", m_bloomLayout);
}

void Renderer::setStars(const std::vector<StarGpu>& stars) {
    m_ctx->waitIdle();
    m_starBuffer.destroy(*m_ctx);
    m_starCount = (uint32_t)stars.size();
    if (m_starCount == 0) return;
    m_starBuffer = gfx::createDeviceLocalBuffer(*m_ctx, stars.data(), stars.size() * sizeof(StarGpu),
                                                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    writeSsbo(m_ctx->device(), m_starSet, m_starBuffer.buffer);
    LOG_INFO("Uploaded {} stars ({:.1f} MB)", m_starCount, stars.size() * sizeof(StarGpu) / 1e6);
}

void Renderer::setVolumes(const VolumeScene& volumes) {
    m_ctx->waitIdle();
    m_volumeBuffer.destroy(*m_ctx);
    std::vector<uint8_t> blob = volumes.pack();
    m_nebulaCount = (uint32_t)volumes.nebulae.size();
    m_volumeBuffer = gfx::createDeviceLocalBuffer(*m_ctx, blob.data(), blob.size(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    m_volumeBlob = std::move(blob);
    VmaAllocationInfo ai{};
    vmaGetAllocationInfo(m_ctx->allocator(), m_volumeBuffer.allocation, &ai);
    LOG_INFO("[mem] volume buffer: memory {} offset {} size {}", (void*)ai.deviceMemory, ai.offset, ai.size);
    for (auto& f : m_frames) {
        vmaGetAllocationInfo(m_ctx->allocator(), f.bodyBuffer.allocation, &ai);
        LOG_INFO("[mem] body buffer:   memory {} offset {} size {}", (void*)ai.deviceMemory, ai.offset, ai.size);
    }
    writeSsbo(m_ctx->device(), m_volumeSet, m_volumeBuffer.buffer);
    LOG_INFO("Uploaded volumetric scene with {} nebulae", m_nebulaCount);
}

void Renderer::setConstellationLines(const std::vector<LineVertex>& vertices) {
    m_ctx->waitIdle();
    m_lineVB.destroy(*m_ctx);
    m_lineVertexCount = (uint32_t)vertices.size();
    if (m_lineVertexCount == 0) return;
    static_assert(sizeof(LineVertex) == 16);
    m_lineVB = gfx::createDeviceLocalBuffer(*m_ctx, vertices.data(), vertices.size() * sizeof(LineVertex),
                                            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
}

int Renderer::addModel(const gfx::ModelData& model) {
    ModelGpu m;
    m.vb = gfx::createDeviceLocalBuffer(*m_ctx, model.vertices.data(), model.vertices.size() * sizeof(gfx::ModelVertex),
                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    m.ib = gfx::createDeviceLocalBuffer(*m_ctx, model.indices.data(), model.indices.size() * sizeof(uint32_t),
                                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    m.extent = model.extent();
    m.minY = model.boundsMin.y;
    m.center = (model.boundsMin + model.boundsMax) * 0.5f;
    m.flame = model.volumetricFlame;
    std::vector<int> imageToTexture(model.images.size(), -1);
    for (size_t i = 0; i < model.images.size(); ++i) {
        gfx::Texture t;
        if (t.upload(*m_ctx, model.images[i], model.images[i].srgb, "model texture"))
            imageToTexture[i] = addTextureWith(std::move(t), m_modelSampler);
    }
    m.primitives = model.primitives;
    auto remap = [&](int image) { return image >= 0 && image < (int)imageToTexture.size() ? imageToTexture[image] : -1; };
    for (auto& p : m.primitives) {
        p.imageIndex = remap(p.imageIndex);
        p.normalImageIndex = remap(p.normalImageIndex);
        p.mrImageIndex = remap(p.mrImageIndex);
        p.occlusionImageIndex = remap(p.occlusionImageIndex);
        p.emissiveImageIndex = remap(p.emissiveImageIndex);
    }
    m_models.push_back(std::move(m));
    return (int)m_models.size() - 1;
}

int Renderer::addTexture(gfx::Texture&& texture, bool clampU) {
    return addTextureWith(std::move(texture), clampU ? m_tileSampler : m_textureSampler);
}

void Renderer::recordShadowPass(VkCommandBuffer cmd, const FrameScene& scene) {
    gfx::transitionImage(cmd, m_shadowImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT);
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth.imageView = m_shadowView;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {0.f, 0}; // reversed: 0 = farthest from the Sun
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {kShadowSize, kShadowSize}};
    ri.layerCount = 1;
    ri.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport vp{0.f, 0.f, (float)kShadowSize, (float)kShadowSize, 0.f, 1.f};
    const VkRect2D sc{{0, 0}, {kShadowSize, kShadowSize}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    const Frame& frame = m_frames[m_frameIndex];
    VkDescriptorSet sets[] = {frame.craftSet, m_textureSet};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_craftLayout, 0, 2, sets, 0, nullptr);
    const VkDeviceSize zero = 0;
    const size_t n = std::min<size_t>(scene.crafts.size(), kMaxCrafts);
    for (int i : scene.shadowCasters) {
        if (i < 0 || (size_t)i >= n || (size_t)i >= scene.craftModels.size()) continue;
        const int mi = scene.craftModels[i];
        if (mi < 0 || mi >= (int)m_models.size() || m_models[mi].flame) continue;
        const ModelGpu& m = m_models[mi];
        vkCmdBindVertexBuffers(cmd, 0, 1, &m.vb.buffer, &zero);
        vkCmdBindIndexBuffer(cmd, m.ib.buffer, 0, VK_INDEX_TYPE_UINT32);
        CraftPushConstants cpc{};
        cpc.viewProj = scene.shadowMatrix;
        cpc.ids = glm::ivec4(i, -1, -1, -1);
        cpc.uvTransform = glm::vec4(0.f, 0.f, 1.f, 1.f);
        cpc.shadowInfo = glm::vec4(-1.f, 0.f, 0.f, kMetersPerParsecF);
        cpc.emissive = glm::vec4(0.f, 0.f, 0.f, -1.f);
        vkCmdPushConstants(cmd, m_craftLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(cpc), &cpc);
        for (const auto& prim : m.primitives) vkCmdDrawIndexed(cmd, prim.indexCount, 1, prim.firstIndex, 0, 0);
    }
    vkCmdEndRendering(cmd);
    gfx::transitionImage(cmd, m_shadowImage, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
}

void Renderer::replaceTexture(int index, gfx::Texture&& texture) {
    if (index < 0 || index >= (int)m_textures.size()) {
        texture.destroy(*m_ctx);
        return;
    }
    m_ctx->waitIdle();
    m_textures[index].destroy(*m_ctx);
    m_textures[index] = std::move(texture);
    VkDescriptorImageInfo info{m_textureSampler, m_textures[index].image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m_textureSet;
    write.dstBinding = 0;
    write.dstArrayElement = (uint32_t)index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(m_ctx->device(), 1, &write, 0, nullptr);
}

int Renderer::addTextureWith(gfx::Texture&& texture, VkSampler sampler) {
    if (m_textures.size() >= kMaxTextures) {
        texture.destroy(*m_ctx);
        return -1;
    }
    m_textures.push_back(std::move(texture));
    const uint32_t index = (uint32_t)m_textures.size() - 1;
    VkDescriptorImageInfo info{sampler, m_textures.back().image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = m_textureSet;
    write.dstBinding = 0;
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(m_ctx->device(), 1, &write, 0, nullptr);
    return (int)index;
}

void Renderer::initImGui() {
    VkDevice dev = m_ctx->device();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // keyboard nav stays off so WASD never gets captured by the overlay
    ImGui::StyleColorsDark();
    ImGui::GetStyle().WindowRounding = 6.f;
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 0.75f;
    {
        int fbw = 0, fbh = 0;
        m_window->framebufferSize(fbw, fbh);
        m_uiScale = std::clamp(fbh / 1080.f, 0.8f, 2.5f);
        const char* body = "C:/Windows/Fonts/segoeui.ttf";
        const char* title = "C:/Windows/Fonts/segoeuil.ttf"; // Segoe UI Light
        if (std::filesystem::exists(body)) {
            m_uiFont = io.Fonts->AddFontFromFileTTF(body, 19.f * m_uiScale);
            m_titleFont = io.Fonts->AddFontFromFileTTF(std::filesystem::exists(title) ? title : body, 64.f * m_uiScale);
        }
        if (!m_uiFont) m_uiFont = io.Fonts->AddFontDefault();
        if (!m_titleFont) m_titleFont = m_uiFont;
        io.FontDefault = m_uiFont;
        ImGui::GetStyle().ScaleAllSizes(m_uiScale);
    }

    ImGui_ImplGlfw_InitForVulkan(m_window->handle(), true);

    VkFormat swapFormat = m_swapchain.format();
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_3;
    info.Instance = m_ctx->instance();
    info.PhysicalDevice = m_ctx->physicalDevice();
    info.Device = dev;
    info.QueueFamily = m_ctx->queueFamily();
    info.Queue = m_ctx->queue();
    info.DescriptorPoolSize = 16; // backend owns its pool (it needs separate SAMPLER + SAMPLED_IMAGE types)
    info.MinImageCount = 2;
    info.ImageCount = m_swapchain.imageCount();
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &swapFormat;
    info.CheckVkResultFn = [](VkResult r) {
        if (r != VK_SUCCESS) LOG_ERROR("ImGui Vulkan error {}", (int)r);
    };
    ImGui_ImplVulkan_Init(&info);
}

// ---------------------------------------------------------------------------------------------
// Per-frame
// ---------------------------------------------------------------------------------------------

void Renderer::beginFrame() {
    // Swapchain rebuilds happen here, before ImGui starts a frame, because a surface-format change
    // also rebuilds the ImGui backend and must not happen while draw data for a frame is alive.
    if (m_recreateSwapchain) recreateSwapchain();
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void Renderer::endFrame(const Camera& camera, double timeSeconds, const RenderSettings& settings,
                        const FrameScene& scene) {
    ImGui::Render();

    if (settings.vsync != m_vsync || settings.hdrOutput != m_preferHdr) {
        m_vsync = settings.vsync;
        m_preferHdr = settings.hdrOutput;
        m_recreateSwapchain = true;
    }
    if (m_recreateSwapchain) return; // rebuilt at the next beginFrame; this frame is dropped

    int w = 0, h = 0;
    m_window->framebufferSize(w, h);
    if (w == 0 || h == 0) return;

    VkDevice dev = m_ctx->device();
    Frame& frame = m_frames[m_frameIndex];

    VK_CHECK(vkWaitForFences(dev, 1, &frame.inFlight, VK_TRUE, UINT64_MAX));
    if (m_diagPending) {
        m_ctx->waitIdle();
        reportDiagnostic();
    }

    // GPU time of the frame that just finished using this slot.
    uint64_t stamps[2] = {};
    if (frame.submitted &&
        vkGetQueryPoolResults(dev, m_timestampPool, m_frameIndex * 2, 2, sizeof(stamps), stamps, sizeof(uint64_t),
                              VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        double period = m_ctx->properties().limits.timestampPeriod; // ns per tick
        m_gpuFrameMs = (float)((stamps[1] - stamps[0]) * period * 1e-6);
    }

    uint32_t imageIndex = 0;
    VkResult acquire = vkAcquireNextImageKHR(dev, m_swapchain.handle(), UINT64_MAX, frame.imageAvailable,
                                             VK_NULL_HANDLE, &imageIndex);
    if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
        m_recreateSwapchain = true;
        return;
    }
    if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) VK_CHECK(acquire);

    // Per-frame body list (the fence guarantees the GPU is done reading this slot's buffer).
    const size_t bodyCount = std::min<size_t>(scene.bodies.size(), kMaxBodies);
    if (bodyCount > 0) {
        std::memcpy(frame.bodyMapped, scene.bodies.data(), bodyCount * sizeof(BodyGpu));
        vmaFlushAllocation(m_ctx->allocator(), frame.bodyBuffer.allocation, 0, bodyCount * sizeof(BodyGpu));
    }

    if (!scene.crafts.empty()) {
        const size_t n = std::min<size_t>(scene.crafts.size(), kMaxCrafts);
        std::memcpy(frame.craftMapped, scene.crafts.data(), n * sizeof(CraftGpu));
        vmaFlushAllocation(m_ctx->allocator(), frame.craftBuffer.allocation, 0, n * sizeof(CraftGpu));
    }
    {
        FrameGpu fg{};
        std::memcpy(fg.bands, scene.audioBands, sizeof fg.bands);
        fg.features[0] = scene.bass;
        fg.features[1] = scene.mid;
        fg.features[2] = scene.treble;
        fg.features[3] = scene.starTint;
        fg.misc[0] = scene.beatAge;
        fg.misc[1] = scene.level;
        fg.misc[2] = scene.audioReact;
        fg.misc[3] = (float)timeSeconds;
        const glm::vec3 fwd = camera.forward();
        fg.viewForward[0] = fwd.x;
        fg.viewForward[1] = fwd.y;
        fg.viewForward[2] = fwd.z;
        fg.mood[0] = scene.moodColor.r;
        fg.mood[1] = scene.moodColor.g;
        fg.mood[2] = scene.moodColor.b;
        fg.mood[3] = scene.waveStrength;
        const size_t n = std::min<size_t>(scene.constellationHighlight.size(), kMaxConstellations);
        if (n) std::memcpy(fg.highlight, scene.constellationHighlight.data(), n * sizeof(float));
        std::memcpy(frame.frameMapped, &fg, sizeof fg);
        vmaFlushAllocation(m_ctx->allocator(), frame.frameBuffer.allocation, 0, sizeof fg);
    }

    VK_CHECK(vkResetFences(dev, 1, &frame.inFlight));
    VK_CHECK(vkResetCommandBuffer(frame.cmd, 0));

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(frame.cmd, &bi));
    recordFrame(frame.cmd, imageIndex, camera, timeSeconds, settings, scene);
    VK_CHECK(vkEndCommandBuffer(frame.cmd));

    VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitInfo.semaphore = frame.imageAvailable;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    VkSemaphoreSubmitInfo signalInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signalInfo.semaphore = m_renderFinished[imageIndex];
    signalInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = frame.cmd;

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &waitInfo;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signalInfo;
    VK_CHECK(vkQueueSubmit2(m_ctx->queue(), 1, &submit, frame.inFlight));
    frame.submitted = true;

    VkSwapchainKHR sc = m_swapchain.handle();
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &m_renderFinished[imageIndex];
    present.swapchainCount = 1;
    present.pSwapchains = &sc;
    present.pImageIndices = &imageIndex;
    VkResult pr = vkQueuePresentKHR(m_ctx->queue(), &present);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR || m_window->wasResized()) {
        m_recreateSwapchain = true;
        m_window->clearResized();
    } else {
        VK_CHECK(pr);
    }

    m_frameIndex = (m_frameIndex + 1) % kFramesInFlight;
}

void Renderer::recordFrame(VkCommandBuffer cmd, uint32_t imageIndex, const Camera& camera, double time,
                           const RenderSettings& settings, const FrameScene& scene) {
    const VkExtent2D ext = m_swapchain.extent();
    const VkViewport viewport{0.f, 0.f, (float)ext.width, (float)ext.height, 0.f, 1.f};
    const VkRect2D scissor{{0, 0}, ext};
    const float aspect = (float)ext.width / (float)ext.height;
    const float tanHalf = std::tan(camera.fovY * 0.5f);
    const Frame& frame = m_frames[m_frameIndex];

    vkCmdResetQueryPool(cmd, m_timestampPool, m_frameIndex * 2, 2);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, m_timestampPool, m_frameIndex * 2);

    // Shared camera data.
    const glm::vec3 r = camera.right(), u = camera.up(), f = camera.forward();
    ViewPushConstants view{};
    view.camRight = glm::vec4(r, 0.f);
    view.camUp = glm::vec4(u, 0.f);
    view.camForward = glm::vec4(f, 0.f);
    view.camPos = glm::vec4(glm::vec3(camera.position), 0.f);
    view.params = glm::vec4(tanHalf, aspect, (float)time, 2.f * tanHalf / (float)ext.height);

    const glm::mat4 viewProjClean = makeViewProj(camera, aspect);
    // TAA: a Halton (2,3) sub-pixel jitter on the projection; the post pass samples it back to centre.
    glm::vec2 jitterPx(0.f);
    if (settings.taa) {
        auto halton = [](uint32_t i, uint32_t b) {
            float f = 1.f, r = 0.f;
            while (i > 0) { f /= (float)b; r += f * (float)(i % b); i /= b; }
            return r;
        };
        const uint32_t k = (m_frameSerial % 8) + 1;
        jitterPx = glm::vec2(halton(k, 2) - 0.5f, halton(k, 3) - 0.5f);
    }
    const glm::vec2 jitterNdc = jitterPx * glm::vec2(2.f / ext.width, 2.f / ext.height);
    const glm::mat4 viewProj = glm::translate(glm::mat4(1.f), glm::vec3(jitterNdc, 0.f)) * viewProjClean;

    if (scene.shadowEnabled && !scene.shadowCasters.empty() && settings.drawBodies) recordShadowPass(cmd, scene);
    const glm::vec4 shadowInfo(scene.shadowEnabled ? (float)m_shadowTextureIndex : -1.f, 1.f / (float)kShadowSize,
                               scene.shadowDepthBias, kMetersPerParsecF);

    // ---- HDR pass: sky -> volumes -> bodies -> stars ----------------------------------------
    // The previous frame read the HDR target from both the bloom compute pass and the tonemap fragment pass.
    gfx::transitionImage(cmd, m_hdr.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    gfx::transitionImage(cmd, m_depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_IMAGE_ASPECT_DEPTH_BIT);
    {
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = m_hdr.view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = m_depth.view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {0.f, 0}; // reversed-Z: 0 = infinitely far

        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = scissor;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        ri.pDepthAttachment = &depth;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // Sky (always drawn: it also clears the colour target)
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_skyPipeline);
        view.look = glm::vec4(settings.drawSky ? settings.skyDensity : 0.f,
                              settings.drawSky ? settings.nebulaIntensity : 0.f, 0.f, 0.f);
        vkCmdPushConstants(cmd, m_skyLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(view), &view);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        // Volumetrics
        if (settings.volumetrics && m_volumeBuffer.buffer) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_volumePipeline);
            VkDescriptorSet volSets[] = {m_volumeSet, frame.frameSet};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_volumeLayout, 0, 2, volSets, 0, nullptr);
            view.look = glm::vec4(settings.galaxyGlow, settings.galaxyDust, settings.nebulaGain, (float)m_nebulaCount);
            vkCmdPushConstants(cmd, m_volumeLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(view), &view);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }

        // Bodies
        const uint32_t totalBodies =
            settings.drawBodies ? (uint32_t)std::min<size_t>(scene.bodies.size(), kMaxBodies) : 0u;
        if (totalBodies > 0) {
            BodyPushConstants bpc{};
            bpc.viewProj = viewProj;
            bpc.sunPos = glm::vec4(scene.sunPosRel, 0.f);
            bpc.params = glm::vec4((float)time, scene.sunRadiance, 0.f, 0.f);
            bpc.shadowMat = scene.shadowMatrix;
            bpc.shadowInfo = shadowInfo;
            bpc.anchorWorld = glm::vec4(scene.detailAnchorWorld, (float)scene.detailBody);
            bpc.anchorLocal = glm::vec4(scene.detailAnchorLocal, (float)scene.sphereCount);
            bpc.patchAnchor = scene.patchAnchor;
            bpc.patchEast = scene.patchEast;
            bpc.patchNorth = scene.patchNorth;
            const VkDeviceSize zero = 0;

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_planetPipeline);
            VkDescriptorSet bodySets[] = {frame.bodySet, m_textureSet};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bodyLayout, 0, 2, bodySets, 0, nullptr);
            vkCmdPushConstants(cmd, m_bodyLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(bpc), &bpc);
            vkCmdBindVertexBuffers(cmd, 0, 1, &m_sphereVB.buffer, &zero);
            vkCmdBindIndexBuffer(cmd, m_sphereIB.buffer, 0, VK_INDEX_TYPE_UINT32);
            const uint32_t spheres = std::min(scene.sphereCount, totalBodies);
            vkCmdDrawIndexed(cmd, m_sphereIndexCount, spheres, 0, 0, 0);

            // Atmosphere shells over the same instances (bodies without one collapse in the vertex shader).
            bpc.params.z = kAtmosphereShell;
            bpc.params.w = scene.auroraStrength;
            VkDescriptorSet atmoSets[] = {frame.bodySet, frame.frameSet};
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_atmoPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_atmoLayout, 0, 2, atmoSets, 0, nullptr);
            vkCmdPushConstants(cmd, m_atmoLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(bpc), &bpc);
            vkCmdDrawIndexed(cmd, m_sphereIndexCount, spheres, 0, 0, 0);
            bpc.params.z = 0.f;
            bpc.params.w = 0.f;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_bodyLayout, 0, 2, bodySets, 0, nullptr);
            vkCmdPushConstants(cmd, m_bodyLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(bpc), &bpc);

            if (totalBodies > spheres) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ringPipeline);
                vkCmdBindVertexBuffers(cmd, 0, 1, &m_ringVB.buffer, &zero);
                vkCmdBindIndexBuffer(cmd, m_ringIB.buffer, 0, VK_INDEX_TYPE_UINT32);
                vkCmdDrawIndexed(cmd, m_ringIndexCount, totalBodies - spheres, 0, 0, spheres);
            }
        }

        // Spacecraft
        if (!scene.crafts.empty() && settings.drawBodies) {
            VkDescriptorSet craftSets[] = {frame.craftSet, m_textureSet};
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_craftPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_craftLayout, 0, 2, craftSets, 0, nullptr);
            const VkDeviceSize zero = 0;
            const size_t n = std::min<size_t>(scene.crafts.size(), kMaxCrafts);
            for (size_t i = 0; i < n; ++i) {
                const int mi = i < scene.craftModels.size() ? scene.craftModels[i] : -1;
                if (mi < 0 || mi >= (int)m_models.size() || m_models[mi].flame) continue;
                // Skip instances too far to cover a pixel: model extent / distance vs pixel angle.
                const glm::vec3 rel = glm::vec3(scene.crafts[i].model[3]);
                const float dist = glm::length(rel);
                const float size = glm::length(glm::vec3(scene.crafts[i].model[0])) * m_models[mi].extent;
                if (dist > 1e-30f && size / dist < 0.25f * (2.f * tanHalf / (float)ext.height)) continue; // the glint covers it
                const ModelGpu& m = m_models[mi];
                vkCmdBindVertexBuffers(cmd, 0, 1, &m.vb.buffer, &zero);
                vkCmdBindIndexBuffer(cmd, m.ib.buffer, 0, VK_INDEX_TYPE_UINT32);
                for (const auto& prim : m.primitives) {
                    CraftPushConstants cpc{};
                    cpc.viewProj = viewProj;
                    cpc.baseColor = prim.baseColor;
                    cpc.ids = glm::ivec4((int)i, prim.imageIndex, prim.normalImageIndex, prim.mrImageIndex);
                    cpc.material = glm::vec4(prim.metallic, prim.roughness, prim.normalScale,
                                             (float)prim.occlusionImageIndex);
                    cpc.shadowMat = scene.shadowMatrix;
                    cpc.shadowInfo = shadowInfo;
                    cpc.uvTransform = prim.uvTransform;
                    cpc.emissive = glm::vec4(prim.emissiveFactor, (float)prim.emissiveImageIndex);
                    vkCmdPushConstants(cmd, m_craftLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       sizeof(cpc), &cpc);
                    vkCmdDrawIndexed(cmd, prim.indexCount, 1, prim.firstIndex, 0, 0);
                }
            }
        }

        // Rocket exhaust: ray-marched volumes, blended after the opaque craft.
        if (!scene.crafts.empty() && settings.drawBodies) {
            const size_t n = std::min<size_t>(scene.crafts.size(), kMaxCrafts);
            const VkDeviceSize zero = 0;
            bool bound = false;
            for (size_t i = 0; i < n; ++i) {
                const int mi = i < scene.craftModels.size() ? scene.craftModels[i] : -1;
                if (mi < 0 || mi >= (int)m_models.size() || !m_models[mi].flame) continue;
                if (!bound) {
                    VkDescriptorSet sets[] = {frame.craftSet, m_textureSet};
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_plumePipeline);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_craftLayout, 0, 2, sets, 0, nullptr);
                    bound = true;
                }
                const ModelGpu& m = m_models[mi];
                vkCmdBindVertexBuffers(cmd, 0, 1, &m.vb.buffer, &zero);
                vkCmdBindIndexBuffer(cmd, m.ib.buffer, 0, VK_INDEX_TYPE_UINT32);
                CraftPushConstants cpc{};
                cpc.viewProj = viewProj;
                cpc.ids = glm::ivec4((int)i, -1, -1, -1);
                cpc.shadowInfo = shadowInfo;
                cpc.uvTransform = glm::vec4((float)time, 0.f, 1.f, 1.f);
                cpc.emissive = glm::vec4(0.f, 0.f, 0.f, -1.f);
                vkCmdPushConstants(cmd, m_craftLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(cpc), &cpc);
                for (const auto& prim : m.primitives) vkCmdDrawIndexed(cmd, prim.indexCount, 1, prim.firstIndex, 0, 0);
            }
        }

        // Unresolved spacecraft: points of light that hand over to the models as they grow.
        if (!scene.crafts.empty() && settings.drawBodies) {
            GlintPushConstants gpc{};
            gpc.viewProj = viewProj;
            gpc.params = glm::vec4((float)ext.width, (float)ext.height, 2.f * tanHalf / (float)ext.height, 1.2f);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_glintPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_glintLayout, 0, 1, &frame.craftSet, 0, nullptr);
            vkCmdPushConstants(cmd, m_glintLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(gpc), &gpc);
            vkCmdDraw(cmd, 6, (uint32_t)std::min<size_t>(scene.crafts.size(), kMaxCrafts), 0, 0);
        }

        // Stars
        if (m_starCount > 0 && settings.drawStars) {
            StarPushConstants spc{};
            spc.viewProj = viewProj;
            spc.camPos = glm::vec4(glm::vec3(camera.position), 0.f);
            spc.params = glm::vec4((float)ext.width, (float)ext.height, settings.starBrightness,
                                   settings.starMaxRadiusPx);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_starPipeline);
            VkDescriptorSet starSets[] = {m_starSet, frame.frameSet};
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_starLayout, 0, 2, starSets, 0, nullptr);
            vkCmdPushConstants(cmd, m_starLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(spc), &spc);
            vkCmdDraw(cmd, m_starCount * 6, 1, 0, 0);
        }

        // Constellation figures
        if (m_lineVertexCount > 0 && scene.constellationIntensity > 0.f && !scene.constellationHighlight.empty()) {
            StarPushConstants lpc{};
            lpc.viewProj = viewProj;
            lpc.camPos = glm::vec4(glm::vec3(camera.position), 0.f);
            lpc.params = glm::vec4(scene.constellationIntensity, 0.f, 0.f, 0.f);
            const VkDeviceSize zero = 0;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_linePipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_lineLayout, 0, 1, &frame.frameSet, 0,
                                    nullptr);
            vkCmdPushConstants(cmd, m_lineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(lpc), &lpc);
            vkCmdBindVertexBuffers(cmd, 0, 1, &m_lineVB.buffer, &zero);
            vkCmdDraw(cmd, m_lineVertexCount, 1, 0, 0);
        }

        // Dust motes around the camera
        if (scene.dustExtent > 0.f && scene.dustBrightness > 0.f) {
            DustPushConstants dpc{};
            dpc.viewProj = viewProj;
            dpc.phase = glm::vec4(scene.dustPhase, scene.dustExtent);
            dpc.velocity = glm::vec4(scene.cameraVelocity, (float)time);
            dpc.params = glm::vec4((float)ext.width, (float)ext.height, scene.dustBrightness, 0.f);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_dustPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_dustLayout, 0, 1, &frame.frameSet, 0,
                                    nullptr);
            vkCmdPushConstants(cmd, m_dustLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(dpc), &dpc);
            vkCmdDraw(cmd, kDustCount * 6, 1, 0, 0);
        }

        // Music nebula around the camera
        if (scene.cloudStrength > 0.f && scene.dustExtent > 0.f) {
            ViewPushConstants cpc = view;
            const float cellExtent = scene.dustExtent * 2.5f;
            cpc.camPos = glm::vec4(scene.cloudPhase, 0.f);
            cpc.look = glm::vec4(scene.cloudStrength, cellExtent, scene.section, scene.level);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cloudPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cloudLayout, 0, 1, &frame.frameSet, 0,
                                    nullptr);
            vkCmdPushConstants(cmd, m_cloudLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(cpc), &cpc);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
        vkCmdEndRendering(cmd);
    }

    // ---- Post resolve: TAA + Sun glare (compute) -------------------------------------------
    gfx::transitionImage(cmd, m_hdr.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    gfx::transitionImage(cmd, m_depth.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    {
        // Previous frame: bloom + tonemap read m_post, the post pass read the other history buffer.
        for (gfx::Image* img : {&m_post, &m_history[0], &m_history[1]})
            gfx::transitionImage(cmd, img->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        PostPushConstants ppc{};
        ppc.prevViewProj = m_historyValid ? m_prevViewProj : viewProjClean;
        ppc.viewProj = viewProjClean;
        ppc.camRight = glm::vec4(r, 0.f);
        ppc.camUp = glm::vec4(u, 0.f);
        ppc.camForward = glm::vec4(f, 0.f);
        ppc.params = glm::vec4(tanHalf, aspect, kNearPlane, settings.taa && m_historyValid ? 0.85f : 0.f);
        ppc.camDelta = glm::vec4(scene.cameraOwnDelta, 0.f);
        ppc.sun = glm::vec4(scene.sunPosRel, scene.sunRadius);
        ppc.glare = glm::vec4(settings.sunGlare * 0.6f, jitterNdc.x * 0.5f, jitterNdc.y * 0.5f, settings.motionBlur);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_postPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_postLayout, 0, 1, &m_postSets[m_historyIndex], 0, nullptr);
        vkCmdPushConstants(cmd, m_postLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ppc), &ppc);
        vkCmdDispatch(cmd, (ext.width + 7) / 8, (ext.height + 7) / 8, 1);
        gfx::transitionImage(cmd, m_post.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        m_historyIndex = 1 - m_historyIndex;
        m_historyValid = settings.taa;
        m_prevViewProj = viewProjClean;
        m_prevCamPos = camera.position;
        ++m_frameSerial;
    }

    // ---- Bloom (compute) --------------------------------------------------------------------
    recordBloom(cmd, settings);
    m_diagThisFrame = m_diagRequested;
    if (m_diagThisFrame) {
        ensureDiagBuffer();
        recordDiagnosticCopy(cmd);
        m_diagRequested = false;
        m_diagPending = true;
    }

    // ---- Tonemap + UI -> swapchain ---------------------------------------------------------
    gfx::transitionImage(cmd, m_swapchain.image(imageIndex), VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    {
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = m_swapchain.view(imageIndex);
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = scissor;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &ri);

        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonemapPipeline);
        VkDescriptorSet tmSets[] = {m_tonemapSet, frame.frameSet};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_tonemapLayout, 0, 2, tmSets, 0, nullptr);
        TonemapPushConstants tpc{{settings.exposure, settings.bloomStrength, m_swapchain.isHdr() ? 1.f : 0.f, 0.f},
                                 {settings.hdrPaperWhite, settings.hdrPeak, 0.f, 0.f}};
        vkCmdPushConstants(cmd, m_tonemapLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(tpc), &tpc);
        vkCmdDraw(cmd, 3, 1, 0, 0);

        if (m_diagThisFrame) {
            // Snapshot the swapchain between the tonemap and the UI.
            vkCmdEndRendering(cmd);
            const VkDeviceSize hdrBytes = (VkDeviceSize)m_hdr.extent.width * m_hdr.extent.height * 8;
            const VkDeviceSize bloomBytes =
                (VkDeviceSize)m_bloomMips[0].image.extent.width * m_bloomMips[0].image.extent.height * 8;
            recordSwapchainCopy(cmd, m_swapchain.image(imageIndex), hdrBytes + bloomBytes);
            color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            vkCmdBeginRendering(cmd, &ri);
            vkCmdSetViewport(cmd, 0, 1, &viewport);
            vkCmdSetScissor(cmd, 0, 1, &scissor);
        }

        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        vkCmdEndRendering(cmd);

        if (m_diagThisFrame) {
            const VkDeviceSize hdrBytes = (VkDeviceSize)m_hdr.extent.width * m_hdr.extent.height * 8;
            const VkDeviceSize bloomBytes =
                (VkDeviceSize)m_bloomMips[0].image.extent.width * m_bloomMips[0].image.extent.height * 8;
            const VkDeviceSize swapBytes = (VkDeviceSize)ext.width * ext.height * 4;
            recordSwapchainCopy(cmd, m_swapchain.image(imageIndex), hdrBytes + bloomBytes + swapBytes);
            if (m_volumeBuffer.buffer) {
                VkBufferCopy copy{0, hdrBytes + bloomBytes + swapBytes * 2, m_volumeBlob.size()};
                vkCmdCopyBuffer(cmd, m_volumeBuffer.buffer, m_diagBuffer.buffer, 1, &copy);
            }
        }
    }

    gfx::transitionImage(cmd, m_swapchain.image(imageIndex), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT,
                         VK_ACCESS_2_NONE);

    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_timestampPool, m_frameIndex * 2 + 1);
}

void Renderer::ensureDiagBuffer() {
    const VkExtent2D hdrExt = m_hdr.extent, bloomExt = m_bloomMips[0].image.extent, swapExt = m_swapchain.extent();
    const VkDeviceSize bytes = (VkDeviceSize)hdrExt.width * hdrExt.height * 8 +
                               (VkDeviceSize)bloomExt.width * bloomExt.height * 8 +
                               (VkDeviceSize)swapExt.width * swapExt.height * 4 * 2 + m_volumeBlob.size();
    if (m_diagBuffer.buffer && m_diagBuffer.size >= bytes) return;
    m_diagBuffer.destroy(*m_ctx);
    m_diagBuffer.create(*m_ctx, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_AUTO,
                        VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT);
}

void Renderer::recordSwapchainCopy(VkCommandBuffer cmd, VkImage image, VkDeviceSize offset) {
    gfx::transitionImage(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};
    copy.bufferOffset = offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {m_swapchain.extent().width, m_swapchain.extent().height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_diagBuffer.buffer, 1, &copy);
    gfx::transitionImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

void Renderer::recordDiagnosticCopy(VkCommandBuffer cmd) {
    const VkExtent2D hdrExt = m_hdr.extent, bloomExt = m_bloomMips[0].image.extent;
    const VkDeviceSize hdrBytes = (VkDeviceSize)hdrExt.width * hdrExt.height * 8;

    gfx::transitionImage(cmd, m_hdr.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy hdrCopy{};
    hdrCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    hdrCopy.imageExtent = {hdrExt.width, hdrExt.height, 1};
    vkCmdCopyImageToBuffer(cmd, m_hdr.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_diagBuffer.buffer, 1, &hdrCopy);
    gfx::transitionImage(cmd, m_hdr.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                         VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);

    gfx::transitionImage(cmd, m_bloomMips[0].image.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy bloomCopy{};
    bloomCopy.bufferOffset = hdrBytes;
    bloomCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bloomCopy.imageExtent = {bloomExt.width, bloomExt.height, 1};
    vkCmdCopyImageToBuffer(cmd, m_bloomMips[0].image.image, VK_IMAGE_LAYOUT_GENERAL, m_diagBuffer.buffer, 1,
                           &bloomCopy);
}

void Renderer::reportDiagnostic() {
    m_diagPending = false;
    VmaAllocationInfo info{};
    vmaGetAllocationInfo(m_ctx->allocator(), m_diagBuffer.allocation, &info);
    vmaInvalidateAllocation(m_ctx->allocator(), m_diagBuffer.allocation, 0, VK_WHOLE_SIZE);
    const uint16_t* data = static_cast<const uint16_t*>(info.pMappedData);

    auto halfToFloat = [](uint16_t h) {
        uint32_t sign = (h & 0x8000u) << 16, exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
        uint32_t bits;
        if (exp == 0) bits = sign | (mant ? [&] { // subnormal
                                  uint32_t e = 127 - 15 + 1, m = mant;
                                  while (!(m & 0x400)) { m <<= 1; --e; }
                                  return (e << 23) | ((m & 0x3FF) << 13);
                              }() : 0u);
        else if (exp == 31) bits = sign | 0x7F800000u | (mant << 13);
        else bits = sign | ((exp + 112) << 23) | (mant << 13);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    };
    auto stats = [&](const char* name, const uint16_t* px, size_t count) {
        double sum[4] = {}, mx[4] = {-1e30, -1e30, -1e30, -1e30}, mn[4] = {1e30, 1e30, 1e30, 1e30};
        size_t bad = 0;
        for (size_t i = 0; i < count; ++i)
            for (int c = 0; c < 4; ++c) {
                float v = halfToFloat(px[i * 4 + c]);
                if (!std::isfinite(v)) { ++bad; continue; }
                sum[c] += v;
                mx[c] = std::max(mx[c], (double)v);
                mn[c] = std::min(mn[c], (double)v);
            }
        LOG_INFO("[diag] {}: non-finite {}  mean({:.4g} {:.4g} {:.4g} {:.4g})  max({:.4g} {:.4g} {:.4g} {:.4g})  "
                 "min({:.4g} {:.4g} {:.4g} {:.4g})",
                 name, bad, sum[0] / count, sum[1] / count, sum[2] / count, sum[3] / count, mx[0], mx[1], mx[2],
                 mx[3], mn[0], mn[1], mn[2], mn[3]);
    };
    const size_t hdrPixels = (size_t)m_hdr.extent.width * m_hdr.extent.height;
    const size_t bloomPixels = (size_t)m_bloomMips[0].image.extent.width * m_bloomMips[0].image.extent.height;
    stats("hdr", data, hdrPixels);
    stats("bloom0", data + hdrPixels * 4, bloomPixels);

    // Dump the two swapchain snapshots and a CPU-tonemapped HDR as PPM images next to the executable.
    const uint32_t sw = m_swapchain.extent().width, sh = m_swapchain.extent().height;
    const uint8_t* swapA = reinterpret_cast<const uint8_t*>(data) + hdrPixels * 8 + bloomPixels * 8;
    const uint8_t* swapB = swapA + (size_t)sw * sh * 4;
    auto writePpm = [&](const std::string& name, auto pixel) {
        std::filesystem::path path = gfx::shaderDirectory().parent_path() / name;
        FILE* f = std::fopen(path.string().c_str(), "wb");
        if (!f) return;
        std::fprintf(f, "P6\n%u %u\n255\n", sw, sh);
        std::vector<uint8_t> row(sw * 3);
        for (uint32_t y = 0; y < sh; ++y) {
            for (uint32_t x = 0; x < sw; ++x) pixel(x, y, &row[x * 3]);
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        LOG_INFO("[diag] wrote {}", path.string());
    };
    // Volume SSBO integrity check.
    if (!m_volumeBlob.empty()) {
        const uint8_t* gpu = swapB + (size_t)sw * sh * 4;
        size_t firstBad = SIZE_MAX, badCount = 0;
        for (size_t i = 0; i < m_volumeBlob.size(); ++i)
            if (gpu[i] != m_volumeBlob[i]) {
                if (firstBad == SIZE_MAX) firstBad = i;
                ++badCount;
            }
        if (badCount == 0) LOG_INFO("[diag] volume buffer intact ({} bytes)", m_volumeBlob.size());
        else {
            LOG_ERROR("[diag] volume buffer CORRUPTED: {} bytes differ, first at offset {}", badCount, firstBad);
            const float* a = reinterpret_cast<const float*>(gpu);
            const float* b = reinterpret_cast<const float*>(m_volumeBlob.data());
            for (size_t i = 0; i < 16; ++i) LOG_ERROR("[diag]   header float {}: gpu {} expected {}", i, a[i], b[i]);
        }
    }

    const std::string tag = std::to_string(m_diagSerial++);
    writePpm("diag_" + tag + "_swap_tonemap.ppm", [&](uint32_t x, uint32_t y, uint8_t* out) {
        const uint8_t* p = swapA + ((size_t)y * sw + x) * 4; // BGRA
        out[0] = p[2]; out[1] = p[1]; out[2] = p[0];
    });
    writePpm("diag_" + tag + "_swap_final.ppm", [&](uint32_t x, uint32_t y, uint8_t* out) {
        const uint8_t* p = swapB + ((size_t)y * sw + x) * 4;
        out[0] = p[2]; out[1] = p[1]; out[2] = p[0];
    });
    writePpm("diag_" + tag + "_hdr.ppm", [&](uint32_t x, uint32_t y, uint8_t* out) {
        const uint16_t* p = data + ((size_t)y * sw + x) * 4;
        for (int c = 0; c < 3; ++c) {
            float v = halfToFloat(p[c]);
            v = std::clamp((v * (2.51f * v + 0.03f)) / (v * (2.43f * v + 0.59f) + 0.14f), 0.f, 1.f);
            out[c] = (uint8_t)(std::pow(v, 1.f / 2.2f) * 255.f + 0.5f);
        }
    });
}

void Renderer::recordBloom(VkCommandBuffer cmd, const RenderSettings& settings) {
    const uint32_t n = (uint32_t)m_bloomMips.size();
    if (n == 0) return;

    auto computeBarrier = [&](VkImage image) {
        gfx::transitionImage(cmd, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    };
    auto dispatch = [&](const BloomMip& mip) {
        vkCmdDispatch(cmd, (mip.image.extent.width + 7) / 8, (mip.image.extent.height + 7) / 8, 1);
    };

    // Previous frame: the upsample chain read every mip in compute and the tonemap read mip 0 in fragment.
    for (auto& mip : m_bloomMips)
        gfx::transitionImage(cmd, mip.image.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                             VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
                             VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_bloomDownPipeline);
    for (uint32_t i = 0; i < n; ++i) {
        VkExtent2D src = i == 0 ? m_hdr.extent : m_bloomMips[i - 1].image.extent;
        BloomPushConstants pc{{1.f / src.width, 1.f / src.height, settings.bloomKnee, i == 0 ? 1.f : 0.f}};
        vkCmdPushConstants(cmd, m_bloomLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_bloomLayout, 0, 1, &m_bloomMips[i].downSet,
                                0, nullptr);
        dispatch(m_bloomMips[i]);
        computeBarrier(m_bloomMips[i].image.image);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_bloomUpPipeline);
    for (int i = (int)n - 2; i >= 0; --i) {
        VkExtent2D src = m_bloomMips[i + 1].image.extent;
        BloomPushConstants pc{{1.f / src.width, 1.f / src.height, settings.bloomRadius, 0.f}};
        vkCmdPushConstants(cmd, m_bloomLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_bloomLayout, 0, 1, &m_bloomMips[i].upSet, 0,
                                nullptr);
        dispatch(m_bloomMips[i]);
        computeBarrier(m_bloomMips[i].image.image);
    }

    gfx::transitionImage(cmd, m_bloomMips[0].image.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

} // namespace space::render
