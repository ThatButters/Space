#version 460
#extension GL_EXT_nonuniform_qualifier : enable
#include "common.glsl"
#include "atmosphere.glsl"

// Volumetric clouds for Earth, seen from within a few thousand kilometres: the cloud map (and today's
// satellite cover) sets where cloud is, 3D gradient noise gives it cauliflower tops and ragged edges, and
// the view ray is marched through a layer from ~1.5 to ~6.5 km with a short march toward the Sun at each
// sample for self-shadowing. Drawn on a shell at the cloud tops, premultiplied over the surface (which
// fades its own flat cloud layer out over the same distance); the atmosphere pass composites over this.

struct Body {
    vec4 posRadius;
    vec4 rotation;
    vec4 color;
    vec4 params; // type, seed, atmosphere, emissive
    ivec4 tex;   // day, night, clouds, ring alpha
    ivec4 tiles; // tiled day map; w = live cloud map (-1 = none)
    ivec4 relief;
    vec4 reliefParams;
    ivec4 patchTex;
    vec4 patchParams;
    ivec4 atmoTex;
    vec4 atmoParams;
};
layout(std430, set = 0, binding = 0) readonly buffer Bodies { Body bodies[]; };
layout(set = 1, binding = 0) uniform sampler2D uTex[1024];
#include "atmosphere_path.glsl"

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 sunPos;
    vec4 params; // x time, y sun radiance, z shell scale, w unused here
    mat4 shadowMat;
    vec4 shadowInfo;
    vec4 anchorWorld;
    vec4 anchorLocal;
    vec4 patchAnchor;
    vec4 patchEast;
    vec4 patchNorth;
} pc;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vLocal;
layout(location = 2) in vec3 vNormal;
layout(location = 3) flat in int vBody;
layout(location = 4) in float vRadial;
layout(location = 5) flat in float vBoost;
layout(location = 0) out vec4 outColor;

const float kKmPerParsecF = 3.0856775814913673e13;
const float kBaseKm = 1.5, kTopKm = 6.5;   // the layer
const float kSigmaPerKm = 7.0;             // extinction of dense cloud (under real, so edges stay translucent)
const int STEPS = 32;

vec2 sphereUv(vec3 p) { return vec2(0.5 - atan(p.z, p.x) / (2.0 * PI), acos(clamp(p.y, -1.0, 1.0)) / PI); }
vec3 rotateInv(vec4 q, vec3 v) { vec4 c = vec4(-q.xyz, q.w); return v + 2.0 * cross(c.xyz, cross(c.xyz, v) + c.w * v); }

vec2 raySphere(vec3 ro, vec3 rd, float r) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - r * r;
    float d = b * b - c;
    if (d < 0.0) return vec2(1.0, -1.0);
    float s = sqrt(d);
    return vec2(-b - s, -b + s);
}

// Cloud cover 0..1 at a body-local direction, from the map (drifting east) and today's satellite cover.
float coverAt(Body b, vec3 pl, float time) {
    vec2 uv = sphereUv(pl);
    float c = b.tex.z >= 0 ? textureLod(uTex[nonuniformEXT(b.tex.z)], uv + vec2(time * 2e-6, 0.0), 0.0).r
                           : smoothstep(0.55, 0.75, fbm(pl * 5.0 + b.params.y * 5.0, 4));
    if (b.tiles.w >= 0) {
        vec2 live = textureLod(uTex[nonuniformEXT(b.tiles.w)], uv, 0.0).rg;
        c = mix(c, live.r, live.g);
    }
    return c;
}

// Density 0..1 at a point p (planet radii, planet-centred, world orientation). base/top in radii.
float densityAt(Body b, vec3 p, float base, float top, float time, out float hn) {
    float h = length(p);
    hn = clamp((h - base) / (top - base), 0.0, 1.0);
    vec3 pl = rotateInv(b.rotation, p);
    float cov = coverAt(b, normalize(pl), time);
    if (cov <= 0.03) return 0.0;
    // The map's 2 km pixels make continent-sized blobs; a ~25 km cell field breaks decks into cells,
    // streets and gaps the way real cloud fields are organised.
    vec3 q = pl + vec3(time * 1.2e-5, 0.0, 0.0);
    float n0 = gfbm(q * 300.0, 3);
    float gap = smoothstep(0.32, 0.62, n0);                  // cells: about half of a solid deck opens up
    float d0 = smoothstep(0.05, 0.9, cov * mix(0.3, 1.25, gap));
    if (d0 <= 0.002) return 0.0;
    // Cauliflower: three scales of 3D gradient noise (~5 km, ~1.3 km, ~400 m). The coarse one also
    // sets how tall each tower is, so the tops are domes and hollows rather than a plateau.
    float n1 = gfbm(q * 1300.0, 2);
    float n2 = gfbm(q * 4800.0 + 3.1, 2);
    // Columns: flat bases, tops that rise with cover and with the coarse noise.
    float colTop = (0.2 + 0.8 * d0) * (0.5 + 0.5 * n1) + 0.2 * (n2 - 0.5);
    float prof = smoothstep(0.0, 0.12, hn) * (1.0 - smoothstep(colTop - 0.35, colTop, hn));
    if (prof <= 0.002) return 0.0;
    float n3 = gfbm(q * 16000.0 + 7.7, 2);
    // Erosion, strongest in thin cover and toward the tops, so edges fray into wisps.
    float erode = 0.75 + 0.5 * (1.0 - d0) + 0.3 * hn;
    float dens = d0 * 1.15 - (1.0 - n1) * 0.55 * erode - (1.0 - n2) * 0.4 * erode - (1.0 - n3) * 0.2;
    // Soft: density ramps in gently at the margins (a thin veil of haze before the cloud proper).
    dens = clamp(dens, 0.0, 1.0);
    float veil = 0.06 * d0 * smoothstep(0.0, 0.15, hn) * (1.0 - smoothstep(0.4, 0.8, hn));
    return (dens * dens * (3.0 - 2.0 * dens) * 0.9 + veil) * prof;
}

float hg(float mu, float g) {
    float k = 1.0 + g * g - 2.0 * g * mu;
    return (1.0 - g * g) / (4.0 * PI * k * sqrt(k));
}

void main() {
    Body b = bodies[vBody];
    if (int(b.params.x + 0.5) != 2 || b.tex.z < 0) discard; // Earth only
    float R = b.posRadius.w;
    vec3 center = b.posRadius.xyz;
    float Rkm = R * kKmPerParsecF;
    float altKm = (length(center) - R) * kKmPerParsecF;
    float vol = 1.0 - smoothstep(2500.0, 5000.0, altKm); // hands over to the flat map far out
    if (vol <= 0.0) discard;

    vec3 ro = -center / R;
    vec3 rd = normalize(vWorldPos);
    float top = 1.0 + kTopKm / Rkm, base = 1.0 + kBaseKm / Rkm;
    float r0 = length(ro);
    bool insideTop = r0 < top;
    if (insideTop == gl_FrontFacing) discard;
    vec2 hT = raySphere(ro, rd, top);
    if (hT.y <= 0.0) discard;
    vec2 hB = raySphere(ro, rd, base);
    vec2 hG = raySphere(ro, rd, 1.0);
    float t0, t1;
    if (r0 > top)       { t0 = max(hT.x, 0.0); t1 = hB.x > 0.0 ? hB.x : hT.y; }
    else if (r0 > base) { t0 = 0.0;            t1 = hB.x > 0.0 ? hB.x : hT.y; }
    else                { t0 = max(hB.y, 0.0); t1 = hT.y; }
    if (hG.x > 0.0 && hG.x < t1) t1 = hG.x;
    t1 = min(t1, t0 + 400.0 / Rkm); // grazing views: 400 km of layer is plenty
    if (t1 <= t0) discard;

    vec3 sunDir = normalize(pc.sunPos.xyz - center);
    const float AU = 4.848e-6;
    float sunDist2 = max(dot(pc.sunPos.xyz - center, pc.sunPos.xyz - center), 1e-30);
    float irradiance = pow((AU * AU) / sunDist2, 0.3);
    float mu = dot(rd, sunDir);
    // Forward-peaked with a little back-scatter, relative to isotropic.
    float phase = (0.7 * hg(mu, 0.55) + 0.3 * hg(mu, -0.2)) * 4.0 * PI;

    float time = pc.params.x;
    float jitter = hash13(vec3(gl_FragCoord.xy, fract(time * 7.31) * 100.0));
    float ds = (t1 - t0) / float(STEPS);
    float dsKm = ds * Rkm;
    float T = 1.0;
    vec3 col = vec3(0.0);
    for (int i = 0; i < STEPS; ++i) {
        float t = t0 + (float(i) + jitter) * ds;
        vec3 p = ro + rd * t;
        float hn;
        float dens = densityAt(b, p, base, top, time, hn);
        if (dens <= 0.002) continue;
        // Light: four steps toward the Sun, short near the sample and growing, through the layer.
        float odSun = 0.0, dl = 0.25;
        for (int j = 0; j < 4; ++j) {
            vec3 q = p + sunDir * ((float(j) + 0.5) * dl / Rkm);
            float hq;
            odSun += densityAt(b, q, base, top, time, hq) * kSigmaPerKm * dl;
            dl *= 1.6;
        }
        bool night = raySphere(p, sunDir, 1.0).x > 0.0;
        // Beer with a multiple-scatter tail, and the powder term that darkens the deep interior of thick
        // cloud relative to its lit, low-density rim.
        float powder = 1.0 - 0.5 * exp(-dens * 6.0);
        float lit = night ? 0.0 : (0.5 * exp(-odSun) + 0.5 * exp(-odSun * 0.12)) * powder;
        vec3 up = normalize(p);
        float day = smoothstep(-0.08, 0.25, dot(up, sunDir)); // no sky fill on the night side
        vec3 sunT = vec3(1.0); // reddened near the terminator by the air above the clouds
        if (b.atmoTex.x >= 0) {
            AtmoClass ac = kAtmo[b.atmoTex.z];
            sunT = textureLod(uTex[nonuniformEXT(b.atmoTex.x)],
                              atmoTransmittanceUv(ac.radiusKm, ac.radiusKm + ac.topKm, length(p) * ac.radiusKm, dot(up, sunDir)), 0.0).rgb;
        }
        vec3 sun = vec3(1.0, 0.98, 0.95) * lit * phase * 0.4 * sunT;
        vec3 sky = vec3(0.30, 0.45, 0.75) * (0.07 + 0.2 * hn) * day;   // blue fill from above
        vec3 ground = vec3(0.10, 0.11, 0.12) * (0.25 * (1.0 - hn)) * day; // a little bounce from below
        float a = 1.0 - exp(-kSigmaPerKm * dens * dsKm);
        col += T * a * (sun + sky + ground) * irradiance;
        T *= 1.0 - a;
        if (T < 0.015) break;
    }
    if (b.atmoTex.x >= 0) {
        // Aerial perspective between the camera and the cloud (the ground behind carries its own).
        AtmoClass ac = kAtmo[b.atmoTex.z];
        vec3 ins, tr;
        atmoPathScatter(b.atmoTex, ro * ac.radiusKm, rd, 0.5 * (t0 + t1) * ac.radiusKm, sunDir, 10, jitter, ins, tr);
        col = col * tr + ins * irradiance * PI * (1.0 - T);
    }
    outColor = vec4(col * vol, 1.0 - (1.0 - T) * vol);
}
