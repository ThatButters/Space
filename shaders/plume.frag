#version 460
#extension GL_EXT_nonuniform_qualifier : enable
#include "common.glsl"
// Rocket exhaust: the plume's cone mesh is only a proxy; each fragment ray-marches an animated volume in
// model space (metres, engine plane at y = 0, exhaust toward -y). A white-hot core with shock diamonds,
// an orange-red mantle that flares with altitude, turbulence streaming downward. Emission only, with
// absorption for depth; premultiplied over the scene, so the ground shows through the thin edges.

struct Craft { mat4 model; vec4 sunDir; vec4 tint; vec4 up; vec4 extra; vec4 crop; };
layout(std430, set = 0, binding = 0) readonly buffer Crafts { Craft crafts[]; };
layout(set = 1, binding = 0) uniform sampler2D uTex[1024];

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 baseColor;
    ivec4 ids;
    vec4 material;
    mat4 shadowMat;
    vec4 shadowInfo;  // w metres per world unit
    vec4 uvTransform; // x time (s)
    vec4 emissive;
} pc;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec3 vModelPos;
layout(location = 0) out vec4 outColor;

const float LEN = 210.0;

float radiusAt(float d) { // d = metres below the engine plane
    // Five F-1s: a 10 m wide core that expands, then flares as the exhaust meets thin air.
    return 5.5 + d * 0.085 + 0.0016 * d * d * smoothstep(40.0, 200.0, d);
}

vec4 sampleFlame(vec3 p, float time) {
    float d = -p.y;
    if (d < 0.0 || d > LEN) return vec4(0.0);
    float rad = length(p.xz);
    float R = radiusAt(d);
    float x = rad / R;
    if (x > 1.25) return vec4(0.0);
    // Turbulence streaming down the plume at ~2.5 km/s (scaled: it must read as motion, not strobe).
    vec3 q = vec3(p.x, p.y + time * 900.0, p.z);
    float n = fbm(q * 0.045, 4);
    float n2 = fbm(q * 0.16 + 7.0, 3);
    float edge = smoothstep(1.2, 0.55, x + (n - 0.5) * 0.7);
    float along = 1.0 - smoothstep(60.0, LEN, d);
    float dens = edge * along * (0.55 + 0.9 * n2);
    // Core: dense, near-white, with shock diamonds every ~14 m for the first 70 m.
    float core = smoothstep(0.55, 0.0, x) * (1.0 - smoothstep(40.0, 90.0, d));
    float diamonds = 0.55 + 0.45 * pow(0.5 + 0.5 * cos(d * 0.45 + 1.2), 3.0);
    core *= diamonds;
    vec3 white = vec3(9.0, 8.4, 7.2);
    vec3 yellow = vec3(6.0, 3.2, 0.9);
    vec3 orange = vec3(3.2, 0.95, 0.18);
    vec3 red = vec3(1.1, 0.22, 0.05);
    vec3 mantle = mix(mix(yellow, orange, smoothstep(0.0, 70.0, d)), red, smoothstep(70.0, 190.0, d));
    vec3 col = mantle * dens + white * core * 1.6;
    float alpha = dens * 0.9 + core * 2.0;
    return vec4(col, alpha);
}

void main() {
    Craft c = crafts[pc.ids.x];
    float time = pc.uvTransform.x;
    float mpu = pc.shadowInfo.w; // metres per world unit
    // Camera and ray in model space (metres).
    // The model matrix is rotation * scale (scale ~1e-17: metres in parsecs) plus a translation; a float
    // inverse underflows, so undo it by hand: transpose the rotation, divide by the scale.
    mat3 rs = mat3(c.model);
    float scale = length(rs[0]);
    mat3 rT = transpose(rs / scale);
    vec3 camM = -(rT * c.model[3].xyz) / scale;
    vec3 dirM = normalize(rT * normalize(vWorldPos));
    // March from the camera to well past the far side of the proxy.
    float tHit = length(vModelPos - camM);
    float tFar = tHit + 2.0 * LEN + 80.0;
    // Clip the march to the slab y in [-LEN, 0].
    float t0 = tHit, t1 = tFar;
    if (abs(dirM.y) > 1e-5) {
        float ta = (0.0 - camM.y) / dirM.y, tb = (-LEN - camM.y) / dirM.y;
        t0 = max(t0, min(ta, tb));
        t1 = min(t1, max(ta, tb));
    }
    if (camM.y < 0.0 && camM.y > -LEN) t0 = max(tHit * 0.0, 0.0); // camera inside the slab
    if (t1 <= t0) discard;
    const int STEPS = 56;
    float dt = (t1 - t0) / float(STEPS);
    float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    vec3 acc = vec3(0.0);
    float trans = 1.0;
    for (int i = 0; i < STEPS && trans > 0.01; ++i) {
        float t = t0 + (float(i) + jitter) * dt;
        vec3 p = camM + dirM * t;
        vec4 s = sampleFlame(p, time);
        if (s.a <= 0.0) continue;
        float a = 1.0 - exp(-s.a * dt * 0.09);
        acc += s.rgb * a * trans * 0.35;
        trans *= 1.0 - a;
    }
    if (trans > 0.999) discard;
    outColor = vec4(acc, trans);
}
