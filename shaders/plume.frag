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

// The exhaust column left behind (crop.y = 1): four kilometres of steam and soot streaming back along the
// rocket's axis, thin and bright near the engines, widening and thinning downrange. Sunlit, not emissive.
const float TRAIL_LEN = 4000.0;

float trailRadiusAt(float d) { return 18.0 + d * 0.07 + 0.00002 * d * d; }

vec4 sampleTrail(vec3 p, float time, vec3 sunM) {
    float d = -p.y;
    if (d < 0.0 || d > TRAIL_LEN) return vec4(0.0);
    float rad = length(p.xz);
    float R = trailRadiusAt(d);
    float x = rad / R;
    if (x > 1.3) return vec4(0.0);
    // The column only drifts slowly against the rocket frame: billows roll, they do not race.
    vec3 q = vec3(p.x, p.y + time * 40.0, p.z) / R;
    float n = fbm(q * 1.6 + vec3(0.0, d * 0.0007, 0.0), 4);
    float n2 = fbm(q * 4.5 + 3.0, 3);
    float edge = smoothstep(1.25, 0.35, x + (n - 0.5) * 0.9);
    // Densest just behind the flame, thinning as it spreads; the first 150 m is still flame territory.
    float along = smoothstep(120.0, 260.0, d) * (1.0 - smoothstep(1800.0, TRAIL_LEN, d));
    float dens = edge * along * (0.45 + 0.9 * n2) * (0.06 + 0.5 * (1.0 - smoothstep(0.0, 2200.0, d)));
    // Lighting: sunlit on the Sun-facing flank, soot-grey in its own shadow, with a bit of sky/ground fill.
    vec3 radial = rad > 1e-3 ? vec3(p.x, 0.0, p.z) / rad : vec3(0.0);
    float sunSide = 0.5 + 0.5 * dot(radial, sunM);
    float depthShade = mix(0.35, 1.0, sunSide) * mix(0.55, 1.0, 1.0 - x * 0.6);
    vec3 albedo = mix(vec3(0.42, 0.40, 0.38), vec3(0.92, 0.91, 0.90), smoothstep(150.0, 900.0, d)); // sooty, then steam
    vec3 col = albedo * (depthShade * 1.05 + 0.18);
    return vec4(col, dens);
}

void main() {
    Craft c = crafts[pc.ids.x];
    float time = pc.uvTransform.x;
    float mpu = pc.shadowInfo.w; // metres per world unit
    bool trail = c.crop.y > 0.5;
    float len = trail ? TRAIL_LEN : LEN;
    // Camera and ray in model space (metres).
    // The model matrix is rotation * scale (scale ~1e-17: metres in parsecs) plus a translation; a float
    // inverse underflows, so undo it by hand: transpose the rotation, divide by the scale.
    mat3 rs = mat3(c.model);
    float scale = length(rs[0]);
    mat3 rT = transpose(rs / scale);
    vec3 camM = -(rT * c.model[3].xyz) / scale;
    vec3 dirM = normalize(rT * normalize(vWorldPos));
    vec3 sunM = normalize(rT * normalize(c.sunDir.xyz));
    // March from the camera to well past the far side of the proxy.
    float tHit = length(vModelPos - camM);
    float tFar = tHit + 2.0 * len + 80.0;
    // Clip the march to the slab y in [-len, 0].
    float t0 = tHit, t1 = tFar;
    if (abs(dirM.y) > 1e-5) {
        float ta = (0.0 - camM.y) / dirM.y, tb = (-len - camM.y) / dirM.y;
        t0 = max(t0, min(ta, tb));
        t1 = min(t1, max(ta, tb));
    }
    if (camM.y < 0.0 && camM.y > -len) t0 = max(tHit * 0.0, 0.0); // camera inside the slab
    if (t1 <= t0) discard;
    const int STEPS = 56;
    float dt = (t1 - t0) / float(STEPS);
    float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    vec3 acc = vec3(0.0);
    float trans = 1.0;
    float irr = c.sunDir.w;
    for (int i = 0; i < STEPS && trans > 0.01; ++i) {
        float t = t0 + (float(i) + jitter) * dt;
        vec3 p = camM + dirM * t;
        if (trail) {
            vec4 s = sampleTrail(p, time, sunM);
            if (s.a <= 0.0) continue;
            float a = 1.0 - exp(-s.a * dt * 0.0005); // thin: at 60 km the exhaust is a faint, wide haze
            acc += s.rgb * irr * a * trans;
            trans *= 1.0 - a;
        } else {
            vec4 s = sampleFlame(p, time);
            if (s.a <= 0.0) continue;
            float a = 1.0 - exp(-s.a * dt * 0.09);
            acc += s.rgb * a * trans * 0.35;
            trans *= 1.0 - a;
        }
    }
    if (trans > 0.999) discard;
    outColor = vec4(acc, trans);
}
