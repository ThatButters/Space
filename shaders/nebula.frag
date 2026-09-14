#version 460
#include "common.glsl"
#include "frame.glsl"

// Volumetric pass: galactic interstellar medium (smooth stellar glow + spiral dust lanes) and a list
// of local nebulae (emission / reflection / dark), raymarched per pixel. Output is premultiplied:
// rgb = in-scattered light, a = transmittance, blended as  dst = rgb + a * dst.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

struct Nebula {
    vec4 posRadius; // xyz centre, w radius (world units)
    vec4 colorA;    // rgb primary emission colour, w density scale
    vec4 colorB;    // rgb secondary colour, w emission scale (per unit length)
    vec4 params;    // x seed, y type (0 emission, 1 reflection, 2 dark), z absorption per unit, w unused
};

layout(std430, set = 0, binding = 0) readonly buffer Volumes {
    vec4 mediumCenter; // xyz galactic centre, w disc scale length
    vec4 mediumShape;  // x disc thickness, y bulge radius, z march tmax, w dust noise scale
    vec4 mediumTint;   // rgb glow colour, w dust density scale
    vec4 mediumMarch;  // x first step length, y glow per unit length, z absorption per unit length
    Nebula nebulae[];
};
FRAME_DATA_BLOCK(1)

layout(push_constant) uniform PushConstants {
    vec4 camRight;
    vec4 camUp;
    vec4 camForward;
    vec4 camPos;
    vec4 params; // x tan(fovY/2), y aspect, z time, w radians per pixel
    vec4 look;   // x galaxy glow, y dust, z nebula gain, w nebula count
} pc;

const int MAX_NEBULAE = 16;
const int MEDIUM_STEPS = 56;
const int NEBULA_STEPS = 28;

// fbm whose fine octaves fade out when the march step is longer than their wavelength (cheap LOD).
float fbmLod(vec3 p, float stepN, int octaves) {
    float sum = 0.0, amp = 0.5, norm = 0.0, freq = 1.0;
    for (int i = 0; i < octaves; ++i) {
        float wavelength = 1.0 / freq;
        float w = amp * clamp(wavelength / max(stepN, 1e-6) - 0.5, 0.0, 1.0);
        sum += w * noise3(p * freq) + (amp - w) * 0.5;
        norm += amp;
        p += vec3(13.7, 5.1, 9.3);
        freq *= 2.02;
        amp *= 0.5;
    }
    return sum / norm;
}

void mediumSample(vec3 p, float stepLen, out vec3 emission, out float absorb) {
    vec3 d = p - mediumCenter.xyz;
    float L = mediumCenter.w;
    float H = mediumShape.x;
    float Rb = mediumShape.y;
    float R = length(d.xz);
    float z = abs(d.y);

    // Unresolved starlight: exponential disc + Gaussian bulge.
    float disc = exp(-R / L) * exp(-z / (H * 2.5));
    float bulge = exp(-dot(d, d) / (Rb * Rb));
    float stellar = disc + 4.0 * bulge;

    // Dust: thinner disc, two log-spiral arms, noisy filaments.
    float dustProfile = exp(-R / (L * 1.2)) * exp(-z / (H * 0.55));
    float theta = atan(d.z, d.x);
    float armPhase = log(max(R, L * 0.05) / (L * 0.2)) / tan(radians(13.0));
    float arms = 0.45 + 0.55 * pow(0.5 + 0.5 * cos(2.0 * (theta - armPhase)), 1.5);
    float nscale = mediumShape.w;
    float n = fbmLod(p / nscale, stepLen / nscale, 5);
    float dust = dustProfile * arms * smoothstep(0.38, 0.78, n) * mediumTint.w;

    vec3 tint = mediumTint.rgb;
    float react = frameReact();
    if (react > 0.0) {
        // Highs cool the galactic glow toward blue, overall level lifts it a little.
        tint *= 1.0 + 0.35 * frameLevel() * react;
    }
    emission = tint * stellar * mediumMarch.y * pc.look.x;
    absorb = dust * mediumMarch.z * pc.look.y;
}

// Marches one nebula. Returns in-scattered colour, transmittance and the distance to its centre.
void marchNebula(int i, vec3 ro, vec3 rd, float jitter, out vec3 C, out float T, out float tc) {
    Nebula nb = nebulae[i];
    vec3 c = nb.posRadius.xyz;
    float R = nb.posRadius.w;
    C = vec3(0.0);
    T = 1.0;
    tc = 1e30;

    vec3 oc = ro - c;
    float b = dot(oc, rd);
    float disc = b * b - (dot(oc, oc) - R * R);
    if (disc < 0.0) return;
    float s = sqrt(disc);
    float t0 = max(-b - s, 0.0);
    float t1 = -b + s;
    if (t1 <= 0.0) return;
    tc = max(-b, 0.0);

    float seed = nb.params.x;
    int type = int(nb.params.y + 0.5);
    float absorb = nb.params.z;
    float ds = (t1 - t0) / float(NEBULA_STEPS);
    float t = t0 + ds * jitter;

    for (int k = 0; k < NEBULA_STEPS; ++k) {
        vec3 q = (ro + rd * t - c) / R;
        float r = length(q);
        if (r < 1.0) {
            vec3 warp = vec3(fbm(q * 1.5 + seed, 3), fbm(q * 1.5 + seed + 5.2, 3), fbm(q * 1.5 + seed + 9.1, 3)) - 0.5;
            vec3 qq = q + warp * 0.7;
            float n = fbm(qq * 2.6 + seed, 5);
            float shape = smoothstep(1.0, 0.2, r);
            float dens = max(n * 1.8 - 0.75, 0.0) * shape * nb.colorA.w;
            float mixn = noise3(qq * 4.0 + seed * 3.0);
            float react = frameReact();
            if (react > 0.0) {
                // Mids sweep the colour balance through the nebula, beats flash the ring through it.
                mixn = fract(mixn + 0.45 * frameMid() * react + 0.15 * sin(seed + frame.misc.w * 0.6) * react);
                dens *= 1.0 + react * (0.6 * frameSkyBand(c - ro) + 1.2 * frameWave(c - ro, 25.0, 40.0));
            }
            vec3 palette = mix(nb.colorA.rgb, nb.colorB.rgb, mixn);
            if (react > 0.0) palette = mix(palette, palette * frameMood() * 1.6, 0.5 * frameLevel() * react);
            vec3 emis = type == 2 ? vec3(0.0) : palette * dens * nb.colorB.w;
            C += T * emis * ds * pc.look.z;
            T *= exp(-dens * absorb * ds);
            if (T < 0.004) break;
        }
        t += ds;
    }
}

void main() {
    vec2 ndc = vec2(vUV.x * 2.0 - 1.0, 1.0 - vUV.y * 2.0);
    float tanHalf = pc.params.x, aspect = pc.params.y;
    vec3 rd = normalize(pc.camForward.xyz + ndc.x * tanHalf * aspect * pc.camRight.xyz + ndc.y * tanHalf * pc.camUp.xyz);
    vec3 ro = pc.camPos.xyz;
    float jitter = hash13(vec3(gl_FragCoord.xy, 1.7));

    int count = min(int(pc.look.w + 0.5), MAX_NEBULAE);
    vec3 nebC[MAX_NEBULAE];
    float nebT[MAX_NEBULAE];
    float nebDist[MAX_NEBULAE];
    float nebMediumT[MAX_NEBULAE];
    for (int i = 0; i < count; ++i) {
        marchNebula(i, ro, rd, jitter, nebC[i], nebT[i], nebDist[i]);
        nebMediumT[i] = 1.0;
    }

    // Galactic medium, log-spaced steps so nearby dust is sharp and the far disc is cheap.
    vec3 col = vec3(0.0);
    float T = 1.0;
    float t0 = mediumMarch.x;
    float tmax = mediumShape.z;
    float ratio = pow(tmax / t0, 1.0 / float(MEDIUM_STEPS));
    float t = t0 * pow(ratio, jitter);
    for (int k = 0; k < MEDIUM_STEPS; ++k) {
        float tn = t * ratio;
        float ds = tn - t;
        vec3 p = ro + rd * (t + 0.5 * ds);

        vec3 e;
        float a;
        mediumSample(p, ds, e, a);

        // Light from this step passes through every nebula that sits in front of it.
        float front = 1.0;
        for (int i = 0; i < count; ++i) {
            if (nebDist[i] < t) front *= nebT[i];
            if (nebDist[i] >= t && nebDist[i] < tn) nebMediumT[i] = T;
        }
        col += T * e * ds * front;
        T *= exp(-a * ds);
        if (T < 0.002) break;
        t = tn;
    }

    // Composite the nebulae: each attenuated by the medium and by nebulae closer than itself.
    float totalT = T;
    for (int i = 0; i < count; ++i) {
        float front = 1.0;
        for (int j = 0; j < count; ++j)
            if (j != i && nebDist[j] < nebDist[i]) front *= nebT[j];
        col += nebC[i] * nebMediumT[i] * front;
        totalT *= nebT[i];
    }

    outColor = vec4(col, totalT);
}
