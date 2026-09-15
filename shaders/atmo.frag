#version 460
#extension GL_EXT_nonuniform_qualifier : enable
#include "common.glsl"
#include "frame.glsl"
#include "atmosphere.glsl"
FRAME_DATA_BLOCK(1)
// Atmospheres, physically based: light scattered toward the camera is ray marched per pixel through the
// body's real atmosphere (Rayleigh, aerosols and absorbers at their real scale heights), with the Sun's
// transmittance and all the higher scattering orders read from lookup tables baked at startup
// (atmosphere.glsl). Drawn on a shell at the top of each atmosphere, premultiplied over what lies behind:
// rgb = scattered light, a = transmittance. From afar it is the thin bright limb; looking down, the haze
// over the ground; from inside, the sky. Earth also carries the aurora and the airglow layer.

struct Body {
    vec4 posRadius;
    vec4 rotation;
    vec4 color;
    vec4 params; // type, seed, atmosphere strength, emissive
    ivec4 tex;
    ivec4 tiles; // tiled day map: first index (-1 = none), columns, rows
    ivec4 relief;       // normal map (east/north), height map, -1 = none
    vec4 reliefParams;  // height min km, height range km, normal strength, detail kind
    ivec4 patchTex;     // local terrain patch: albedo, normal, height (-1 = none)
    vec4 patchParams;   // height min m, height range m, metres per texel, 1 when active
    ivec4 atmoTex;      // transmittance LUT, multiple-scattering LUT, atmosphere class (-1 = none)
    vec4 atmoParams;    // x top of the atmosphere / radius
};
layout(std430, set = 0, binding = 0) readonly buffer Bodies { Body bodies[]; };
layout(set = 2, binding = 0) uniform sampler2D uTex[1024];

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 sunPos;
    vec4 params; // x time, y sun radiance, z shell (< 0: each body's own atmosphere), w aurora strength
} pc;

layout(location = 0) in vec3 vWorldPos; // camera-relative
layout(location = 1) in vec3 vLocal;
layout(location = 2) in vec3 vNormal;
layout(location = 3) flat in int vBody;
layout(location = 4) in float vRadial;
layout(location = 0) out vec4 outColor;

vec3 rotateInv(vec4 q, vec3 v) { vec4 c = vec4(-q.xyz, q.w); return v + 2.0 * cross(c.xyz, cross(c.xyz, v) + c.w * v); }

// Auroral curtains, geometry after the real thing:
//  - centred on the geomagnetic dipole pole (80.7 N, 72.7 W; the south oval is antipodal),
//  - an oval, not a cap: ~15 deg from the pole on the day side, ~23 deg at midnight, wider at night,
//  - expands toward the equator with geomagnetic activity (here: the music's bass),
//  - green (557 nm oxygen) brightest near 110 km, red (630 nm) above 200 km, violet nitrogen on top.
vec3 aurora(vec3 pLocal, vec3 sunLocal, float h, float time, float strength) {
    const vec3 mpole = normalize(vec3(0.1616 * 0.2974, 0.9869, 0.1616 * 0.9548)); // lat 80.7, lon -72.7 (east = -z)
    float mlat = asin(clamp(dot(pLocal, mpole), -1.0, 1.0));
    vec3 pole = mlat >= 0.0 ? mpole : -mpole;
    float colat = 1.5707963 - abs(mlat);
    if (colat > 0.6) return vec3(0.0);

    // Magnetic local time: angle around the pole measured from the sunward direction.
    vec3 e1 = normalize(sunLocal - pole * dot(sunLocal, pole));
    vec3 e2 = cross(pole, e1);
    vec3 pp = normalize(pLocal - pole * dot(pLocal, pole));
    float mlt = atan(dot(pp, e2), dot(pp, e1)); // 0 = noon, pi = midnight
    float night = 0.5 - 0.5 * cos(mlt);

    float react = frameReact();
    float bass = react > 0.0 ? frameBass() : 0.0;
    float activity = 0.3 + 0.7 * bass * react;                        // Kp stand-in
    float ovalColat = radians(15.0 + 8.0 * night + 5.0 * activity);
    float ovalWidth = radians(2.0 + 2.5 * night + 1.5 * activity);
    float oval = exp(-pow((colat - ovalColat) / ovalWidth, 2.0));
    if (oval < 0.002) return vec3(0.0);

    float band = react > 0.0 ? frameSkyBand(pLocal) : 0.3;
    float treble = react > 0.0 ? frameTreble() : 0.2;
    // Curtains: fine structure along the oval, drifting; rays sharpen with activity and the highs.
    float along = mlt * 8.0 + colat * 50.0;
    float curtain = fbm(vec3(along + time * 0.15, colat * 40.0, time * 0.05), 4);
    float rays = pow(max(noise3(vec3(along * 8.0 - time * 0.4, colat * 30.0, 2.0)), 0.0), 3.0);
    float shape = smoothstep(0.42, 0.75, curtain + 0.25 * rays * (treble + activity));

    // Vertical structure in planet radii: 100 km = 0.0157 R.
    float km = h * 6371.0;
    float green = exp(-pow((km - 110.0) / 35.0, 2.0)) + 0.35 * exp(-pow((km - 150.0) / 60.0, 2.0));
    float red = smoothstep(180.0, 260.0, km) * (1.0 - smoothstep(260.0, 330.0, km)) * 0.55;
    float violet = exp(-pow((km - 95.0) / 20.0, 2.0)) * 0.35 * activity;
    vec3 c = vec3(0.25, 1.0, 0.35) * green + vec3(0.95, 0.22, 0.30) * red + vec3(0.55, 0.30, 1.0) * violet;
    float music = 0.6 + react * (1.6 * band + 1.2 * treble);
    return c * oval * shape * strength * music;
}

// Sample positions along [tA, tB], densest at tc: where a grazing ray runs deepest into the atmosphere,
// or at the ground when looking down.
float atmoEdge(float u, float tA, float tc, float tB) {
    float w = (tc - tA) / max(tB - tA, 1e-6);
    if (u <= w) {
        float k = 1.0 - u / max(w, 1e-6);
        return tc - (tc - tA) * k * k;
    }
    float k = (u - w) / max(1.0 - w, 1e-6);
    return tc + (tB - tc) * k * k;
}

void main() {
    Body b = bodies[vBody];
    int type = int(b.params.x + 0.5);
    int cls = b.atmoTex.z;
    if (cls < 0) discard;
    AtmoClass a = kAtmo[cls];
    float R = a.radiusKm, Rt = R + a.topKm;

    // Geometry in kilometres, relative to the planet's centre.
    float kmPerUnit = R / b.posRadius.w;
    vec3 centre = b.posRadius.xyz;
    vec3 ro = -centre * kmPerUnit;
    vec3 rd = normalize(vWorldPos);

    // From outside only the near faces of the shell draw; from inside only the far faces.
    bool inside = dot(ro, ro) < Rt * Rt;
    if (inside == gl_FrontFacing) discard;
    vec2 hit = atmoRaySphere(ro, rd, Rt);
    if (hit.y <= 0.0) discard;
    float tA = max(hit.x, 0.0), tB = hit.y;
    vec2 ground = atmoRaySphere(ro, rd, R);
    // Over the ground the surface and cloud shaders carry the aerial perspective; here only the aurora is
    // added there (seen from above), and the sky and limb everywhere else.
    bool groundHit = ground.x > 0.0 && ground.x < tB;
    if (groundHit && (type != 2 || pc.params.w <= 0.0)) discard;
    if (groundHit) tB = ground.x;
    if (tB <= tA) discard;

    vec3 sunDir = normalize(pc.sunPos.xyz - centre);
    float mu = dot(rd, sunDir);
    float phaseR = atmoPhaseRayleigh(mu);
    float phaseM = atmoPhaseMie(a.mieG, mu);

    const int STEPS = 40;
    float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233)) + fract(pc.params.x * 0.37) * 91.7) * 43758.5453);
    float tc = clamp(-dot(ro, rd), tA, tB);
    vec3 L = vec3(0.0), T = vec3(1.0);
    vec3 auroraLight = vec3(0.0);
    float tPrev = tA;
    for (int i = 0; i < STEPS; ++i) {
        float tNext = atmoEdge(float(i + 1) / float(STEPS), tA, tc, tB);
        float dt = tNext - tPrev;
        float t = tPrev + dt * jitter;
        tPrev = tNext;
        if (dt <= 0.0) continue;
        vec3 p = ro + rd * t;
        float pr = length(p);
        vec3 up = p / pr;
        vec3 sR, sM;
        vec3 ext = atmoExtinction(a, pr - R, sR, sM);
        float muS = dot(up, sunDir);
        // In the planet's shadow past the terminator, softened over about half a scale height.
        float along = dot(p, sunDir);
        float miss = length(p - along * sunDir);
        float lit = along > 0.0 ? 1.0 : smoothstep(R - 0.5 * a.rayleighH, R + 0.5 * a.rayleighH, miss);
        vec3 sunT = textureLod(uTex[nonuniformEXT(b.atmoTex.x)], atmoTransmittanceUv(R, Rt, pr, muS), 0.0).rgb * lit;
        vec3 ms = textureLod(uTex[nonuniformEXT(b.atmoTex.y)], atmoMsUv(R, Rt, pr, muS), 0.0).rgb;
        vec3 S = (sR * phaseR + sM * phaseM) * sunT + (sR + sM) * ms;
        vec3 stepT = exp(-ext * dt);
        if (!groundHit) L += T * S * (vec3(1.0) - stepT) / max(ext, vec3(1e-12));

        if (type == 2 && pc.params.w > 0.0) {
            // Aurora and airglow are self-emissive: not scaled by sunlight, brighter on the night side.
            vec3 pl = rotateInv(b.rotation, p / R);
            vec3 sl = normalize(rotateInv(b.rotation, sunDir));
            float hKm = pr - R;
            float dsR = dt / 6371.0;
            float night = lit < 0.5 ? 1.0 : 0.25;
            auroraLight += T * aurora(pl, sl, hKm / 6371.0, pc.params.x, pc.params.w) * dsR * night;
            // Airglow: the faint green oxygen layer near 90 km that rims the night limb in photos from orbit.
            float glow = exp(-pow((hKm - 92.0) / 16.0, 2.0)) * (lit < 0.5 ? 1.0 : 0.15);
            auroraLight += T * vec3(0.18, 0.55, 0.28) * glow * dsR * 0.005;
        }
        if (!groundHit) T *= stepT;
    }

    // Sunlight: the same compressed inverse-square the surfaces use. Surfaces are lit as
    // albedo x cos x irradiance (no 1/pi), so scattered radiance carries the same factor of pi.
    const float AU = 4.848e-6;
    vec3 toSun = pc.sunPos.xyz - centre;
    float sunDist2 = max(dot(toSun, toSun), 1e-30);
    float irradiance = pow((AU * AU) / sunDist2, 0.3);
    vec3 light = L * irradiance * PI + auroraLight * 40.0;
    outColor = vec4(light, dot(T, vec3(1.0 / 3.0)));
}
