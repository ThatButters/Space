#include "scene/StarField.h"

#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>
#include <random>

namespace space {

glm::vec3 blackbodyColor(float kelvin) {
    float t = std::clamp(kelvin, 1500.f, 40000.f) / 100.f;
    glm::vec3 c;
    c.r = t <= 66.f ? 1.f : std::clamp(1.2929f * std::pow(t - 60.f, -0.1332f), 0.f, 1.f);
    c.g = t <= 66.f ? std::clamp(0.3901f * std::log(t) - 0.6318f, 0.f, 1.f)
                    : std::clamp(1.1299f * std::pow(t - 60.f, -0.0755f), 0.f, 1.f);
    c.b = t >= 66.f ? 1.f : (t <= 19.f ? 0.f : std::clamp(0.5432f * std::log(t - 10.f) - 1.1962f, 0.f, 1.f));
    return c * c;
}

void StarField::generate(const Params& p) {
    std::mt19937 rng(p.seed);
    std::uniform_real_distribution<float> uni(0.f, 1.f);
    std::normal_distribution<float> gauss(0.f, 1.f);

    const float pi = glm::pi<float>();
    const float tanPitch = std::tan(glm::radians(p.armPitchDeg));
    const float r0 = 0.6f; // radius where the arms start

    m_stars.clear();
    m_stars.reserve(p.count);

    for (uint32_t i = 0; i < p.count; ++i) {
        glm::vec3 pos;
        float temperature;
        bool inArm = false;

        if (uni(rng) < p.bulgeFraction) {
            // Central bulge: old, warm stars in a slightly flattened Gaussian blob.
            pos = glm::vec3(gauss(rng), gauss(rng) * 0.65f, gauss(rng)) * p.bulgeSigma;
            temperature = 3500.f + 2500.f * std::pow(uni(rng), 2.f);
        } else {
            // Exponential disc, truncated.
            float r = 0.f;
            do {
                r = -p.discScale * std::log(std::max(uni(rng), 1e-6f));
            } while (r > p.discRadius);

            float theta = uni(rng) * 2.f * pi;
            if (r > r0 && uni(rng) < p.armFraction) {
                // Logarithmic spiral, two arms; spread widens with radius so arms fade outward.
                float armPhase = std::log(r / r0) / tanPitch;
                float arm = (uni(rng) < 0.5f) ? 0.f : pi;
                theta = armPhase + arm + gauss(rng) * (p.armSpread * (0.7f + 0.6f * r / p.discRadius));
                inArm = true;
            }

            float flare = 1.f + 1.5f * r / p.discRadius; // discs get thicker toward the rim
            pos = glm::vec3(r * std::cos(theta), gauss(rng) * p.thickness * flare, r * std::sin(theta));

            // Arms host young hot blue stars; the rest of the disc is a cooler mix.
            float u = uni(rng);
            temperature = inArm ? (u < 0.15f ? 12000.f + 18000.f * uni(rng) : 3800.f + 4500.f * std::pow(uni(rng), 1.5f))
                                : 3200.f + 4000.f * std::pow(uni(rng), 2.f);
        }

        // Log-normal luminosity; hot stars are brighter. Units are arbitrary, tuned in the shader.
        float logL = -3.9f + 0.9f * gauss(rng) + 1.6f * std::log(temperature / 5000.f);
        float radiance = std::exp(logL);

        StarGpu s;
        s.posRadiance = glm::vec4(pos, radiance);
        s.color = glm::vec4(blackbodyColor(temperature), 0.f);
        m_stars.push_back(s);
    }
}

} // namespace space
