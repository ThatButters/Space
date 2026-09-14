#version 460
// Spacecraft: glTF meshes with a per-instance camera-relative model matrix. Also used, with the
// shadow map's matrix in viewProj, for the depth-only shadow pass.

struct Craft {
    mat4 model;     // camera-relative, includes scale
    vec4 sunDir;    // xyz direction to the Sun (world), w irradiance factor
    vec4 tint;      // rgb multiplier, w ground albedo under a lander
    vec4 up;        // xyz away from the parent body, w 1 near a body
    vec4 extra;     // x size (world units), y sunlit fraction, z metres per model unit, w glint allowed
};
layout(std430, set = 0, binding = 0) readonly buffer Crafts { Craft crafts[]; };

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 baseColor;  // material base colour factor
    ivec4 ids;       // x craft index, y base colour texture, z normal map, w metallic-roughness map (-1 = none)
    vec4 material;   // x metallic, y roughness, z normal scale, w occlusion texture index (-1 = none)
    mat4 shadowMat;
    vec4 shadowInfo; // x shadow texture index, y 1 / resolution, z depth bias, w metres per world unit
    vec4 uvTransform; // xy offset, zw scale (KHR_texture_transform)
    vec4 emissive;    // rgb factor, w emissive texture index (-1 = none)
} pc;

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec3 vModelPos;

void main() {
    Craft c = crafts[pc.ids.x];
    vec4 world = c.model * vec4(inPos, 1.0);
    vWorldPos = world.xyz;
    vNormal = normalize(mat3(c.model) * inNormal);
    vUv = inUv * pc.uvTransform.zw + pc.uvTransform.xy;
    vModelPos = inPos;
    gl_Position = pc.viewProj * world;
}
