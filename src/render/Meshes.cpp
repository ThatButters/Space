#include "render/Meshes.h"

#include <glm/gtc/constants.hpp>
#include <cmath>

namespace space::render {

MeshData makeUvSphere(uint32_t stacks, uint32_t slices) {
    MeshData m;
    const float pi = glm::pi<float>();
    for (uint32_t i = 0; i <= stacks; ++i) {
        float v = (float)i / stacks;
        float phi = v * pi; // 0 at +Y pole
        for (uint32_t j = 0; j <= slices; ++j) {
            float u = (float)j / slices;
            float theta = u * 2.f * pi;
            glm::vec3 p(std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta));
            m.vertices.emplace_back(p, 0.f);
        }
    }
    for (uint32_t i = 0; i < stacks; ++i) {
        for (uint32_t j = 0; j < slices; ++j) {
            uint32_t a = i * (slices + 1) + j;
            uint32_t b = a + slices + 1;
            // CCW when viewed from outside (right-handed, +Y up, theta increasing toward +Z).
            m.indices.insert(m.indices.end(), {a, a + 1, b, b, a + 1, b + 1});
        }
    }
    return m;
}

MeshData makeRing(uint32_t segments) {
    MeshData m;
    const float pi = glm::pi<float>();
    for (uint32_t j = 0; j <= segments; ++j) {
        float theta = (float)j / segments * 2.f * pi;
        float c = std::cos(theta), s = std::sin(theta);
        m.vertices.emplace_back(c, 0.f, s, 0.f); // inner
        m.vertices.emplace_back(c, 0.f, s, 1.f); // outer
    }
    for (uint32_t j = 0; j < segments; ++j) {
        uint32_t a = j * 2, b = a + 2;
        m.indices.insert(m.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
    }
    return m;
}

} // namespace space::render
