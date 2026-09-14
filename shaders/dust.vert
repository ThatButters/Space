#version 460
#include "common.glsl"
#include "frame.glsl"
FRAME_DATA_BLOCK(0)
// Interplanetary dust motes: a procedural field that wraps around the camera. Each mote lives at a
// hashed position inside a cube of side `extent`; the cube is re-centred on the camera through the
// camera's fractional phase (computed in double precision on the CPU), so nothing jitters even when
// the camera sits at 1 AU. Motes streak along the camera's velocity and fade near the cube faces.

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 phase;    // xyz camera phase in the cell (0..1), w extent (world units)
    vec4 velocity; // xyz camera velocity (world units / s), w time
    vec4 params;   // viewport w, h, brightness, unused
} pc;

layout(location = 0) out vec2 vUV;
layout(location = 1) out vec3 vColor;
layout(location = 2) out float vRadius;
layout(location = 3) out float vTail;

const vec2 kCorners[6] = vec2[](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));

void main() {
    uint id = uint(gl_VertexIndex) / 6u;
    uint corner = uint(gl_VertexIndex) % 6u;
    float extent = pc.phase.w;

    vec3 h = hash33(vec3(float(id) * 0.731 + 1.0, float(id) * 0.117 + 7.0, 3.0));
    vec3 h2 = hash33(vec3(float(id) * 0.331 + 5.0, 2.0, float(id) * 0.211 + 9.0));

    // Comets: about one mote in 90 is a larger body with its own slow drift and a glowing tail.
    bool comet = h2.z > 0.989;
    vec3 own = comet ? (hash33(vec3(float(id) * 0.53, 4.0, 1.0)) - 0.5) * 0.12 * pc.velocity.w : vec3(0.0);
    // Camera-relative position: wrap the mote's cell coordinate around the camera phase.
    // Every mote drifts slowly on its own so the field breathes even when the camera is still.
    vec3 slowDrift = (hash33(vec3(float(id) * 0.19, 8.0, 3.0)) - 0.5) * 0.004 * pc.velocity.w;
    vec3 rel = (fract(h + own + slowDrift - pc.phase.xyz) - 0.5) * extent;

    // Fade with distance inside the sphere inscribed in the wrap cube: no visible container shape.
    float fade = 1.0 - smoothstep(0.30, 0.5, length(rel) / extent);

    // Streak: the quad spans the mote's position now and a moment ago along the camera's motion.
    vec3 ownVel = comet ? (hash33(vec3(float(id) * 0.53, 4.0, 1.0)) - 0.5) * 0.12 * extent : vec3(0.0);
    vec3 relPrev = rel + (pc.velocity.xyz - ownVel) * (comet ? 0.5 : 0.035);
    vec4 clipA = pc.viewProj * vec4(rel, 1.0);
    vec4 clipB = pc.viewProj * vec4(relPrev, 1.0);
    if (clipA.w <= 1e-6 || clipB.w <= 1e-6 || fade <= 0.001) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        vUV = vec2(0.0);
        vColor = vec3(0.0);
        vRadius = 1.0;
        vTail = 0.0;
        return;
    }
    vec2 viewport = pc.params.xy;
    vec2 a = clipA.xy / clipA.w, b = clipB.xy / clipB.w;
    vec2 axis = (b - a) * viewport * 0.5; // pixels
    float len = length(axis);
    vec2 dir = len > 1e-3 ? axis / len : vec2(1.0, 0.0);
    vec2 perp = vec2(-dir.y, dir.x);

    // Size: mostly tiny, a few larger grains; nearer motes are bigger.
    float dist = length(rel) / extent;
    float grain = pow(h2.x, 6.0);
    // Plain motes are soft puffs of dust: larger, dim, barely streaked. Comets keep a real tail.
    float radius = mix(2.5, 7.0, grain) * (1.6 - dist);
    if (comet) radius = mix(3.0, 5.5, h2.y) * (1.6 - dist);
    float streak = comet ? min(len * 0.5, 160.0) : min(len * 0.12, 6.0);
    if (comet) streak = max(streak, 40.0 * (1.6 - dist));

    vec2 c = kCorners[corner];
    vec2 centerNdc = mix(a, b, 0.5);
    vec2 offsetPx = dir * c.x * (radius + streak) + perp * c.y * radius;
    gl_Position = vec4(centerNdc + offsetPx / viewport * 2.0, clipA.z / clipA.w, 1.0);
    gl_Position.xyz *= 1.0; // already NDC; w = 1

    // Colour: pale sunlit grey with a hint of warmth, brighter when large, dimmer when streaked (energy conserved).
    float twinkle = 0.85 + 0.15 * sin(pc.velocity.w * (2.0 + 4.0 * h2.y) + h2.z * 6.28);
    float brightness = pc.params.z * fade * (0.012 + 0.06 * grain) * twinkle / (1.0 + streak * 0.08);
    // Motes are something you fly past: invisible while the camera hangs still, so they never read as a
    // sky full of faint galaxies (comets keep their slow drift).
    if (!comet) brightness *= smoothstep(0.4, 5.0, len);
    vec3 color = vec3(0.95, 0.92, 0.88) * brightness;
    if (comet) color = vec3(0.85, 0.92, 1.0) * pc.params.z * fade * 0.9 * (0.6 + 0.4 * twinkle);

    // Music: the shockwave sweeps through the field (this is where the depth reads), motes shimmer on
    // their own band, and the wave carries the mood colour.
    float react = frameReact();
    if (react > 0.0) {
        float band = 0.5 * frameSkyBand(rel) + 0.5 * frameBand(int(id * 3u + 5u) & 31);
        float wave = frameWave(rel, extent * 0.35, extent * 0.08);
        color *= 1.0 + react * (comet ? 2.5 : 1.0) * band * band;
        color += frameMood() * brightness * react * 14.0 * wave;
        radius += 1.5 * wave * react;
    }
    vColor = color;
    vTail = comet ? 1.0 : 0.0;
    vUV = c;
    vRadius = radius;
}
