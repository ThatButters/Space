#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace space {

// GPU layout of one nebula (64 bytes). Mirrors shaders/nebula.frag.
struct NebulaGpu {
    glm::vec4 posRadius; // xyz centre, w radius
    glm::vec4 colorA;    // rgb, w density scale
    glm::vec4 colorB;    // rgb, w emission scale
    glm::vec4 params;    // seed, type (0 emission, 1 reflection, 2 dark), absorption, unused
};

// Header of the volumes storage buffer (64 bytes) followed by the nebula array.
struct VolumeHeader {
    glm::vec4 mediumCenter; // xyz galactic centre, w disc scale length
    glm::vec4 mediumShape;  // thickness, bulge radius, march tmax, dust noise scale
    glm::vec4 mediumTint;   // rgb glow colour, w dust density scale
    glm::vec4 mediumMarch;  // x first step length, y glow per unit length, z absorption per unit length
};

struct VolumeScene {
    VolumeHeader header{};
    std::vector<NebulaGpu> nebulae;

    // Packs header + nebulae into one contiguous blob for upload.
    std::vector<uint8_t> pack() const;
};

// Real nebulae at catalogued positions, in the parsec/galactic frame used by the star catalogue.
VolumeScene makeGalacticVolumes();

// Random nebulae along the arms of the procedural galaxy (matches StarField::Params defaults).
VolumeScene makeProceduralVolumes(uint32_t seed = 7);

// Galactic longitude/latitude (degrees) + distance -> engine coordinates.
glm::vec3 galacticToEngine(float lDeg, float bDeg, float distance);

} // namespace space
