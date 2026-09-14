#pragma once
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace space {

// GPU layout of one star: 32 bytes, std430-friendly.
struct StarGpu {
    glm::vec4 posRadiance; // xyz position (world units), w radiance
    glm::vec4 color;       // linear rgb (blackbody), w unused
};

// Procedural spiral galaxy used until the Gaia DR3 loader lands. Deterministic for a given seed.
class StarField {
public:
    struct Params {
        uint32_t count = 400'000;
        uint32_t seed = 1337;
        float discRadius = 14.f;   // world units
        float discScale = 3.2f;    // exponential scale length
        float thickness = 0.10f;   // vertical sigma at the centre
        float bulgeFraction = 0.14f;
        float bulgeSigma = 0.7f;
        float armFraction = 0.72f; // stars that cluster near the spiral arms
        float armPitchDeg = 16.f;
        float armSpread = 0.28f;   // angular sigma around an arm (radians)
    };

    void generate(const Params& p);
    const std::vector<StarGpu>& stars() const { return m_stars; }

private:
    std::vector<StarGpu> m_stars;
};

// Blackbody colour (linear sRGB) for a temperature in Kelvin, matches the shader helper.
glm::vec3 blackbodyColor(float kelvin);

} // namespace space
