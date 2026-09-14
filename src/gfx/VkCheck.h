#pragma once
#include <vulkan/vulkan.h>
#include <stdexcept>
#include <string>

namespace space::gfx {

inline void vkCheck(VkResult r, const char* what) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string(what) + " failed with VkResult " + std::to_string((int)r));
}

} // namespace space::gfx

#define VK_CHECK(expr) ::space::gfx::vkCheck((expr), #expr)
