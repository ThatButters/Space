#version 460
#include "frame.glsl"
// HDR + bloom -> display.
//   SDR: exposure, ACES-fitted tonemap, output linear (sRGB swapchain encodes).
//   HDR: exposure, soft shoulder toward the panel's peak, output scRGB linear where 1.0 = 80 nits.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uHdr;
layout(set = 0, binding = 1) uniform sampler2D uBloom;
FRAME_DATA_BLOCK(1)

layout(push_constant) uniform PushConstants {
    vec4 params; // x: exposure, y: bloom strength, z: 1 = HDR output, w: unused
    vec4 hdr;    // x: paper white (nits), y: peak luminance (nits)
} pc;

vec3 acesFitted(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

void main() {
    vec3 hdr, bloom;
    float ca = 0.0; // beat: a soft radial colour split at the edges
    if (ca > 1e-4) {
        vec2 d = (vUV - 0.5) * ca * length(vUV - 0.5) * 2.0;
        hdr = vec3(texture(uHdr, vUV + d).r, texture(uHdr, vUV).g, texture(uHdr, vUV - d).b);
        bloom = vec3(texture(uBloom, vUV + d).r, texture(uBloom, vUV).g, texture(uBloom, vUV - d).b);
    } else {
        hdr = texture(uHdr, vUV).rgb;
        bloom = texture(uBloom, vUV).rgb;
    }
    vec3 color = (hdr + bloom * pc.params.y) * pc.params.x;

    if (pc.params.z > 0.5) {
        // Scene value 1.0 maps to paper white; highlights roll off smoothly toward the peak so the
        // Sun and bright stars glow without clipping. Units: multiples of 80 nits (scRGB).
        float paper = pc.hdr.x / 80.0;
        float peak = pc.hdr.y / 80.0;
        vec3 c = color * paper;
        // Per-channel soft clip that is linear below ~half the peak.
        vec3 shoulder = peak * (1.0 - exp(-c / peak));
        vec3 mapped = mix(c, shoulder, smoothstep(0.0, peak, c));
        outColor = vec4(mapped, 1.0);
        return;
    }

    vec3 ldr = acesFitted(color);
    // Cheap dither to stop banding in the deep blacks of space.
    float dither = (fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) - 0.5) / 255.0;
    outColor = vec4(ldr + dither, 1.0);
}
