#version 460
#extension GL_EXT_nonuniform_qualifier : enable
#include "common.glsl"

// Sun, planets and moons. The mesh is only a proxy slightly larger than the body (drawn back faces
// only); every pixel ray-traces the exact sphere in double precision and writes its true depth, so
// the ground is perfectly round from orbit down to standing beside a lander. Surfaces use
// equirectangular maps (procedural fallbacks), relief maps (normals plus a height map for terrain
// self-shadowing), close-range procedural regolith anchored on the CPU in double precision, and
// spacecraft shadows from a sun-aligned depth map.

struct Body {
    vec4 posRadius;
    vec4 rotation;
    vec4 color;
    vec4 params; // type, seed, atmosphere / ring inner, emissive
    ivec4 tex;   // day, night, clouds, ring alpha (-1 = none)
    ivec4 tiles; // tiled day map: first index (-1 = none), columns, rows
    ivec4 relief;       // normal map (east/north), height map, -1 = none
    vec4 reliefParams;  // height min km, height range km, normal strength, detail kind
    ivec4 patchTex;     // local terrain patch: albedo, normal, height (-1 = none)
    vec4 patchParams;   // height min m, height range m, metres per texel, 1 when active
};
layout(std430, set = 0, binding = 0) readonly buffer Bodies { Body bodies[]; };
layout(set = 1, binding = 0) uniform sampler2D uTex[1024];

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 sunPos;
    vec4 params;      // x time, y sun radiance, z shell scale (0 = surface pass), w aurora strength
    mat4 shadowMat;   // camera-relative world -> shadow map clip (xy -1..1, z reversed depth)
    vec4 shadowInfo;  // x shadow texture index (-1 = none), y 1 / resolution, z depth bias, w metres per world unit
    vec4 anchorWorld; // xyz camera-relative surface anchor, w index of the body it sits on (-1 = none)
    vec4 anchorLocal; // xyz the anchor in body-local metres modulo 4096, w number of sphere bodies
    vec4 patchAnchor; // xy anchor uv in the active terrain patch, z du per metre east, w dv per metre north
    vec4 patchEast;   // xyz body-local east at the anchor
    vec4 patchNorth;  // xyz body-local north at the anchor
} pc;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vLocal;
layout(location = 2) in vec3 vNormal;
layout(location = 3) flat in int vBody;
layout(location = 4) in float vRadial;
layout(location = 0) out vec4 outColor;

const int SUN = 0, ROCKY = 1, EARTH = 2, GAS = 3, ICE = 4, RING = 5;

// Equirectangular lookup: u along longitude (seam at -x, east = +x toward -z so the map is not
// mirrored in a right-handed frame), v from the north pole down.
vec2 sphereUv(vec3 p) { return vec2(0.5 - atan(p.z, p.x) / (2.0 * PI), acos(clamp(p.y, -1.0, 1.0)) / PI); }

// Derivatives across the longitude seam wrap around; fix them so the mip level stays continuous.
vec2 seamDx, seamDy;
void prepareSeam(vec2 uv) {
    seamDx = dFdx(uv);
    seamDy = dFdy(uv);
    if (abs(seamDx.x) > 0.5) seamDx.x -= sign(seamDx.x);
    if (abs(seamDy.x) > 0.5) seamDy.x -= sign(seamDy.x);
}
vec4 sampleMap(int index, vec2 uv) { return textureGrad(uTex[nonuniformEXT(index)], uv, seamDx, seamDy); }
// Day map: a single equirect texture, or a grid of tiles (row-major from the north-west corner).
vec4 sampleDay(Body b, vec2 uv) {
    if (b.tiles.x < 0) return sampleMap(b.tex.x, uv);
    vec2 grid = vec2(b.tiles.y, b.tiles.z);
    vec2 scaled = clamp(uv, 0.0, 0.999999) * grid;
    ivec2 cell = ivec2(floor(scaled));
    int index = b.tiles.x + cell.y * b.tiles.y + cell.x;
    return textureGrad(uTex[nonuniformEXT(index)], scaled - vec2(cell), seamDx * grid, seamDy * grid);
}
vec3 rotateInv(vec4 q, vec3 v) { vec4 c = vec4(-q.xyz, q.w); return v + 2.0 * cross(c.xyz, cross(c.xyz, v) + c.w * v); }
vec3 rotateVec(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }

vec3 sunSurface(vec3 p, float seed, float time) {
    float g1 = fbm(p * 18.0 + seed + time * 0.02, 4);
    float g2 = fbm(p * 60.0 - seed + time * 0.05, 3);
    return vec3(1.0, 0.85, 0.6) * (0.75 + 0.35 * g1 + 0.15 * g2);
}

vec3 rockySurface(vec3 p, vec3 base, float seed) {
    float n = fbm(p * 6.0 + seed, 5);
    float craters = smoothstep(0.55, 0.75, noise3(p * 30.0 + seed * 7.0)) * 0.35;
    return base * (0.65 + 0.7 * n) * (1.0 - craters);
}

vec3 earthSurface(vec3 p, float seed, out float ocean) {
    vec3 warp = vec3(fbm(p * 2.0 + seed, 3), fbm(p * 2.0 + seed + 4.0, 3), fbm(p * 2.0 + seed + 8.0, 3)) - 0.5;
    float continents = fbm(p * 3.2 + warp * 0.9 + seed, 6);
    float land = smoothstep(0.50, 0.53, continents);
    ocean = 1.0 - land;
    float ice = smoothstep(0.86, 0.94, abs(p.y) + 0.06 * fbm(p * 8.0, 3));
    vec3 seaCol = mix(vec3(0.02, 0.09, 0.28), vec3(0.03, 0.22, 0.42), smoothstep(0.35, 0.5, continents));
    float veg = fbm(p * 9.0 + seed * 2.0, 4);
    vec3 landCol = mix(vec3(0.10, 0.22, 0.06), vec3(0.45, 0.38, 0.22), smoothstep(0.35, 0.7, veg));
    return mix(mix(seaCol, landCol, land), vec3(0.9, 0.93, 0.97), ice);
}

vec3 gasSurface(vec3 p, vec3 base, float seed, bool ice) {
    float warp = fbm(p * 4.0 + seed, 4) - 0.5;
    float bands = sin((p.y + warp * 0.12) * (ice ? 9.0 : 22.0) + seed);
    float storms = smoothstep(0.62, 0.8, fbm(p * 7.0 + vec3(0.0, seed, 0.0), 5));
    vec3 c = mix(base * 0.68, base * 1.15, bands * 0.5 + 0.5);
    if (!ice) c = mix(c, base * vec3(1.2, 0.85, 0.75), storms * 0.6);
    return ice ? mix(c, base, 0.6) : c;
}

// ---- Close-range regolith ------------------------------------------------------------------------
// Craters of seven sizes (1 km cells down to 25 cm) in body-local metres. Everything is periodic over 4096 m
// so the CPU can hand over the anchor position modulo 4096 and the pattern stays fixed to the ground
// at centimetre precision, however far the camera is from the body's centre.
uvec3 pcg3d(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
vec3 hash3(vec3 cell, float salt) {
    return vec3(pcg3d(uvec3(ivec3(cell) + ivec3(int(salt) * 7919, int(salt) * 104729, int(salt) * 1299709)))) / 4294967295.0;
}

// One crater layer. Craters are circles on the local tangent plane with a smooth Gaussian bowl and rim (no
// hard edges); most are old and subdued, a few are fresh. Adds the layer's slope (body-local, metres per
// metre), multiplies the shadow the up-sun rim casts into each bowl into `shade`, returns an albedo factor.
float craterLayer(vec3 q, vec3 n, vec3 sunH, float tanE, float cell, float coverage, float salt, float layerWeight,
                  float boulders, float footprintM, inout vec3 slope, inout float shade) {
    float minRc = footprintM * 4.0 / cell; // a crater must span ~8 pixels, or it only adds speckle
    float period = 4096.0 / cell;
    vec3 cq = q / cell;
    vec3 base = floor(cq);
    vec3 lo = base + step(0.5, cq - base) - 1.0; // the 2x2x2 block of cells nearest the point
    float albedo = 1.0;
    for (int i = 0; i < 8; ++i) {
        vec3 id = lo + vec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        vec3 wrapped = mod(id, period);
        vec3 h = hash3(wrapped, salt);
        if (h.z > coverage) continue;
        vec3 h2 = hash3(wrapped, salt + 17.0);
        vec3 d3 = cq - (id + 0.25 + 0.5 * h2);
        float off = dot(d3, n);
        if (abs(off) > 0.35) continue; // centre too far off this surface: it belongs to another slice
        vec3 d = d3 - n * off;          // circular on the tangent plane
        float r = length(d);
        float rc = 0.07 + 0.23 * h.x * h.x; // radius in cells (reach 2 rc stays inside the 2x2x2 block)
        float x = r / rc;
        if (x > 2.0) continue;
        float weight = layerWeight * smoothstep(minRc, minRc * 2.0, rc);
        if (weight <= 0.001) continue;
        vec3 dir = d / max(r, 1e-5);
        if (h2.x < boulders) {
            // A rock: a small Gaussian dome, a touch brighter than the soil.
            float g = exp(-x * x / 0.35);
            slope += (-2.0 * x / 0.35 * g * 0.45) * dir * weight;
            albedo *= mix(1.0, 1.0 + 0.08 * g, weight);
            continue;
        }
        float fresh = h.y * h.y * h.y;
        float depth = 0.06 + 0.32 * fresh;           // bowl depth / radius
        float rimA = depth * mix(0.25, 0.55, fresh);  // rim height / radius
        const float sb = 0.62;                        // bowl width
        float sr = mix(0.42, 0.24, fresh);            // rim width
        float gb = exp(-x * x / (sb * sb));
        float gr = exp(-(x - 1.0) * (x - 1.0) / (sr * sr));
        // h(x) / rc = -depth * gb + rimA * gr  ->  slope = dh/dr = d(h/rc)/dx
        float dhdx = depth * gb * 2.0 * x / (sb * sb) - rimA * gr * 2.0 * (x - 1.0) / (sr * sr);
        slope += dhdx * dir * weight;
        // Fresh ejecta is brighter; old craters have weathered to the surrounding tone.
        albedo *= mix(1.0, 1.0 + 0.2 * fresh * exp(-(x - 1.1) * (x - 1.1) * 3.0), weight);
        if (x < 1.0 && tanE > 0.0) {
            // Shadow from the rim toward the Sun: follow the sun-ward horizontal ray to the rim circle.
            float ds = dot(d, sunH);
            float ell = -ds + sqrt(max(ds * ds - (r * r - rc * rc), 0.0));
            float hx = -depth * gb + rimA * gr;
            float hRim = -depth * exp(-1.0 / (sb * sb)) + rimA;
            float clearance = ell * tanE - (hRim - hx) * rc;
            shade *= mix(1.0, smoothstep(-0.04 * rc, 0.04 * rc, clearance), weight);
        }
    }
    return albedo;
}

// Soft isotropic blotches for regolith tone: a Gaussian-weighted blend of random tones at jittered points
// (3x3x3 neighbourhood, so the blend has no seams), periodic over 4096 m. No lattice streaks, no facets.
float blotches(vec3 q, float cell, float salt) {
    float period = 4096.0 / cell;
    vec3 cq = q / cell;
    vec3 base = floor(cq);
    float sum = 0.0, wsum = 1e-6;
    for (int z = -1; z <= 1; ++z)
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) {
                vec3 id = base + vec3(x, y, z);
                vec3 wrapped = mod(id, period);
                vec3 h = hash3(wrapped, salt);
                vec3 dv = cq - (id + h);
                float w = exp(-dot(dv, dv) * 6.0);
                sum += w * hash3(wrapped, salt + 3.0).x;
                wsum += w;
            }
    return sum / wsum;
}

vec3 surfaceDetail(Body b, int bodyIndex, vec3 hitWorld, vec3 nLocal, vec3 p, vec3 Ll, float footprintM,
                   float patchW, out float albedoMod, out float shade) {
    albedoMod = 1.0;
    shade = 1.0;
    float kind = b.reliefParams.w;
    if (kind < 0.5 || int(pc.anchorWorld.w) != bodyIndex) return nLocal;
    vec3 offsetM = rotateInv(b.rotation, hitWorld - pc.anchorWorld.xyz) * pc.shadowInfo.w;
    float reach = 1.0 - smoothstep(90000.0, 150000.0, length(offsetM));
    if (reach <= 0.0) return nLocal;
    vec3 q = mod(pc.anchorLocal.xyz + offsetM, 4096.0);
    bool mars = kind > 1.5;
    // Per layer (1 km .. 25 cm cells): small craters are far more common, and the smallest scales are
    // strewn with rocks (more on Mars, whose plains are rockier and less cratered).
    float coverageL[7] = mars ? float[](0.10, 0.12, 0.14, 0.18, 0.25, 0.35, 0.45)
                              : float[](0.35, 0.40, 0.45, 0.55, 0.62, 0.70, 0.75);
    float bouldersL[7] = mars ? float[](0.0, 0.0, 0.2, 0.5, 0.7, 0.8, 0.85)
                              : float[](0.0, 0.0, 0.02, 0.08, 0.22, 0.35, 0.45);
    float sinE = dot(Ll, p);
    vec3 horiz = Ll - p * sinE;
    float hl = max(length(horiz), 1e-4);
    vec3 sunH = horiz / hl;
    float tanE = sinE / hl;
    vec3 slope = vec3(0.0);
    float cells[7] = float[](1024.0, 256.0, 64.0, 16.0, 4.0, 1.0, 0.25);
    for (int l = 0; l < 7; ++l) {
        float cell = cells[l];
        float w = smoothstep(cell * 0.2, cell * 0.05, footprintM) * reach; // craters at least ~10 px wide
        // Real terrain data replaces invented craters at the scales it resolves.
        // (the orthophoto resolves ~1 m, so inside a patch only the finest layer stays procedural)
        if (patchW > 0.0 && cell >= 1.0) w *= 1.0 - patchW;
        if (w <= 0.001) continue;
        albedoMod *= craterLayer(q, p, sunH, tanE, cell, coverageL[l], float(l + 1), w, bouldersL[l], footprintM, slope, shade);
    }
    // Regolith tone: overlapping soft blotches at a few scales.
    float mottle = blotches(q, 64.0, 29.0) * 0.45 + blotches(q, 16.0, 31.0) * 0.35 + blotches(q, 4.0, 37.0) * 0.2;
    albedoMod *= mix(1.0, 0.72 + 0.56 * mottle, reach * smoothstep(40.0, 2.0, footprintM) * (1.0 - patchW));
    slope *= mars ? 0.7 : 1.0;
    vec3 tangentSlope = slope - nLocal * dot(slope, nLocal);
    return normalize(nLocal - tangentSlope);
}

// Terrain self-shadowing: march the height map toward the Sun (with the globe's curvature) and keep
// the lowest clearance, softened by the width of the solar disc.
float terrainShadow(Body b, vec3 p, vec3 Ll, vec2 uv, vec3 east, vec3 north) {
    float sinE = dot(p, Ll);
    if (sinE < -0.25) return 0.0;
    vec3 horiz = Ll - p * sinE;
    float hl = length(horiz);
    if (hl < 1e-4) return 1.0;
    horiz /= hl;
    float tanE = sinE / hl;
    float ae = dot(horiz, east), an = dot(horiz, north);
    float Rm = b.posRadius.w * pc.shadowInfo.w;
    float cosLat = max(length(p.xz), 0.02);
    float lo = b.reliefParams.x * 1000.0, range = b.reliefParams.y * 1000.0;
    int tex = b.relief.y;
    float texelM = 2.0 * PI * Rm / float(textureSize(uTex[nonuniformEXT(tex)], 0).x);
    float h0 = textureLod(uTex[nonuniformEXT(tex)], uv, 0.0).r * range + lo;
    float vis = 1.0;
    float d = texelM * 1.5;
    for (int i = 0; i < 22 && d < 180000.0; ++i) {
        vec2 suv = uv + vec2(d * ae / (2.0 * PI * Rm * cosLat), -d * an / (PI * Rm));
        float lod = max(log2(d / (texelM * 5.0)), 0.0);
        float hs = textureLod(uTex[nonuniformEXT(tex)], suv, lod).r * range + lo;
        float rayH = h0 + d * tanE + d * d / (2.0 * Rm);
        float penumbra = d * 0.0093 + texelM * 0.35;
        vis = min(vis, clamp((rayH - hs) / penumbra + 0.5, 0.0, 1.0));
        d *= 1.33;
    }
    return vis;
}

// Shadows cast by the local terrain patch's heights (metre-scale rims, boulders, the real crater walls).
float patchShadow(Body b, vec2 puv, vec3 p, vec3 Ll) {
    float sinE = dot(p, Ll);
    if (sinE < -0.25) return 0.0;
    vec3 horiz = Ll - p * sinE;
    float hl = length(horiz);
    if (hl < 1e-4) return 1.0;
    horiz /= hl;
    float tanE = sinE / hl;
    float ae = dot(horiz, pc.patchEast.xyz), an = dot(horiz, pc.patchNorth.xyz);
    int tex = b.patchTex.z;
    float texelM = b.patchParams.z;
    float h0 = textureLod(uTex[nonuniformEXT(tex)], puv, 0.0).r * b.patchParams.y + b.patchParams.x;
    float vis = 1.0;
    float d = texelM * 1.5;
    for (int i = 0; i < 32 && d < 4000.0; ++i) {
        vec2 suv = puv + vec2(d * ae * pc.patchAnchor.z, d * an * pc.patchAnchor.w);
        if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) break;
        float lod = max(log2(d / (texelM * 6.0)), 0.0);
        float hs = textureLod(uTex[nonuniformEXT(tex)], suv, lod).r * b.patchParams.y + b.patchParams.x;
        float rayH = h0 + d * tanE;
        float penumbra = d * 0.0093 + texelM * 0.4;
        vis = min(vis, clamp((rayH - hs) / penumbra + 0.5, 0.0, 1.0));
        d *= 1.22;
    }
    return vis;
}

// Spacecraft shadows (landers, flags) from the sun-aligned depth map, 5x5 PCF.
float craftShadow(vec3 hitWorld, vec3 Nw) {
    int idx = int(pc.shadowInfo.x);
    if (idx < 0) return 1.0;
    float invScale = length(vec3(pc.shadowMat[0][0], pc.shadowMat[1][0], pc.shadowMat[2][0])); // 1 / half extent
    float texelWorld = 2.0 * pc.shadowInfo.y / max(invScale, 1e-30);
    vec4 sc = pc.shadowMat * vec4(hitWorld + Nw * texelWorld * 1.5, 1.0);
    vec2 suv = sc.xy * 0.5 + 0.5;
    if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0))) || sc.z <= 0.0 || sc.z >= 1.0) return 1.0;
    float lit = 0.0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x) {
            float stored = textureLod(uTex[nonuniformEXT(idx)], suv + vec2(x, y) * pc.shadowInfo.y * 1.5, 0.0).r;
            lit += (sc.z + pc.shadowInfo.z >= stored) ? 1.0 : 0.0;
        }
    return lit / 25.0;
}

void main() {
    Body b = bodies[vBody];
    int type = int(b.params.x + 0.5);
    float seed = b.params.y;
    float time = pc.params.x;
    bool hasDay = b.tex.x >= 0;

    if (type == RING) {
        // This shader writes gl_FragDepth for the ray-traced spheres, so every path must write it: without
        // this the rings depth-test against an undefined value and break up into blocks over the globe.
        gl_FragDepth = gl_FragCoord.z;
        vec3 N = normalize(vNormal);
        vec3 toSunR = pc.sunPos.xyz - vWorldPos;
        vec3 L = normalize(toSunR);
        vec3 V = normalize(-vWorldPos);
        // Ring map: radial strip, x = inner..outer.
        vec4 ring = hasDay ? texture(uTex[nonuniformEXT(b.tex.x)], vec2(vRadial, 0.5)) : vec4(b.color.rgb, 0.0);
        float density;
        if (hasDay) density = ring.a;
        else {
            float bandsA = fbm(vec3(vRadial * 40.0, seed, 0.0), 4);
            float bandsB = noise3(vec3(vRadial * 160.0, seed * 3.0, 1.0));
            density = smoothstep(0.25, 0.7, bandsA) * (0.55 + 0.45 * bandsB);
            density *= 1.0 - 0.8 * smoothstep(0.58, 0.60, vRadial) * smoothstep(0.66, 0.64, vRadial);
        }
        density *= smoothstep(0.0, 0.02, vRadial) * smoothstep(1.0, 0.98, vRadial);
        float ndl = abs(dot(N, L));
        // Planet shadow: from this ring point toward the Sun, does the ray hit the globe?
        vec3 pl = vec3(vLocal.x, 0.0, vLocal.z) * mix(b.params.z, 1.0, vRadial); // ring-local, outer radius = 1
        vec3 Ll = normalize(rotateInv(b.rotation, L));
        float planetR = b.color.w;
        float tca = dot(-pl, Ll);
        float dperp = length(pl + Ll * max(tca, 0.0));
        float shadow = tca > 0.0 ? smoothstep(planetR * 0.985, planetR * 1.015, dperp) : 1.0;
        // Lit face vs. unlit face: light that reaches the far side is transmitted through the particles.
        bool sameSide = sign(dot(N, L)) == sign(dot(N, V));
        float faceLight = sameSide ? 1.0 : 0.38;
        vec3 lit = ring.rgb * (0.06 + 1.1 * ndl) * shadow * faceLight;
        float alpha = clamp(density, 0.0, 1.0) * 0.95;
        outColor = vec4(lit * alpha, 1.0 - alpha); // premultiplied over
        return;
    }

    // --- Exact sphere ---------------------------------------------------------------------------
    // vWorldPos lies on the proxy along this pixel's view ray, so its direction is the ray.
    vec3 rd = normalize(vWorldPos);
    dvec3 C = dvec3(b.posRadius.xyz);
    double R = double(b.posRadius.w);
    dvec3 D = dvec3(rd);
    double bq = dot(C, D);
    double cq = dot(C, C) - R * R;
    double disc = bq * bq - cq;
    bool hitOk = disc >= 0.0 && bq > 0.0;
    double t = hitOk ? cq / (bq + sqrt(disc)) : max(bq, 0.0); // near root, cancellation-free
    if (cq <= 0.0) {                                        // camera below the surface: shade the far wall
        t = bq + sqrt(max(disc, 0.0));
        hitOk = true;
    }
    dvec3 surfaceCentre = C;
    if (int(pc.anchorWorld.w) == vBody) {
        // Close to this body: intersect relative to the surface anchor under the camera, which the CPU
        // placed on the sphere in double precision. The body centre as a float is only good to ~10 cm at
        // lunar radius, and that rounding changes every frame, making the ground bob under a lander.
        // With the anchor A and its normal n, |tD - A + nR|^2 = R^2 has only small, exact coefficients.
        dvec3 A = dvec3(pc.anchorWorld.xyz);
        dvec3 n = dvec3(normalize(rotateVec(b.rotation, cross(pc.patchEast.xyz, pc.patchNorth.xyz))));
        double Bq = dot(D, A) - R * dot(D, n);
        double Cq = dot(A, A) - 2.0 * R * dot(A, n);
        double disc2 = Bq * Bq - Cq;
        hitOk = disc2 >= 0.0 && Bq > 0.0;
        t = hitOk ? Cq / (Bq + sqrt(disc2)) : max(Bq, 0.0);
        if (Cq <= 0.0) {
            t = Bq + sqrt(max(disc2, 0.0));
            hitOk = true;
        }
        surfaceCentre = A - n * R;
    }
    dvec3 Pd = D * t;
    vec3 hit = vec3(Pd);
    vec3 N = vec3(normalize(Pd - surfaceCentre));
    vec3 p = normalize(rotateInv(b.rotation, N));
    vec2 uv = sphereUv(p);
    prepareSeam(uv); // derivatives: before any discard or divergent branch
    float footprintM = max(length(dFdx(hit)), length(dFdy(hit))) * pc.shadowInfo.w;
    // Local terrain patch coordinates, relative to the double-precision anchor (derivatives taken here too).
    vec3 patchOffM = rotateInv(b.rotation, hit - pc.anchorWorld.xyz) * pc.shadowInfo.w;
    vec2 puv = pc.patchAnchor.xy + vec2(dot(patchOffM, pc.patchEast.xyz) * pc.patchAnchor.z,
                                        dot(patchOffM, pc.patchNorth.xyz) * pc.patchAnchor.w);
    vec2 pdx = dFdx(puv), pdy = dFdy(puv);
    float patchW = 0.0;
    if (b.patchParams.w > 0.5 && int(pc.anchorWorld.w) == vBody) {
        vec2 edge = min(puv, 1.0 - puv);
        patchW = smoothstep(0.0, 0.08, min(edge.x, edge.y));
    }
    if (!hitOk) discard;
    {
        vec3 row2 = vec3(pc.viewProj[0][2], pc.viewProj[1][2], pc.viewProj[2][2]);
        vec3 row3 = vec3(pc.viewProj[0][3], pc.viewProj[1][3], pc.viewProj[2][3]);
        float w = max(dot(row3, hit), 1e-37);
        gl_FragDepth = clamp((dot(row2, hit) + pc.viewProj[3][2]) / w, 0.0, 1.0);
    }

    vec3 V = -rd;
    vec3 toSun = pc.sunPos.xyz - hit;
    float sunDist2 = max(dot(toSun, toSun), 1e-30);
    vec3 L = toSun * inversesqrt(sunDist2);

    if (type == SUN) {
        // Limb darkening + granulation; radiance carried by pc.params.y so bloom gets real energy.
        float mu = max(dot(N, V), 0.0);
        float limb = 0.45 + 0.55 * pow(mu, 0.6);
        vec3 surf = hasDay ? sampleDay(b, uv).rgb * (0.8 + 0.4 * fbm(p * 40.0 + time * 0.03, 3))
                           : sunSurface(p, seed, time);
        outColor = vec4(b.color.rgb * surf * limb * pc.params.y, 1.0);
        return;
    }

    // --- Albedo ---------------------------------------------------------------------------------
    vec3 albedo;
    float ocean = 0.0;
    if (hasDay) {
        albedo = sampleDay(b, uv).rgb;
        if (type == EARTH) {
            // Ocean mask from the map itself: water is the only strongly blue-dominant surface.
            ocean = smoothstep(0.02, 0.12, albedo.b - max(albedo.r, albedo.g) * 0.9);
        }
    } else if (type == EARTH) albedo = earthSurface(p, seed, ocean);
    else if (type == GAS)     albedo = gasSurface(p, b.color.rgb, seed, false);
    else if (type == ICE)     albedo = gasSurface(p, b.color.rgb, seed, true);
    else                      albedo = rockySurface(p, b.color.rgb, seed);
    if (patchW > 0.0 && b.patchTex.x >= 0)
        albedo = mix(albedo, textureGrad(uTex[nonuniformEXT(b.patchTex.x)], puv, pdx, pdy).rgb, patchW);

    // --- Relief ---------------------------------------------------------------------------------
    vec3 Ll = normalize(rotateInv(b.rotation, L));
    float cosLat = length(p.xz);
    vec3 east = cosLat > 1e-5 ? vec3(p.z, 0.0, -p.x) / cosLat : vec3(0.0, 0.0, -1.0);
    vec3 north = cross(p, east);
    vec3 nLocal = p;
    if (b.relief.x >= 0) {
        vec2 ne = (sampleMap(b.relief.x, uv).rg * 2.0 - 1.0) * b.reliefParams.z;
        float nz = sqrt(max(1.0 - dot(ne, ne), 0.05));
        nLocal = normalize(east * ne.x + north * ne.y + p * nz);
    }
    if (patchW > 0.0 && b.patchTex.y >= 0) {
        vec2 ne = textureGrad(uTex[nonuniformEXT(b.patchTex.y)], puv, pdx, pdy).rg * 2.0 - 1.0;
        float nz = sqrt(max(1.0 - dot(ne, ne), 0.05));
        vec3 nPatch = normalize(east * ne.x + north * ne.y + p * nz);
        nLocal = normalize(mix(nLocal, nPatch, patchW));
    }
    float albedoMod = 1.0, detailShade = 1.0;
    nLocal = surfaceDetail(b, vBody, hit, nLocal, p, Ll, footprintM, patchW, albedoMod, detailShade);
    albedo *= albedoMod;
    vec3 Ns = rotateVec(b.rotation, nLocal);
    float terrain = b.relief.y >= 0 ? terrainShadow(b, p, Ll, uv, east, north) : 1.0;
    if (patchW > 0.0 && b.patchTex.z >= 0) terrain = min(terrain, mix(1.0, patchShadow(b, puv, p, Ll), patchW));
    float crafts = craftShadow(hit, N);

    // --- Lighting -------------------------------------------------------------------------------
    // Inverse-square from the Sun normalised to 1 at Earth, compressed so the outer planets stay visible.
    const float AU = 4.848e-6;
    float irradiance = pow((AU * AU) / sunDist2, 0.3);
    // Eclipses and transits: any other body between this point and the Sun covers part of the solar disc
    // (the Moon's shadow on Earth, Io's on Jupiter, Earth's on the Moon).
    {
        float sunAng = asin(clamp(bodies[0].posRadius.w * inversesqrt(sunDist2), 0.0, 1.0));
        int count = int(pc.anchorLocal.w);
        for (int j = 1; j < count; ++j) {
            if (j == vBody) continue;
            vec3 rel = bodies[j].posRadius.xyz - hit;
            float dist2 = dot(rel, rel);
            if (dist2 >= sunDist2 || dist2 <= 0.0) continue;
            float dist = sqrt(dist2);
            float along = dot(rel, L);
            if (along <= 0.0) continue;
            float bodyAng = asin(clamp(bodies[j].posRadius.w / dist, 0.0, 1.0));
            float sep = acos(clamp(along / dist, -1.0, 1.0));
            if (sep > bodyAng + sunAng) continue;
            float covered = clamp((bodyAng + sunAng - sep) / (2.0 * sunAng), 0.0, 1.0);
            float maxCover = min(1.0, (bodyAng * bodyAng) / max(sunAng * sunAng, 1e-12));
            irradiance *= 1.0 - covered * maxCover;
        }
    }
    float diffuse;
    if (type == ROCKY && b.params.z <= 0.0) {
        // Airless regolith (Lunar-Lambert): mostly Lommel-Seeliger, so the full Moon stays bright to the
        // limb instead of shading like a plastic ball, with enough Lambert for relief to read at low sun.
        float mu0 = max(dot(Ns, L), 0.0);
        float mu = max(dot(N, V), 0.02);
        diffuse = mix(mu0, 2.0 * mu0 / (mu0 + mu + 1e-4), 0.55);
        diffuse *= smoothstep(-0.02, 0.01, dot(N, L) + 0.02); // no light past the geometric terminator
    } else {
        float wrap = b.params.z * 0.15;
        diffuse = clamp((dot(Ns, L) + wrap) / (1.0 + wrap), 0.0, 1.0);
    }
    diffuse *= terrain * crafts * detailShade;

    // Ring shadow: trace from the surface toward the Sun to the ring plane and read the ring's density.
    if (b.color.w > 0.0 && b.tex.w >= 0) {
        if (abs(Ll.y) > 1e-4) {
            float tPlane = -p.y / Ll.y;
            if (tPlane > 0.0) {
                vec3 hitP = p + Ll * tPlane;
                float r = length(hitP.xz) / b.color.w;          // 0..1 of ring outer radius
                float u = (r - b.params.w) / max(1.0 - b.params.w, 1e-4);
                if (u > 0.0 && u < 1.0) {
                    float ringA = texture(uTex[nonuniformEXT(b.tex.w)], vec2(u, 0.5)).a;
                    diffuse *= 1.0 - 0.92 * ringA;
                }
            }
        }
    }
    vec3 color = albedo * diffuse * irradiance;
    if (type == ROCKY && b.params.z <= 0.0) {
        // Shadows on airless ground are dark, not black: sunlit regolith all around bounces a little in.
        float daylit = smoothstep(-0.02, 0.05, dot(N, L));
        color += albedo * irradiance * 0.025 * daylit * (1.0 - 0.5 * diffuse);
    }

    if (type == EARTH) {
        vec3 H = normalize(L + V);
        float spec = pow(max(dot(N, H), 0.0), 60.0) * ocean * 0.12;
        // Clouds drift east at ~80 m/s (real weather, not a time-lapse).
        float clouds = b.tex.z >= 0 ? sampleMap(b.tex.z, uv + vec2(time * 2e-6, 0.0)).r
                                    : smoothstep(0.55, 0.75, fbm(p * 5.0 + vec3(time * 0.004, 0.0, 0.0) + seed * 5.0, 5));
        if (b.tiles.w >= 0) {
            // Today's real cloud cover where the satellite passes have data; the static map elsewhere.
            vec2 live = sampleMap(b.tiles.w, uv).rg;
            clouds = mix(clouds, live.r, live.g);
        }
        // Below the cloud map's ~5 km pixels, break the edges up with fine cellular structure (only when the
        // camera is close enough for a map pixel to span several screen pixels).
        float cloudDetail = 1.0 - smoothstep(600.0, 2500.0, footprintM);
        if (cloudDetail > 0.0 && clouds > 0.01) {
            vec3 pd = p + vec3(time * 2e-6 * 6.2831853, 0.0, 0.0);
            float fine = fbm(pd * 1500.0, 4) * 0.6 + fbm(pd * 6000.0, 3) * 0.4;
            // Mottle rather than shred: thin cloud stays thin, edges get structure.
            float shaped = clouds * clamp(0.45 + 1.1 * fine, 0.0, 1.4) * smoothstep(0.02, 0.25, clouds + (fine - 0.5) * 0.3);
            clouds = clamp(mix(clouds, shaped, cloudDetail), 0.0, 1.0);
        }
        color = mix(color, vec3(1.0) * diffuse * irradiance, clouds * 0.9) + spec * irradiance;
        float night = smoothstep(0.05, -0.15, dot(N, L));
        vec3 lights = b.tex.y >= 0 ? sampleMap(b.tex.y, uv).rgb * vec3(1.0, 0.9, 0.75)
                                   : vec3(1.0, 0.75, 0.45) * smoothstep(0.62, 0.8, fbm(p * 14.0 + seed, 4)) * (1.0 - ocean);
        color += lights * night * (1.0 - clouds * 0.8) * 0.05;
    }

    // Atmosphere rim: fresnel-weighted, coloured by the body, brighter on the day side.
    float fres = pow(1.0 - max(dot(N, V), 0.0), 3.5);
    vec3 atmoCol = type == EARTH ? vec3(0.35, 0.55, 1.0) : b.color.rgb * 0.8 + 0.2;
    float dayside = clamp(dot(N, L) * 0.5 + 0.5, 0.0, 1.0);
    color += atmoCol * fres * b.params.z * irradiance * (0.15 + 0.85 * dayside) * 0.15;

    outColor = vec4(color, 1.0);
}
