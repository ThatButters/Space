#version 460
#include "common.glsl"

// Procedural deep-sky background: layered star field + nebula + galactic band.
// This is the placeholder "hello universe" pass. Milestone 2 replaces the star layers with
// instanced Gaia DR3 catalogue stars and keeps the nebula as a raymarched volume.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// Must match ViewPushConstants in Renderer.cpp (shared with nebula.frag).
layout(push_constant) uniform PushConstants {
    vec4 camRight;    // xyz
    vec4 camUp;       // xyz
    vec4 camForward;  // xyz
    vec4 camPos;      // xyz (unused here: the sky is at infinity)
    vec4 params;      // x: tan(fovY/2), y: aspect, z: time, w: radians per pixel
    vec4 look;        // x: star density, y: nebula intensity
} pc;

// Cube-map parameterisation: a direction maps to one of six faces and a 2D coordinate in [-1,1].
// Stars live in a 2D grid on each face, so a 3x3 neighbourhood always covers every star whose
// point-spread reaches this pixel (the only seams are the face edges, where blobs are tiny).
int cubeFace(vec3 d, out vec2 uv) {
    vec3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z) { uv = vec2(d.z, d.y) / a.x; return d.x > 0.0 ? 0 : 1; }
    if (a.y >= a.z)               { uv = vec2(d.x, d.z) / a.y; return d.y > 0.0 ? 2 : 3; }
    uv = vec2(d.x, d.y) / a.z;    return d.z > 0.0 ? 4 : 5;
}

vec3 cubeDir(int face, vec2 uv) {
    if (face == 0) return normalize(vec3( 1.0, uv.y, uv.x));
    if (face == 1) return normalize(vec3(-1.0, uv.y, uv.x));
    if (face == 2) return normalize(vec3(uv.x,  1.0, uv.y));
    if (face == 3) return normalize(vec3(uv.x, -1.0, uv.y));
    if (face == 4) return normalize(vec3(uv.x, uv.y,  1.0));
    return normalize(vec3(uv.x, uv.y, -1.0));
}

vec3 starLayer(vec3 dir, float scale, float density, float pixelAngle, float brightnessScale) {
    vec2 uv;
    int face = cubeFace(dir, uv);
    vec2 p = (uv * 0.5 + 0.5) * scale;
    vec2 cell = floor(p);
    vec3 col = vec3(0.0);

    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
        vec2 c2 = cell + vec2(x, y);
        if (c2.x < 0.0 || c2.y < 0.0 || c2.x >= scale || c2.y >= scale) continue;
        vec3 c = vec3(c2, float(face) * 131.0 + scale);
        vec3 h = hash33(c);
        if (h.x > density) continue;

        vec2 jitter = (hash33(c + 7.31).xy - 0.5) * 0.9;
        vec2 suv = ((c2 + 0.5 + jitter) / scale) * 2.0 - 1.0;
        vec3 sdir = cubeDir(face, suv);
        float cosA = dot(dir, sdir);
        float ang = sqrt(max(0.0, 2.0 - 2.0 * cosA)); // ~angle for small angles, cheap

        // Heavy-tailed brightness: almost everything is faint, a handful of stars blaze.
        float mag = pow(hash13(c + 3.7), 8.0);
        float brightness = mix(0.08, 12.0, mag) * brightnessScale;

        float temp = mix(2800.0, 22000.0, pow(hash13(c + 11.9), 2.5));
        vec3 tint = blackbody(temp);

        // Gaussian point-spread scaled to the pixel footprint; bright stars get a wider halo.
        float sigma = pixelAngle * (0.7 + 0.6 * mag);
        float psf = exp(-(ang * ang) / (2.0 * sigma * sigma));
        float halo = exp(-ang / (pixelAngle * 3.0)) * 0.01 * mag;
        col += tint * brightness * (psf + halo);
    }
    return col;
}

void main() {
    // Reconstruct view ray from the camera basis (Y up in screen space).
    vec2 ndc = vec2(vUV.x * 2.0 - 1.0, 1.0 - vUV.y * 2.0);
    float tanHalf = pc.params.x, aspect = pc.params.y, time = pc.params.z, pixelAngle = pc.params.w;
    vec3 dir = normalize(pc.camForward.xyz + ndc.x * tanHalf * aspect * pc.camRight.xyz + ndc.y * tanHalf * pc.camUp.xyz);

    float starDensity = pc.look.x;
    float nebulaIntensity = pc.look.y;

    // Galactic plane is y = 0 in both catalogues, so the faint background band lines up with the
    // volumetric medium.
    float galLat = dir.y;
    float band = exp(-galLat * galLat * 18.0);

    // --- Nebula / interstellar dust ------------------------------------------------------
    vec3 warp = vec3(fbm(dir * 2.0 + 3.1, 3), fbm(dir * 2.0 + 7.7, 3), fbm(dir * 2.0 + 1.3, 3));
    float n1 = fbm(dir * 3.5 + warp * 1.5, 5);
    float n2 = fbm(dir * 7.0 - warp * 2.0 + 20.0, 4);
    float dust = smoothstep(0.45, 0.85, n1);
    float glow = pow(smoothstep(0.35, 0.9, n2), 2.0);

    vec3 nebulaA = vec3(0.35, 0.12, 0.45); // magenta-violet
    vec3 nebulaB = vec3(0.05, 0.25, 0.45); // teal-blue
    vec3 nebulaC = vec3(0.55, 0.30, 0.12); // warm dust
    vec3 nebula = mix(nebulaA, nebulaB, n2) * glow * 0.10 + nebulaC * dust * band * 0.05;
    vec3 milkyWay = vec3(0.65, 0.62, 0.70) * band * (0.025 + 0.035 * fbm(dir * 12.0, 4));

    vec3 col = (nebula + milkyWay) * nebulaIntensity;

    // --- Stars ----------------------------------------------------------------------------
    // Six faces of scale^2 cells each, so densities drop as the layers get finer:
    // roughly 0.5k bright, 4.5k medium and 19k faint stars across the whole sky at density 1.0.
    float densityBoost = 0.5 + 0.9 * band; // more stars along the galactic plane
    col += starLayer(dir, 18.0, 0.25 * starDensity, pixelAngle, 1.0);
    col += starLayer(dir, 55.0, 0.25 * starDensity * densityBoost, pixelAngle, 0.30);
    col += starLayer(dir, 140.0, 0.16 * starDensity * densityBoost, pixelAngle, 0.08);

    // Very faint unresolved background + subtle slow shimmer so it never feels static.
    col += vec3(0.0015, 0.0015, 0.0022) * (0.8 + 0.2 * sin(time * 0.1 + dir.x * 3.0));

    outColor = vec4(col, 1.0);
}
