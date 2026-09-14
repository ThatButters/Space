#version 460
#include "common.glsl"
#include "frame.glsl"
// Music nebula: a volumetric cloud field around the camera. Density and colour follow the spectrum
// by direction (bass warm and low, highs cool and high) and swell with the phrase energy. The noise
// domain is periodic in the wrap cell so the field is stable at any distance from the Sun.

FRAME_DATA_BLOCK(0)

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PushConstants {
    vec4 camRight;
    vec4 camUp;
    vec4 camForward;
    vec4 phase;  // xyz camera phase inside the cloud cell (0..1)
    vec4 params; // x tan(fovY/2), y aspect, z time, w radians per pixel
    vec4 look;   // x strength, y cell extent (world units), z section energy, w level
} pc;

const int STEPS = 22;

// Periodic value noise on a unit cell with integer frequency, so wrapping is seamless.
float pnoise(vec3 p, float freq) {
    vec3 s = p * freq;
    vec3 i = floor(s), f = fract(s);
    f = f * f * (3.0 - 2.0 * f);
    #define H(o) hash13(mod(i + o, freq))
    return mix(mix(mix(H(vec3(0,0,0)), H(vec3(1,0,0)), f.x), mix(H(vec3(0,1,0)), H(vec3(1,1,0)), f.x), f.y),
               mix(mix(H(vec3(0,0,1)), H(vec3(1,0,1)), f.x), mix(H(vec3(0,1,1)), H(vec3(1,1,1)), f.x), f.y), f.z);
    #undef H
}

float pfbm(vec3 p, float drift) {
    float sum = 0.0, amp = 0.5;
    float freq = 3.0;
    for (int i = 0; i < 4; ++i) {
        sum += amp * pnoise(p + vec3(drift * 0.11, drift * 0.07, -drift * 0.05) * (1.0 / freq), freq);
        freq *= 2.0;
        amp *= 0.5;
    }
    return sum;
}

// Warm-to-cool colour by height/direction, blended with the music mood.
vec3 bandColor(float y) {
    vec3 low = vec3(1.0, 0.45, 0.20), mid = vec3(0.75, 0.35, 0.95), high = vec3(0.30, 0.75, 1.0);
    float t = y * 0.5 + 0.5;
    vec3 c = t < 0.5 ? mix(low, mid, t * 2.0) : mix(mid, high, (t - 0.5) * 2.0);
    return mix(c, c * frameMood() * 1.3, 0.5);
}

void main() {
    float strength = pc.look.x;
    if (strength <= 0.0) discard;
    vec2 ndc = vec2(vUV.x * 2.0 - 1.0, 1.0 - vUV.y * 2.0);
    float tanHalf = pc.params.x, aspect = pc.params.y, time = pc.params.z;
    vec3 rd = normalize(pc.camForward.xyz + ndc.x * tanHalf * aspect * pc.camRight.xyz + ndc.y * tanHalf * pc.camUp.xyz);

    float extent = pc.look.y;
    float radius = 0.5 * extent;          // march to the inscribed sphere
    float jitter = hash13(vec3(gl_FragCoord.xy, 3.1));
    float ds = radius / float(STEPS);
    float t = ds * jitter;

    float section = pc.look.z, level = pc.look.w;
    float region = frameSkyBand(rd);       // how much of the mix lives in this direction
    float drift = time * 0.35;

    vec3 col = vec3(0.0);
    float T = 1.0;
    for (int i = 0; i < STEPS; ++i) {
        vec3 rel = rd * t;
        vec3 q = rel / extent + pc.phase.xyz; // cloud-cell coordinates, periodic
        float n = pfbm(q, drift);
        float shell = smoothstep(0.0, 0.25, t / radius) * (1.0 - smoothstep(0.5, 1.0, t / radius)); // hollow near the eye, fades at the edge
        // Threshold rises when the music is quiet so the cloud thins to wisps, and drops with energy.
        float thresh = 0.70 - 0.14 * section - 0.10 * region * region;
        float dens = max(n - thresh, 0.0) * 2.5 * shell * strength * (0.35 + 0.65 * level);
        vec3 c = bandColor(rd.y) * (0.6 + 1.2 * region);
        float sigma = dens * 0.35 / radius;  // per unit length: wisps, not fog
        col += T * c * dens * (ds / radius) * 0.45;
        T *= exp(-sigma * ds);
        if (T < 0.02) break;
        t += ds;
    }
    outColor = vec4(col, T);
}
