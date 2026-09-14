#define VMA_IMPLEMENTATION
#include "gfx/Context.h"
#include "gfx/VkCheck.h"
#include "core/Log.h"
#include "core/Window.h"

#include <cstring>
#include <vector>

namespace space::gfx {

namespace {

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        LOG_ERROR("[vk] {}", data->pMessage);
    else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        LOG_WARN("[vk] {}", data->pMessage);
    else
        LOG_TRACE("[vk] {}", data->pMessage);
    return VK_FALSE;
}

bool hasLayer(const char* name) {
    uint32_t count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (auto& l : layers)
        if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}

bool hasDeviceExtension(VkPhysicalDevice dev, const char* name) {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(dev, nullptr, &count, exts.data());
    for (auto& e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

} // namespace

void Context::init(const Window& window, bool enableValidation) {
    createInstance(window, enableValidation);
    m_surface = window.createSurface(m_instance);
    pickPhysicalDevice();
    createDevice();

    VmaAllocatorCreateInfo ai{};
    ai.vulkanApiVersion = VK_API_VERSION_1_3;
    ai.instance = m_instance;
    ai.physicalDevice = m_physicalDevice;
    ai.device = m_device;
    ai.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    VK_CHECK(vmaCreateAllocator(&ai, &m_allocator));

    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = m_queueFamily;
    VK_CHECK(vkCreateCommandPool(m_device, &pi, nullptr, &m_oneShotPool));

    LOG_INFO("Vulkan ready on {} (driver {}.{}.{})", m_props.deviceName, VK_VERSION_MAJOR(m_props.driverVersion),
             VK_VERSION_MINOR(m_props.driverVersion), VK_VERSION_PATCH(m_props.driverVersion));
}

void Context::shutdown() {
    if (m_device) vkDeviceWaitIdle(m_device);
    if (m_oneShotPool) vkDestroyCommandPool(m_device, m_oneShotPool, nullptr);
    if (m_allocator) vmaDestroyAllocator(m_allocator);
    if (m_device) vkDestroyDevice(m_device, nullptr);
    if (m_surface) vkDestroySurfaceKHR(m_instance, m_surface, nullptr);
    if (m_debugMessenger) {
        auto fn = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance,
                                                                             "vkDestroyDebugUtilsMessengerEXT");
        if (fn) fn(m_instance, m_debugMessenger, nullptr);
    }
    if (m_instance) vkDestroyInstance(m_instance, nullptr);
    *this = Context{};
}

void Context::waitIdle() const { vkDeviceWaitIdle(m_device); }

void Context::createInstance(const Window& window, bool enableValidation) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Space";
    app.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app.pEngineName = "Space";
    app.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> extensions = window.requiredInstanceExtensions();
    std::vector<const char*> layers;

    // HDR (scRGB / HDR10) surface colour spaces.
    {
        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
        for (auto& e : available)
            if (std::strcmp(e.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0)
                extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    }

    if (instanceExtensionHook) {
        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
        m_extraInstanceExts.clear();
        instanceExtensionHook(m_extraInstanceExts);
        for (const std::string& name : m_extraInstanceExts) {
            bool have = false, dup = false;
            for (auto& e : available) have |= name == e.extensionName;
            for (const char* e : extensions) dup |= name == e;
            if (!have) LOG_WARN("Instance extension {} not available", name);
            else if (!dup) extensions.push_back(name.c_str());
        }
    }

    const char* validationLayer = "VK_LAYER_KHRONOS_validation";
    if (enableValidation) {
        if (hasLayer(validationLayer)) {
            layers.push_back(validationLayer);
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            LOG_WARN("Validation requested but {} not available (install the Vulkan SDK)", validationLayer);
            enableValidation = false;
        }
    }

    VkDebugUtilsMessengerCreateInfoEXT dbg{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    dbg.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    dbg.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
    dbg.pfnUserCallback = debugCallback;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = (uint32_t)extensions.size();
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount = (uint32_t)layers.size();
    ci.ppEnabledLayerNames = layers.data();
    if (enableValidation) ci.pNext = &dbg;
    VK_CHECK(vkCreateInstance(&ci, nullptr, &m_instance));

    if (enableValidation) {
        auto fn = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(m_instance,
                                                                            "vkCreateDebugUtilsMessengerEXT");
        if (fn) VK_CHECK(fn(m_instance, &dbg, nullptr, &m_debugMessenger));
        LOG_INFO("Vulkan validation layers enabled");
    }
}

void Context::pickPhysicalDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(m_instance, &count, nullptr);
    if (count == 0) throw std::runtime_error("No Vulkan-capable GPU found");
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(m_instance, &count, devices.data());

    int bestScore = -1;
    for (auto dev : devices) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(dev, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;
        if (!hasDeviceExtension(dev, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

        // Need a queue family that does graphics and can present to our surface.
        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> families(qcount);
        vkGetPhysicalDeviceQueueFamilyProperties(dev, &qcount, families.data());
        int family = -1;
        for (uint32_t i = 0; i < qcount; ++i) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(dev, i, m_surface, &present);
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                family = (int)i;
                break;
            }
        }
        if (family < 0) continue;

        int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1000 : 10;
        if (score > bestScore) {
            bestScore = score;
            m_physicalDevice = dev;
            m_props = props;
            m_queueFamily = (uint32_t)family;
        }
    }
    if (!m_physicalDevice) throw std::runtime_error("No GPU with Vulkan 1.3 + presentation support found");
}

void Context::createDevice() {
    float priority = 1.f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = m_queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE; // glslc lowers `discard` to demote for vulkan1.3
    f13.maintenance4 = VK_TRUE;

    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.pNext = &f13;
    f12.bufferDeviceAddress = VK_TRUE;
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.timelineSemaphore = VK_TRUE;
    f12.scalarBlockLayout = VK_TRUE;

    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    f2.features.samplerAnisotropy = VK_TRUE;
    f2.features.textureCompressionBC = VK_TRUE;
    f2.features.shaderInt64 = VK_TRUE;
    f2.features.shaderFloat64 = VK_TRUE;
    f2.features.fillModeNonSolid = VK_TRUE;

    std::vector<const char*> extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    // Promoted to core in 1.3, but the ImGui backend resolves the KHR entry point by name.
    if (hasDeviceExtension(m_physicalDevice, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME))
        extensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    if (deviceExtensionHook) {
        m_extraDeviceExts.clear();
        deviceExtensionHook(m_instance, m_physicalDevice, m_extraDeviceExts);
        for (const std::string& name : m_extraDeviceExts) {
            bool dup = false;
            for (const char* e : extensions) dup |= name == e;
            if (!hasDeviceExtension(m_physicalDevice, name.c_str())) LOG_WARN("Device extension {} not available", name);
            else if (!dup) extensions.push_back(name.c_str());
        }
    }

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = (uint32_t)extensions.size();
    ci.ppEnabledExtensionNames = extensions.data();
    VK_CHECK(vkCreateDevice(m_physicalDevice, &ci, nullptr, &m_device));
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);
}

VkCommandBuffer Context::beginOneShot() {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = m_oneShotPool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(m_device, &ai, &cmd));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    return cmd;
}

void Context::endOneShot(VkCommandBuffer cmd) {
    VK_CHECK(vkEndCommandBuffer(cmd));
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    VK_CHECK(vkQueueSubmit2(m_queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(m_queue));
    vkFreeCommandBuffers(m_device, m_oneShotPool, 1, &cmd);
}

} // namespace space::gfx
