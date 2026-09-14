#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace space::render {

struct MeshData {
    std::vector<glm::vec4> vertices; // xyz position, w = auxiliary (0 for spheres, radial 0..1 for rings)
    std::vector<uint32_t> indices;
};

// Unit UV sphere, counter-clockwise winding seen from outside.
MeshData makeUvSphere(uint32_t stacks, uint32_t slices);

// Unit annulus in the XZ plane. Vertex w carries the radial parameter so the shader can set the inner radius.
MeshData makeRing(uint32_t segments);

} // namespace space::render
