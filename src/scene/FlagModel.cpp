#include "scene/FlagModel.h"

#include <glm/gtc/constants.hpp>
#include <algorithm>
#include <cmath>

namespace space {

namespace {

// Point-in-star test for a regular five-pointed star (outer radius 1, inner radius 0.382), point up.
bool insideStar(float x, float y) {
    const float pi = glm::pi<float>();
    const float r = std::sqrt(x * x + y * y);
    if (r > 1.f) return false;
    const float sector = 2.f * pi / 5.f;
    // Fold the angle (0 = straight up) into [0, sector/2]: 0 on a tip, sector/2 on a notch.
    float a = std::fmod(std::atan2(x, y) + 2.f * pi, sector);
    a = std::min(a, sector - a);
    // The edge runs from the tip (radius 1, angle 0) to the notch (radius 0.382, angle sector/2).
    const glm::vec2 tip(0.f, 1.f);
    const glm::vec2 notch(std::sin(sector * 0.5f) * 0.382f, std::cos(sector * 0.5f) * 0.382f);
    const glm::vec2 e = notch - tip;
    const glm::vec2 d(std::sin(a), std::cos(a));
    // Distance along d to the edge line: tip + e*s' = d*t  ->  t = cross(tip, e) / cross(d, e).
    const float denom = d.x * e.y - d.y * e.x;
    if (std::abs(denom) < 1e-6f) return true;
    const float t = (tip.x * e.y - tip.y * e.x) / denom;
    return r <= t;
}

gfx::DecodedImage makeFlagTexture() {
    // 5:3 like the flown 3 x 5 ft flags; canton 7 stripes tall and 40 % of the fly, stars per US Code.
    const int W = 2048, H = 1229;
    gfx::DecodedImage img;
    img.width = W;
    img.height = H;
    img.rgba.resize((size_t)W * H * 4);
    const glm::vec3 red(0.698f, 0.133f, 0.204f), white(1.f), blue(0.235f, 0.231f, 0.431f);
    const float cantonW = 0.4f * W, cantonH = H * 7.f / 13.f;
    const float starR = 0.0616f * H * 0.5f;
    const int SS = 4;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            glm::vec3 acc(0.f);
            for (int sy = 0; sy < SS; ++sy)
                for (int sx = 0; sx < SS; ++sx) {
                    const float fx = x + (sx + 0.5f) / SS, fy = y + (sy + 0.5f) / SS;
                    glm::vec3 c;
                    if (fx < cantonW && fy < cantonH) {
                        c = blue;
                        // 9 rows alternating 6 and 5 stars.
                        const float row = fy / cantonH * 10.f;
                        const int ri = (int)std::floor(row + 0.5f);
                        if (ri >= 1 && ri <= 9) {
                            const bool six = (ri % 2) == 1;
                            const float col = fx / cantonW * 12.f;
                            int ci = (int)std::floor(col + 0.5f);
                            const bool ok = six ? (ci % 2 == 1 && ci >= 1 && ci <= 11) : (ci % 2 == 0 && ci >= 2 && ci <= 10);
                            if (ok) {
                                const float cx = ci / 12.f * cantonW, cy = ri / 10.f * cantonH;
                                if (insideStar((fx - cx) / starR, -(fy - cy) / starR)) c = white;
                            }
                        }
                    } else {
                        const int stripe = std::min(12, (int)std::floor(fy / H * 13.f));
                        c = (stripe % 2 == 0) ? red : white;
                    }
                    acc += c;
                }
            acc /= float(SS * SS);
            // Nylon weave: faint thread texture so the cloth does not read as a decal.
            const float weave = 0.965f + 0.035f * (((x ^ (y * 3)) & 3) == 0 ? 1.f : 0.f);
            uint8_t* p = img.rgba.data() + ((size_t)y * W + x) * 4;
            for (int k = 0; k < 3; ++k) {
                const float lin = acc[k] * weave;
                const float srgb = lin <= 0.0031308f ? lin * 12.92f : 1.055f * std::pow(lin, 1.f / 2.4f) - 0.055f;
                p[k] = (uint8_t)std::clamp(srgb * 255.f + 0.5f, 0.f, 255.f);
            }
            p[3] = 255;
        }
    }
    return img;
}

void addCylinder(gfx::ModelData& m, glm::vec3 a, glm::vec3 b, float radius, int segments, const glm::vec4& color,
                 float metallic, float roughness) {
    gfx::ModelPrimitive prim;
    prim.firstIndex = (uint32_t)m.indices.size();
    prim.baseColor = color;
    prim.metallic = metallic;
    prim.roughness = roughness;
    const glm::vec3 axis = glm::normalize(b - a);
    const glm::vec3 ref = std::abs(axis.y) < 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    const glm::vec3 u = glm::normalize(glm::cross(axis, ref)), v = glm::cross(axis, u);
    const uint32_t base = (uint32_t)m.vertices.size();
    for (int i = 0; i <= segments; ++i) {
        const float t = (float)i / segments * glm::two_pi<float>();
        const glm::vec3 n = u * std::cos(t) + v * std::sin(t);
        m.vertices.push_back({a + n * radius, n, {(float)i / segments, 0.f}});
        m.vertices.push_back({b + n * radius, n, {(float)i / segments, 1.f}});
    }
    for (int i = 0; i < segments; ++i) {
        const uint32_t k = base + i * 2;
        m.indices.insert(m.indices.end(), {k, k + 2, k + 1, k + 1, k + 2, k + 3});
    }
    // End caps.
    for (int end = 0; end < 2; ++end) {
        const glm::vec3 c = end == 0 ? a : b;
        const glm::vec3 n = end == 0 ? -axis : axis;
        const uint32_t center = (uint32_t)m.vertices.size();
        m.vertices.push_back({c, n, {0.5f, 0.5f}});
        for (int i = 0; i <= segments; ++i) {
            const float t = (float)i / segments * glm::two_pi<float>();
            m.vertices.push_back({c + (u * std::cos(t) + v * std::sin(t)) * radius, n, {0.5f, 0.5f}});
        }
        for (int i = 0; i < segments; ++i) m.indices.insert(m.indices.end(), {center, center + 1 + i, center + 2 + i});
    }
    prim.indexCount = (uint32_t)m.indices.size() - prim.firstIndex;
    m.primitives.push_back(prim);
}

} // namespace

gfx::ModelData makeExhaustPlumeModel() {
    gfx::ModelData m;
    auto cone = [&](float r0, float r1, float length, float ragged, glm::vec3 emissive, float seed) {
        gfx::ModelPrimitive prim;
        prim.firstIndex = (uint32_t)m.indices.size();
        prim.baseColor = glm::vec4(0.05f, 0.03f, 0.02f, 1.f);
        prim.emissiveFactor = emissive;
        prim.roughness = 1.f;
        const int rings = 24, segs = 28;
        const uint32_t base = (uint32_t)m.vertices.size();
        for (int j = 0; j <= rings; ++j) {
            const float t = (float)j / rings;
            const float y = -length * t;
            for (int i = 0; i <= segs; ++i) {
                const float a = (float)i / segs * glm::two_pi<float>();
                const float rag = 1.f + ragged * (std::sin(a * 5.f + seed + t * 9.f) * 0.5f + std::sin(a * 3.f - t * 14.f + seed * 2.f) * 0.3f) * t;
                const float r = (r0 + (r1 - r0) * t) * rag;
                const glm::vec3 p(std::cos(a) * r, y, std::sin(a) * r);
                m.vertices.push_back({p, glm::normalize(glm::vec3(std::cos(a), 0.25f, std::sin(a))), {(float)i / segs, t}});
            }
        }
        for (int j = 0; j < rings; ++j)
            for (int i = 0; i < segs; ++i) {
                const uint32_t a = base + j * (segs + 1) + i, b = a + 1, c = a + segs + 1, d = c + 1;
                m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
            }
        prim.indexCount = (uint32_t)m.indices.size() - prim.firstIndex;
        m.primitives.push_back(prim);
    };
    // A single enclosing proxy: the fragment shader ray-marches the flame inside it.
    cone(9.0f, 100.0f, 212.f, 0.0f, glm::vec3(0.f), 0.f);
    m.volumetricFlame = true;
    m.boundsMin = m.boundsMax = m.vertices[0].position;
    for (const auto& v : m.vertices) {
        m.boundsMin = glm::min(m.boundsMin, v.position);
        m.boundsMax = glm::max(m.boundsMax, v.position);
    }
    return m;
}

gfx::ModelData makeExhaustTrailModel() {
    // Same proxy shape as the flame, four kilometres long; the shader's trail branch fills it (plume.frag).
    gfx::ModelData m = makeExhaustPlumeModel();
    for (auto& v : m.vertices) {
        const float t = -v.position.y / 212.f;                       // 0 at the engine plane, 1 at the end
        const float r = (40.f + 700.f * t) / (9.f + 91.f * t);        // new radius over the flame cone's
        v.position = glm::vec3(v.position.x * r, -4000.f * t, v.position.z * r);
    }
    m.boundsMin = m.boundsMax = m.vertices[0].position;
    for (const auto& v : m.vertices) {
        m.boundsMin = glm::min(m.boundsMin, v.position);
        m.boundsMax = glm::max(m.boundsMax, v.position);
    }
    return m;
}

gfx::ModelData makeApolloFlagModel() {
    gfx::ModelData m;
    const float mastTop = 2.18f;              // visible mast height above the regolith
    const float flyLen = 1.524f, hoist = 0.914f; // 5 ft x 3 ft
    const float barY = mastTop - 0.03f;

    const glm::vec4 aluminium(0.80f, 0.80f, 0.78f, 1.f);
    addCylinder(m, {0, 0, 0}, {0, mastTop, 0}, 0.0165f, 16, aluminium, 0.9f, 0.35f);
    addCylinder(m, {0.01f, barY, 0}, {flyLen + 0.04f, barY, 0}, 0.0095f, 12, aluminium, 0.9f, 0.35f);

    // Cloth: a grid hanging from the crossbar, with the rippled look of the flown flag.
    gfx::ModelPrimitive cloth;
    cloth.firstIndex = (uint32_t)m.indices.size();
    cloth.baseColor = glm::vec4(1.f);
    cloth.metallic = 0.f;
    cloth.roughness = 0.85f;
    cloth.imageIndex = 0;
    const int NX = 64, NY = 36;
    const uint32_t base = (uint32_t)m.vertices.size();
    std::vector<glm::vec3> pos((NX + 1) * (NY + 1));
    for (int j = 0; j <= NY; ++j) {
        for (int i = 0; i <= NX; ++i) {
            const float u = (float)i / NX, v = (float)j / NY; // v = 0 at the crossbar
            float x = 0.025f + u * flyLen * 0.985f;
            float y = barY - 0.012f - v * hoist;
            // Ripples from the partly extended crossbar: strongest in the middle of the fly, fading
            // toward the sewn sleeves at the mast and the bar; the free lower edge sags a little.
            const float along = std::sin(u * 7.3f * glm::pi<float>() + 0.6f) * 0.028f +
                                std::sin(u * 17.0f + v * 3.0f) * 0.008f;
            const float mastPin = std::clamp(u * 6.f, 0.f, 1.f);
            const float z = along * mastPin * (0.35f + 0.65f * v) + 0.012f * std::sin(v * 9.f + u * 4.f) * v;
            y -= 0.03f * v * v * u;
            pos[j * (NX + 1) + i] = glm::vec3(x, y, z);
        }
    }
    for (int j = 0; j <= NY; ++j) {
        for (int i = 0; i <= NX; ++i) {
            const glm::vec3& p = pos[j * (NX + 1) + i];
            const glm::vec3 dx = pos[j * (NX + 1) + std::min(i + 1, NX)] - pos[j * (NX + 1) + std::max(i - 1, 0)];
            const glm::vec3 dy = pos[std::min(j + 1, NY) * (NX + 1) + i] - pos[std::max(j - 1, 0) * (NX + 1) + i];
            glm::vec3 n = glm::normalize(glm::cross(dy, dx));
            m.vertices.push_back({p, n, {(float)i / NX, (float)j / NY}});
        }
    }
    for (int j = 0; j < NY; ++j) {
        for (int i = 0; i < NX; ++i) {
            const uint32_t a = base + j * (NX + 1) + i;
            const uint32_t b = a + 1, c = a + NX + 1, d = c + 1;
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }
    }
    cloth.indexCount = (uint32_t)m.indices.size() - cloth.firstIndex;
    m.primitives.push_back(cloth);
    m.images.push_back(makeFlagTexture());

    m.boundsMin = m.boundsMax = m.vertices[0].position;
    for (const auto& v : m.vertices) {
        m.boundsMin = glm::min(m.boundsMin, v.position);
        m.boundsMax = glm::max(m.boundsMax, v.position);
    }
    return m;
}

} // namespace space
