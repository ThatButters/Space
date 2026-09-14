#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <vector>

namespace space::render {

// DLSS Super Resolution through the NGX SDK. All NGX types stay inside Dlss.cpp; without the SDK
// (SPACE_HAS_DLSS undefined) every call reports "not available" and the engine keeps its own TAA.
class Dlss {
public:
    enum class Quality { Performance, Balanced, Quality, UltraQuality, DLAA };
    static const char* qualityName(Quality q);

    // Extensions DLSS needs, to enable before the instance / device exist (filtered by the context).
    static void instanceExtensions(std::vector<std::string>& out);
    static void deviceExtensions(VkInstance instance, VkPhysicalDevice pd, std::vector<std::string>& out);

    bool init(VkInstance instance, VkPhysicalDevice pd, VkDevice device);
    void shutdown();
    bool available() const { return m_available; }

    // The internal render size DLSS wants for a display size and quality.
    bool optimalRenderSize(uint32_t outW, uint32_t outH, Quality q, uint32_t& renderW, uint32_t& renderH) const;

    // (Re)creates the feature for these sizes. Records into cmd (a one-shot command buffer is fine).
    bool createFeature(VkCommandBuffer cmd, uint32_t renderW, uint32_t renderH, uint32_t outW, uint32_t outH, Quality q);
    void releaseFeature();
    bool hasFeature() const { return m_feature != nullptr; }

    struct ImageRef {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0, height = 0;
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    };
    struct EvalInputs {
        ImageRef color, depth, motion, output;
        float jitterX = 0.f, jitterY = 0.f; // render pixels, the offset the projection was translated by
        bool reset = false;
        float frameDeltaMs = 16.f;
    };
    bool evaluate(VkCommandBuffer cmd, const EvalInputs& in);

private:
    bool m_available = false;
    VkDevice m_device = VK_NULL_HANDLE;
    void* m_params = nullptr;  // NVSDK_NGX_Parameter*
    void* m_feature = nullptr; // NVSDK_NGX_Handle*
};

} // namespace space::render
