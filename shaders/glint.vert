#version 460
// Spacecraft too small to resolve: a sunlit point of light at the craft's position (six vertices per
// instance build a small screen-space quad). It fades out as the real model grows past a few pixels,
// so a station approached from afar shows up as a moving star first instead of popping in late.

struct Craft {
    mat4 model;  // camera-relative, includes scale (model[3] = position)
    vec4 sunDir; // w: irradiance including eclipse
    vec4 tint;
    vec4 up;
    vec4 extra;  // x size (world units), y sunlit fraction, z metres per model unit, w glint allowed
};
layout(std430, set = 0, binding = 0) readonly buffer Crafts { Craft crafts[]; };

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 params; // x viewport width, y height, z radians per pixel, w visibility floor
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec3 vColor;

const vec2 kCorners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
const float kRadiusPx = 3.0;
const float kMetersPerUnit = 3.0856775814913673e16;

void collapse() {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    vUV = vec2(0.0);
    vColor = vec3(0.0);
}

void main() {
    Craft c = crafts[gl_InstanceIndex];
    vec2 corner = kCorners[gl_VertexIndex % 6];
    float sizeW = c.extra.x;
    if (sizeW <= 0.0 || c.extra.w < 0.5 || c.sunDir.w <= 0.0) { collapse(); return; }
    vec3 p = c.model[3].xyz;
    float d = length(p);
    float sizePx = sizeW / max(d, 1e-30) / pc.params.z;
    float fade = 1.0 - smoothstep(2.0, 7.0, sizePx);
    vec4 clip = pc.viewProj * vec4(p, 1.0);
    if (fade <= 0.0 || clip.w <= 0.0) { collapse(); return; }

    // Sunlight reflected by a structure covering ~a quarter of its bounding square, albedo ~0.5.
    float energy = 0.125 * sizePx * sizePx * c.sunDir.w;
    // Floor: close to home a sunlit craft reads as a star (the real ISS is one of the brightest objects in
    // the night sky), fading with distance so nothing twinkles from across the solar system.
    float km = d * kMetersPerUnit / 1000.0;
    energy = max(energy, pc.params.w * c.sunDir.w * (1.0 - smoothstep(20000.0, 400000.0, km)));
    const float sigma = 0.9;
    float peak = energy / (6.2831853 * sigma * sigma);
    vColor = vec3(1.0, 0.97, 0.92) * peak * fade;
    vUV = corner * (kRadiusPx / sigma);
    gl_Position = clip + vec4(corner * kRadiusPx * 2.0 / pc.params.xy * clip.w, 0.0, 0.0);
}
