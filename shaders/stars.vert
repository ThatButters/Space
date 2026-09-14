#version 460
#include "frame.glsl"
// Instanced point-sprite stars (steady: not modulated by the music). Six vertices per star build a camera-facing quad in clip space.
// The quad size in pixels follows the received flux so bright stars bloom into wider Gaussians,
// while the total energy of each star stays constant (normalised in the fragment shader).

struct Star {
    vec4 posRadiance; // xyz world position, w radiance
    vec4 color;       // linear rgb
};

layout(std430, set = 0, binding = 0) readonly buffer Stars { Star stars[]; };
FRAME_DATA_BLOCK(1)

layout(push_constant) uniform PushConstants {
    mat4 viewProj;   // rotation-only view * projection (camera-relative rendering)
    vec4 camPos;     // xyz camera position in world units
    vec4 params;     // x: viewport width, y: viewport height, z: brightness scale, w: max radius px
} pc;

layout(location = 0) out vec2 vUV;       // -1..1 across the quad
layout(location = 1) out vec3 vColor;    // radiance-weighted colour
layout(location = 2) out float vRadius;  // quad half-size in pixels

const vec2 kCorners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));

void main() {
    uint id = uint(gl_VertexIndex) / 6u;
    uint corner = uint(gl_VertexIndex) % 6u;
    Star s = stars[id];

    vec3 rel = s.posRadiance.xyz - pc.camPos.xyz;
    float d2 = max(dot(rel, rel), 1e-8);
    vec4 clip = pc.viewProj * vec4(rel, 1.0);

    // Behind the camera: collapse the quad off-screen.
    if (clip.w <= 1e-4) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        vUV = vec2(0.0);
        vColor = vec3(0.0);
        vRadius = 1.0;
        return;
    }

    float flux = s.posRadiance.w * pc.params.z / d2;
    float ringLight = 0.0;
    // Stars no longer follow the music (the steady sky reads better); other effects still react.
    const float react = 0.0;

    // Radius grows logarithmically with flux; floor keeps faint stars at least a couple of pixels
    // so they do not flicker, ceiling stops nearby stars from filling the screen.
    float radius = clamp(1.3 + 0.45 * log2(1.0 + (flux + ringLight) * 6.0), 1.3, pc.params.w);

    vec2 c = kCorners[corner];
    vec2 offsetNdc = c * radius / vec2(pc.params.x, pc.params.y) * 2.0;
    clip.xy += offsetNdc * clip.w;

    gl_Position = clip;
    vUV = c;
    vec3 starColor = s.color.rgb;
    if (react > 0.0) {
        // Energetic passages tint the whole sky toward the mood colour; quiet ones leave it natural.
        float tint = clamp(frame.features.w * react * (0.35 * frameLevel() + 0.5 * frameBand(int(id * 11u) & 31)), 0.0, 0.85);
        starColor = mix(starColor, starColor * frameMood() * 1.4, tint);
    }
    vColor = starColor * flux + frameMood() * ringLight;
    vRadius = radius;
}
