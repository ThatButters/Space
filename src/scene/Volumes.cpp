#include "scene/Volumes.h"

#include <glm/gtc/constants.hpp>
#include <cmath>
#include <cstring>
#include <random>

namespace space {

namespace {

// Linear-light palettes.
const glm::vec3 kHAlpha{0.95f, 0.16f, 0.22f};
const glm::vec3 kOIII{0.18f, 0.72f, 0.80f};
const glm::vec3 kReflection{0.30f, 0.48f, 1.00f};
const glm::vec3 kWarmDust{0.75f, 0.45f, 0.25f};

NebulaGpu nebula(glm::vec3 pos, float radius, glm::vec3 a, glm::vec3 b, float density, float emission,
                 int type, float absorb, float seed) {
    NebulaGpu n;
    n.posRadius = glm::vec4(pos, radius);
    n.colorA = glm::vec4(a, density);
    n.colorB = glm::vec4(b, emission);
    n.params = glm::vec4(seed, (float)type, absorb, 0.f);
    return n;
}

} // namespace

glm::vec3 galacticToEngine(float lDeg, float bDeg, float d) {
    float l = glm::radians(lDeg), b = glm::radians(bDeg);
    float gx = d * std::cos(b) * std::cos(l);
    float gy = d * std::cos(b) * std::sin(l);
    float gz = d * std::sin(b);
    return {gx, gz, -gy}; // engine: x -> galactic centre, y -> north galactic pole
}

std::vector<uint8_t> VolumeScene::pack() const {
    std::vector<uint8_t> blob(sizeof(VolumeHeader) + nebulae.size() * sizeof(NebulaGpu));
    std::memcpy(blob.data(), &header, sizeof(VolumeHeader));
    if (!nebulae.empty())
        std::memcpy(blob.data() + sizeof(VolumeHeader), nebulae.data(), nebulae.size() * sizeof(NebulaGpu));
    return blob;
}

VolumeScene makeGalacticVolumes() {
    VolumeScene s;
    s.header.mediumCenter = glm::vec4(8178.f, 0.f, 0.f, 2600.f); // Sun -> Sgr A* = 8.18 kpc
    s.header.mediumShape = glm::vec4(160.f, 1100.f, 26000.f, 220.f);
    s.header.mediumTint = glm::vec4(0.95f, 0.88f, 0.78f, 1.0f);
    // Glow: ~0.1 integrated radiance along an in-plane 10 kpc path. Dust: optical depth ~4 toward the
    // centre in the plane, ~0.3 toward the poles.
    s.header.mediumMarch = glm::vec4(3.0f, 3.5e-4f, 1.8e-3f, 0.f);

    // Emission-line regions, mostly HII: (l, b, distance pc, radius pc)
    auto E = [&](float l, float b, float d, float r, float density, float emission, float seed) {
        s.nebulae.push_back(nebula(galacticToEngine(l, b, d), r, kHAlpha, kOIII, density, emission, 0, 0.12f, seed));
    };
    E(209.0f, -19.4f, 412.f, 7.f, 1.4f, 0.05f, 1.f);     // Orion, M42
    E(206.3f, -2.1f, 1600.f, 22.f, 0.9f, 0.012f, 2.f);   // Rosette
    E(160.2f, -12.4f, 300.f, 14.f, 0.6f, 0.006f, 3.f);   // California
    E(6.0f, -1.2f, 1250.f, 16.f, 1.0f, 0.02f, 4.f);      // Lagoon, M8
    E(7.0f, -0.3f, 1300.f, 8.f, 1.1f, 0.03f, 5.f);       // Trifid, M20
    E(17.0f, 0.8f, 1700.f, 13.f, 1.0f, 0.02f, 6.f);      // Eagle, M16
    E(15.1f, -0.7f, 1600.f, 11.f, 1.1f, 0.025f, 7.f);    // Omega, M17
    E(85.6f, -1.4f, 800.f, 17.f, 0.8f, 0.01f, 8.f);      // North America
    E(74.0f, -8.6f, 740.f, 15.f, 0.7f, 0.014f, 9.f);     // Veil
    E(287.6f, -0.6f, 2600.f, 38.f, 1.2f, 0.02f, 10.f);   // Carina
    E(36.2f, -57.1f, 200.f, 1.6f, 1.3f, 0.09f, 11.f);    // Helix
    E(205.0f, -15.0f, 400.f, 55.f, 0.35f, 0.0025f, 12.f); // Barnard's Loop (faint, huge)

    // Reflection nebulae: blue starlight scattered by dust.
    s.nebulae.push_back(nebula(galacticToEngine(166.6f, -23.5f, 136.f), 4.f, kReflection, kReflection * 0.7f,
                               1.0f, 0.03f, 1, 0.10f, 13.f)); // Pleiades
    s.nebulae.push_back(nebula(galacticToEngine(353.7f, 17.7f, 140.f), 7.f, kReflection, kWarmDust, 0.9f, 0.02f,
                               1, 0.16f, 14.f)); // Rho Ophiuchi

    // Dark clouds: absorb only.
    s.nebulae.push_back(nebula(galacticToEngine(303.3f, -1.5f, 180.f), 11.f, kWarmDust, kWarmDust, 1.3f, 0.f, 2,
                               0.35f, 15.f)); // Coalsack
    s.nebulae.push_back(nebula(galacticToEngine(30.0f, 1.5f, 350.f), 70.f, kWarmDust, kWarmDust, 0.6f, 0.f, 2,
                               0.05f, 16.f)); // Aquila Rift
    return s;
}

VolumeScene makeProceduralVolumes(uint32_t seed) {
    VolumeScene s;
    s.header.mediumCenter = glm::vec4(0.f, 0.f, 0.f, 3.2f);
    s.header.mediumShape = glm::vec4(0.10f, 0.9f, 60.f, 0.9f);
    s.header.mediumTint = glm::vec4(0.95f, 0.88f, 0.78f, 1.0f);
    s.header.mediumMarch = glm::vec4(0.01f, 0.28f, 1.5f, 0.f); // same look, 1 unit ~ 800 pc

    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> uni(0.f, 1.f);
    std::normal_distribution<float> gauss(0.f, 1.f);
    const float pi = glm::pi<float>();
    const float tanPitch = std::tan(glm::radians(16.f));

    for (int i = 0; i < 14; ++i) {
        float r = 1.5f + 9.f * uni(rng);
        float theta = std::log(r / 0.6f) / tanPitch + (uni(rng) < 0.5f ? 0.f : pi) + gauss(rng) * 0.2f;
        glm::vec3 pos(r * std::cos(theta), gauss(rng) * 0.06f, r * std::sin(theta));
        float radius = 0.25f + 0.55f * uni(rng);
        float kind = uni(rng);
        if (kind < 0.6f)
            s.nebulae.push_back(nebula(pos, radius, kHAlpha, kOIII, 1.1f, 0.6f / radius, 0, 2.5f / radius, (float)i));
        else if (kind < 0.85f)
            s.nebulae.push_back(nebula(pos, radius, kReflection, kWarmDust, 1.0f, 0.5f / radius, 1, 2.5f / radius, (float)i));
        else
            s.nebulae.push_back(nebula(pos, radius, kWarmDust, kWarmDust, 1.2f, 0.f, 2, 6.f / radius, (float)i));
    }
    return s;
}

} // namespace space
