#pragma once
#include "gfx/Buffer.h"
#include "gfx/Image.h"
#include "gfx/Swapchain.h"
#include "gfx/GltfModel.h"
#include "gfx/Texture.h"
#include "scene/Craft.h"
#include "scene/SolarSystem.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <array>
#include <vector>

namespace space {
class Window;
class Camera;
struct StarGpu;
struct VolumeScene;
struct LineVertex;
namespace gfx {
class Context;
}
} // namespace space

struct ImFont;

namespace space::render {

// Tunables surfaced in the debug UI.
struct RenderSettings {
    float exposure = 1.0f;
    float bloomStrength = 0.35f;
    float bloomKnee = 1.0f;   // luminance where bloom starts to pick up energy
    float bloomRadius = 1.0f; // upsample tent radius scale
    float starBrightness = 1.0f; // 3D star flux multiplier
    float starMaxRadiusPx = 7.f;
    float skyDensity = 0.35f;    // background (infinite) star density
    float nebulaIntensity = 0.18f; // background sky haze
    float galaxyGlow = 0.55f;     // volumetric medium emission
    float galaxyDust = 1.0f;      // volumetric medium absorption
    float nebulaGain = 1.0f;      // local nebulae emission
    bool hdrOutput = true;      // present scRGB when the display supports it
    float hdrPaperWhite = 200.f; // nits for scene value 1.0
    float hdrPeak = 1000.f;      // nits the shoulder rolls off toward
    bool volumetrics = true;
    bool drawSky = true;
    bool drawStars = true;
    bool drawBodies = true;
    bool vsync = true;
    bool taa = true;            // temporal anti-aliasing
    float sunGlare = 1.0f;      // glare strength around the Sun (0 = off)
};

// GPU layout of the per-frame shared block (shaders/frame.glsl).
struct FrameGpu {
    float bands[32];
    float features[4];    // bass, mid, treble, beat envelope
    float misc[4];        // beat age, level, reactivity, time
    float viewForward[4];
    float mood[4];        // rgb pulse colour, w saturation
    float highlight[128];
};

// Everything that changes per frame besides the camera.
struct FrameScene {
    std::vector<BodyGpu> bodies; // spheres first, then rings
    uint32_t sphereCount = 0;
    glm::vec3 sunPosRel{0.f}; // camera-relative
    float sunRadiance = 1.f;
    float sunRadius = 0.f;    // world units
    // The camera's own movement this frame (world units), excluding riding along with a body or craft:
    // what the temporal resolve reprojects the previous frame with.
    glm::vec3 cameraOwnDelta{0.f};
    // Spacecraft instances (index = craft index) and which model each one uses.
    std::vector<CraftGpu> crafts;
    std::vector<int> craftModels; // model index per craft, -1 = skip
    // Constellation figures: per-figure highlight in [0,1]; empty = do not draw.
    std::vector<float> constellationHighlight;
    float constellationIntensity = 0.f; // 0 hides the lines
    // Music features (all 0 when nothing plays or reactivity is off).
    float audioBands[32] = {};
    float bass = 0.f, mid = 0.f, treble = 0.f, beat = 0.f, beatAge = 100.f, level = 0.f, audioReact = 0.f;
    glm::vec3 moodColor{1.f};
    float waveStrength = 0.f; // optional slow tide on energy swells (0 = off)
    float cloudStrength = 0.f; // music nebula density (0 = off)
    float auroraStrength = 0.f;
    float starTint = 0.5f; // how far energetic music pulls star colours toward the mood
    float section = 0.5f;
    // Local dust field: camera velocity (world units / s) and field extent (world units); 0 = off.
    glm::vec3 cameraVelocity{0.f};
    float dustExtent = 0.f;
    float dustBrightness = 1.f;
    // Sub-cell phase of the camera inside the dust field (computed in double on the CPU).
    glm::vec3 dustPhase{0.f};
    glm::vec3 cloudPhase{0.f}; // same idea, for the (larger) music-cloud cell
    // Spacecraft shadows: a sun-aligned orthographic depth map around the craft nearest the camera.
    bool shadowEnabled = false;
    glm::mat4 shadowMatrix{1.f};    // camera-relative world -> shadow clip (xy -1..1, reversed depth)
    float shadowDepthBias = 0.f;    // in shadow depth units
    std::vector<int> shadowCasters; // craft indices drawn into the map
    // Close-range surface detail (planet.frag): a surface point near the camera, in double precision.
    int detailBody = -1;
    glm::vec3 detailAnchorWorld{0.f}; // camera-relative
    glm::vec3 detailAnchorLocal{0.f}; // body-local metres modulo 4096
    // Local terrain patch around the anchor (planet.frag): uv of the anchor and uv per metre, plus the
    // body-local east / north directions at the anchor.
    glm::vec4 patchAnchor{0.f};
    glm::vec4 patchEast{0.f};
    glm::vec4 patchNorth{0.f};
};

// Rotation-only view * reversed-Z infinite projection, the same matrix every 3D pass uses.
glm::mat4 makeViewProj(const Camera& camera, float aspect);

// Frame orchestration (all into one HDR RGBA16F target with a reversed-Z depth buffer):
//   sky        procedural infinite background (no depth)
//   volumes    galactic medium + nebulae raymarch, premultiplied over (no depth)
//   bodies     sun / planets / moons (depth write), rings (depth test, blended)
//   stars      catalogue point sprites (depth test), additive
//   bloom      compute mip chain
//   tonemap    -> sRGB swapchain, ImGui on top
class Renderer {
public:
    void init(gfx::Context& ctx, Window& window);
    void shutdown();

    // Upload a star catalogue (replaces any previous one). Must be called outside a frame.
    void setStars(const std::vector<StarGpu>& stars);
    // Upload the volumetric scene (galactic medium + nebulae).
    void setVolumes(const VolumeScene& volumes);

    // Upload constellation line segments (pairs of vertices).
    void setConstellationLines(const std::vector<LineVertex>& vertices);

    // Uploads a glTF model (mesh + embedded textures); returns its index or -1.
    int addModel(const gfx::ModelData& model);
    float modelExtent(int index) const { return m_models[index].extent; }
    float modelMinY(int index) const { return m_models[index].minY; }

    // Registers a texture in the bindless array; returns its index (or -1 if the array is full).
    // clampU: for tiles of a larger map (no longitude wrap inside a tile).
    int addTexture(gfx::Texture&& texture, bool clampU = false);
    // Swaps the texture at an index (live data refreshed while running). Waits for the GPU.
    void replaceTexture(int index, gfx::Texture&& texture);
    static constexpr uint32_t kMaxTextures = 1024; // shaders declare uTex[1024]

    // Call once per frame: builds ImGui frame state so the app can submit UI between begin/end.
    void beginFrame();
    void endFrame(const Camera& camera, double timeSeconds, const RenderSettings& settings, const FrameScene& scene);

    void requestSwapchainRecreate() { m_recreateSwapchain = true; }
    float lastGpuFrameMs() const { return m_gpuFrameMs; }
    bool hdrAvailable() const { return m_swapchain.hdrAvailable(); }
    bool hdrActive() const { return m_swapchain.isHdr(); }

    // Debug: on the next frame, read the HDR target and bloom mip 0 back and log channel statistics.
    void requestDiagnostic() { m_diagRequested = true; }

    // UI fonts (Segoe UI when available): body text and a large display face for titles.
    ImFont* uiFont() const { return m_uiFont; }
    ImFont* titleFont() const { return m_titleFont; }
    float uiScale() const { return m_uiScale; }

    static constexpr uint32_t kMaxBodies = 64;

private:
    static constexpr uint32_t kFramesInFlight = 2;

    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
        VkFence inFlight = VK_NULL_HANDLE;
        bool submitted = false; // timestamps are only readable after the slot has been used once
        gfx::Buffer bodyBuffer; // host-visible, persistently mapped
        void* bodyMapped = nullptr;
        VkDescriptorSet bodySet = VK_NULL_HANDLE;
        gfx::Buffer craftBuffer; // per-frame spacecraft instances
        void* craftMapped = nullptr;
        VkDescriptorSet craftSet = VK_NULL_HANDLE;
        gfx::Buffer frameBuffer; // per-frame FrameGpu block (music + constellation highlights)
        void* frameMapped = nullptr;
        VkDescriptorSet frameSet = VK_NULL_HANDLE;
    };

    void createSwapchainDependent();
    void destroySwapchainDependent();
    void recreateSwapchain();
    void createPipelines();
    void createMeshes();
    void initImGui();
    void updateTonemapDescriptor();
    void createBloomChain();
    void destroyBloomChain();
    void recordFrame(VkCommandBuffer cmd, uint32_t imageIndex, const Camera& camera, double time,
                     const RenderSettings& settings, const FrameScene& scene);
    void recordBloom(VkCommandBuffer cmd, const RenderSettings& settings);
    void recordShadowPass(VkCommandBuffer cmd, const FrameScene& scene);
    int addTextureWith(gfx::Texture&& texture, VkSampler sampler);

    gfx::Context* m_ctx = nullptr;
    Window* m_window = nullptr;

    gfx::Swapchain m_swapchain;
    std::vector<VkSemaphore> m_renderFinished; // one per swapchain image
    std::array<Frame, kFramesInFlight> m_frames{};
    uint32_t m_frameIndex = 0;
    bool m_recreateSwapchain = false;
    bool m_vsync = true;
    bool m_preferHdr = true;
    void createSwapchainFormatDependent(); // tonemap pipeline + ImGui backend
    void destroySwapchainFormatDependent();

    gfx::Image m_hdr;
    gfx::Image m_depth;
    // Post resolve (TAA + glare): output read by bloom and tonemap, history ping-pong.
    gfx::Image m_post;
    gfx::Image m_history[2];
    int m_historyIndex = 0;
    bool m_historyValid = false;
    uint32_t m_frameSerial = 0;
    glm::mat4 m_prevViewProj{1.f};
    glm::dvec3 m_prevCamPos{0.0};
    VkDescriptorSetLayout m_postSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_postPool = VK_NULL_HANDLE;
    VkDescriptorSet m_postSets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipelineLayout m_postLayout = VK_NULL_HANDLE;
    VkPipeline m_postPipeline = VK_NULL_HANDLE;
    void createPostResources();
    void destroyPostResources();
    VkSampler m_linearSampler = VK_NULL_HANDLE;

    // Bloom mip chain (half res downward), all kept in GENERAL layout for compute access.
    struct BloomMip {
        gfx::Image image;
        VkDescriptorSet downSet = VK_NULL_HANDLE; // src = previous mip (or HDR), dst = this mip
        VkDescriptorSet upSet = VK_NULL_HANDLE;   // src = next mip, dst = this mip
    };
    std::vector<BloomMip> m_bloomMips;
    VkDescriptorPool m_bloomPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_bloomSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_bloomLayout = VK_NULL_HANDLE;
    VkPipeline m_bloomDownPipeline = VK_NULL_HANDLE;
    VkPipeline m_bloomUpPipeline = VK_NULL_HANDLE;

    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_tonemapSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet m_tonemapSet = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_ssboFragSetLayout = VK_NULL_HANDLE;   // one SSBO, fragment stage (volumes)
    VkDescriptorSetLayout m_ssboVertexSetLayout = VK_NULL_HANDLE; // one SSBO, vertex stage (stars)
    VkDescriptorSetLayout m_ssboBothSetLayout = VK_NULL_HANDLE;   // one SSBO, vertex + fragment (bodies)

    VkPipelineLayout m_skyLayout = VK_NULL_HANDLE;
    VkPipeline m_skyPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_tonemapLayout = VK_NULL_HANDLE;
    VkPipeline m_tonemapPipeline = VK_NULL_HANDLE;

    // 3D star catalogue pass
    gfx::Buffer m_starBuffer;
    uint32_t m_starCount = 0;
    VkDescriptorSet m_starSet = VK_NULL_HANDLE;
    VkPipelineLayout m_starLayout = VK_NULL_HANDLE;
    VkPipeline m_starPipeline = VK_NULL_HANDLE;

    // Volumetrics
    gfx::Buffer m_volumeBuffer;
    std::vector<uint8_t> m_volumeBlob; // CPU copy, used by the diagnostic to detect GPU-side corruption
    uint32_t m_nebulaCount = 0;
    VkDescriptorSet m_volumeSet = VK_NULL_HANDLE;
    VkPipelineLayout m_volumeLayout = VK_NULL_HANDLE;
    VkPipeline m_volumePipeline = VK_NULL_HANDLE;

    // Bodies (sun, planets, moons, rings)
    std::vector<gfx::Texture> m_textures;
    VkSampler m_tileSampler = VK_NULL_HANDLE;
    VkSampler m_textureSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_textureSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool m_texturePool = VK_NULL_HANDLE;
    VkDescriptorSet m_textureSet = VK_NULL_HANDLE;
    gfx::Buffer m_sphereVB, m_sphereIB, m_ringVB, m_ringIB;
    uint32_t m_sphereIndexCount = 0, m_ringIndexCount = 0;
    VkPipelineLayout m_bodyLayout = VK_NULL_HANDLE;
    VkPipeline m_planetPipeline = VK_NULL_HANDLE;
    VkPipeline m_ringPipeline = VK_NULL_HANDLE;
    VkPipeline m_atmoPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_atmoLayout = VK_NULL_HANDLE;

    // Music nebula around the camera
    VkPipelineLayout m_cloudLayout = VK_NULL_HANDLE;
    VkPipeline m_cloudPipeline = VK_NULL_HANDLE;
    static constexpr float kAtmosphereShell = 1.045f;

    // Constellation lines
    gfx::Buffer m_lineVB;
    uint32_t m_lineVertexCount = 0;
    VkPipelineLayout m_lineLayout = VK_NULL_HANDLE;
    VkPipeline m_linePipeline = VK_NULL_HANDLE;

    // Spacecraft models
    struct ModelGpu {
        gfx::Buffer vb, ib;
        std::vector<gfx::ModelPrimitive> primitives; // imageIndex remapped to texture-array indices
        float extent = 1.f;
        float minY = 0.f;
        bool flame = false;
    };
    std::vector<ModelGpu> m_models;
    VkPipelineLayout m_craftLayout = VK_NULL_HANDLE;
    VkPipeline m_craftPipeline = VK_NULL_HANDLE;
    static constexpr uint32_t kMaxCrafts = 64;
    VkSampler m_modelSampler = VK_NULL_HANDLE;  // repeat in both axes
    VkSampler m_shadowSampler = VK_NULL_HANDLE; // nearest, clamped
    VkPipeline m_shadowPipeline = VK_NULL_HANDLE;
    VkPipelineLayout m_glintLayout = VK_NULL_HANDLE; // unresolved spacecraft as points of light
    VkPipeline m_glintPipeline = VK_NULL_HANDLE;
    VkPipeline m_plumePipeline = VK_NULL_HANDLE; // ray-marched rocket exhaust (craft layout)
    VkImage m_shadowImage = VK_NULL_HANDLE;     // owned by its m_textures entry
    VkImageView m_shadowView = VK_NULL_HANDLE;
    int m_shadowTextureIndex = -1;
    static constexpr uint32_t kShadowSize = 4096;

    // Interplanetary dust motes (procedural, no buffers)
    VkPipelineLayout m_dustLayout = VK_NULL_HANDLE;
    VkPipeline m_dustPipeline = VK_NULL_HANDLE;
    static constexpr uint32_t kDustCount = 7000;
    static constexpr uint32_t kMaxConstellations = 128;

    ImFont* m_uiFont = nullptr;
    ImFont* m_titleFont = nullptr;
    float m_uiScale = 1.f;
    VkQueryPool m_timestampPool = VK_NULL_HANDLE;
    float m_gpuFrameMs = 0.f;

    bool m_diagRequested = false;
    bool m_diagPending = false;
    bool m_diagThisFrame = false;
    uint32_t m_diagSerial = 0;
    gfx::Buffer m_diagBuffer; // host-visible readback: HDR, bloom mip 0, swapchain after tonemap, after UI
    void ensureDiagBuffer();
    void recordDiagnosticCopy(VkCommandBuffer cmd);
    void recordSwapchainCopy(VkCommandBuffer cmd, VkImage image, VkDeviceSize offset);
    void reportDiagnostic();
};

} // namespace space::render
