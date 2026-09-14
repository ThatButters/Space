#include "render/Dlss.h"
#include "core/Log.h"

#if SPACE_HAS_DLSS
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_vk.h>
#endif

namespace space::render {

const char* Dlss::qualityName(Quality q) {
    switch (q) {
    case Quality::Performance: return "performance";
    case Quality::Balanced: return "balanced";
    case Quality::Quality: return "quality";
    case Quality::UltraQuality: return "ultra quality";
    case Quality::DLAA: return "DLAA (native)";
    }
    return "?";
}

#if SPACE_HAS_DLSS

namespace {

// Identifies this application to NGX (a custom engine needs a project id, not an NVIDIA-issued one).
constexpr const char* kProjectId = "6f4c2e1a-7b3d-4c9e-9a21-5d8e0f3b6c47"; // any UUID: a custom engine has no NVIDIA-issued id
constexpr const char* kEngineVersion = "0.1";

NVSDK_NGX_FeatureDiscoveryInfo discoveryInfo() {
    NVSDK_NGX_FeatureDiscoveryInfo info{};
    info.SDKVersion = NVSDK_NGX_Version_API;
    info.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    info.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    info.Identifier.v.ProjectDesc.ProjectId = kProjectId;
    info.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    info.Identifier.v.ProjectDesc.EngineVersion = kEngineVersion;
    info.ApplicationDataPath = L".";
    info.FeatureInfo = nullptr;
    return info;
}

NVSDK_NGX_PerfQuality_Value toNgx(Dlss::Quality q) {
    switch (q) {
    case Dlss::Quality::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    case Dlss::Quality::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
    case Dlss::Quality::Quality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    case Dlss::Quality::UltraQuality: return NVSDK_NGX_PerfQuality_Value_UltraQuality;
    case Dlss::Quality::DLAA: return NVSDK_NGX_PerfQuality_Value_DLAA;
    }
    return NVSDK_NGX_PerfQuality_Value_MaxQuality;
}

NVSDK_NGX_Resource_VK resource(const Dlss::ImageRef& r, bool readWrite) {
    VkImageSubresourceRange range{r.aspect, 0, 1, 0, 1};
    return NVSDK_NGX_Create_ImageView_Resource_VK(r.view, r.image, range, r.format, r.width, r.height, readWrite);
}

} // namespace

void Dlss::instanceExtensions(std::vector<std::string>& out) {
    const NVSDK_NGX_FeatureDiscoveryInfo info = discoveryInfo();
    uint32_t count = 0;
    VkExtensionProperties* props = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&info, &count, &props)) || !props) return;
    for (uint32_t i = 0; i < count; ++i) out.emplace_back(props[i].extensionName);
}

void Dlss::deviceExtensions(VkInstance instance, VkPhysicalDevice pd, std::vector<std::string>& out) {
    const NVSDK_NGX_FeatureDiscoveryInfo info = discoveryInfo();
    uint32_t count = 0;
    VkExtensionProperties* props = nullptr;
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(instance, pd, &info, &count, &props)) || !props)
        return;
    for (uint32_t i = 0; i < count; ++i) out.emplace_back(props[i].extensionName);
    LOG_INFO("DLSS: {} device extensions requested", count);
}

bool Dlss::init(VkInstance instance, VkPhysicalDevice pd, VkDevice device) {
    m_device = device;
    NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_Init_with_ProjectID(kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, L".",
                                                            instance, pd, device, vkGetInstanceProcAddr, vkGetDeviceProcAddr);
    if (NVSDK_NGX_FAILED(r)) {
        LOG_WARN("DLSS: NGX init failed (0x{:x}); using TAA", (unsigned)r);
        return false;
    }
    NVSDK_NGX_Parameter* params = nullptr;
    r = NVSDK_NGX_VULKAN_GetCapabilityParameters(&params);
    if (NVSDK_NGX_FAILED(r) || !params) {
        LOG_WARN("DLSS: capability query failed (0x{:x}); using TAA", (unsigned)r);
        NVSDK_NGX_VULKAN_Shutdown1(device);
        return false;
    }
    int supported = 0, needsDriver = 0;
    NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_Available, &supported);
    NVSDK_NGX_Parameter_GetI(params, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
    if (!supported) {
        LOG_WARN("DLSS: super sampling not available on this GPU/driver{}; using TAA",
                 needsDriver ? " (driver update needed)" : "");
        NVSDK_NGX_VULKAN_DestroyParameters(params);
        NVSDK_NGX_VULKAN_Shutdown1(device);
        return false;
    }
    m_params = params;
    m_available = true;
    LOG_INFO("DLSS: super resolution available");
    return true;
}

void Dlss::shutdown() {
    releaseFeature();
    if (m_params) NVSDK_NGX_VULKAN_DestroyParameters((NVSDK_NGX_Parameter*)m_params);
    m_params = nullptr;
    if (m_available && m_device) NVSDK_NGX_VULKAN_Shutdown1(m_device);
    m_available = false;
    m_device = VK_NULL_HANDLE;
}

bool Dlss::optimalRenderSize(uint32_t outW, uint32_t outH, Quality q, uint32_t& renderW, uint32_t& renderH) const {
    if (!m_available) return false;
    uint32_t maxW = 0, maxH = 0, minW = 0, minH = 0;
    float sharpness = 0.f;
    const NVSDK_NGX_Result r = NGX_DLSS_GET_OPTIMAL_SETTINGS((NVSDK_NGX_Parameter*)m_params, outW, outH, toNgx(q), &renderW,
                                                             &renderH, &maxW, &maxH, &minW, &minH, &sharpness);
    if (NVSDK_NGX_FAILED(r) || renderW == 0 || renderH == 0) return false;
    return true;
}

bool Dlss::createFeature(VkCommandBuffer cmd, uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH, Quality q) {
    if (!m_available) return false;
    releaseFeature();
    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth = renderW;
    cp.Feature.InHeight = renderH;
    cp.Feature.InTargetWidth = outW;
    cp.Feature.InTargetHeight = outH;
    cp.Feature.InPerfQualityValue = toNgx(q);
    // Linear HDR input, motion vectors at render resolution and jittered like the colour, reversed depth;
    // DLSS meters its own exposure from the frame.
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              NVSDK_NGX_DLSS_Feature_Flags_MVJittered | NVSDK_NGX_DLSS_Feature_Flags_DepthInverted |
                              NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    NVSDK_NGX_Handle* handle = nullptr;
    const NVSDK_NGX_Result r = NGX_VULKAN_CREATE_DLSS_EXT1(m_device, cmd, 1, 1, &handle, (NVSDK_NGX_Parameter*)m_params, &cp);
    if (NVSDK_NGX_FAILED(r) || !handle) {
        LOG_WARN("DLSS: feature creation failed (0x{:x})", (unsigned)r);
        return false;
    }
    m_feature = handle;
    LOG_INFO("DLSS {}: {}x{} -> {}x{}", qualityName(q), renderW, renderH, outW, outH);
    return true;
}

void Dlss::releaseFeature() {
    if (m_feature) NVSDK_NGX_VULKAN_ReleaseFeature((NVSDK_NGX_Handle*)m_feature);
    m_feature = nullptr;
}

bool Dlss::evaluate(VkCommandBuffer cmd, const EvalInputs& in) {
    if (!m_feature) return false;
    NVSDK_NGX_Resource_VK color = resource(in.color, false);
    NVSDK_NGX_Resource_VK depth = resource(in.depth, false);
    NVSDK_NGX_Resource_VK motion = resource(in.motion, false);
    NVSDK_NGX_Resource_VK output = resource(in.output, true);
    NVSDK_NGX_VK_DLSS_Eval_Params ep{};
    ep.Feature.pInColor = &color;
    ep.Feature.pInOutput = &output;
    ep.pInDepth = &depth;
    ep.pInMotionVectors = &motion;
    ep.InJitterOffsetX = in.jitterX;
    ep.InJitterOffsetY = in.jitterY;
    ep.InRenderSubrectDimensions = {in.color.width, in.color.height};
    ep.InReset = in.reset ? 1 : 0;
    ep.InMVScaleX = 1.f;
    ep.InMVScaleY = 1.f;
    ep.InPreExposure = 1.f;
    ep.InExposureScale = 1.f;
    ep.InFrameTimeDeltaInMsec = in.frameDeltaMs;
    const NVSDK_NGX_Result r = NGX_VULKAN_EVALUATE_DLSS_EXT(cmd, (NVSDK_NGX_Handle*)m_feature, (NVSDK_NGX_Parameter*)m_params, &ep);
    if (NVSDK_NGX_FAILED(r)) {
        LOG_WARN("DLSS: evaluate failed (0x{:x})", (unsigned)r);
        return false;
    }
    return true;
}

#else // no SDK: stubs

void Dlss::instanceExtensions(std::vector<std::string>&) {}
void Dlss::deviceExtensions(VkInstance, VkPhysicalDevice, std::vector<std::string>&) {}
bool Dlss::init(VkInstance, VkPhysicalDevice, VkDevice) {
    LOG_INFO("DLSS: built without the NGX SDK; using TAA");
    return false;
}
void Dlss::shutdown() {}
bool Dlss::optimalRenderSize(uint32_t, uint32_t, Quality, uint32_t&, uint32_t&) const { return false; }
bool Dlss::createFeature(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t, Quality) { return false; }
void Dlss::releaseFeature() {}
bool Dlss::evaluate(VkCommandBuffer, const EvalInputs&) { return false; }

#endif

} // namespace space::render
