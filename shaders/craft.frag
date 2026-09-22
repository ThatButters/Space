#version 460
#extension GL_EXT_nonuniform_qualifier : enable
// Spacecraft shading: glTF metallic-roughness PBR (GGX specular, Smith visibility, Schlick Fresnel)
// lit by the Sun, with normal / metallic-roughness / occlusion / emissive maps when a model has them,
// shadows from the sun-aligned depth map, bounce light from the ground under landers, and a faint
// sky fill so the night side of a spacecraft is not pure black.

struct Craft {
    mat4 model;
    vec4 sunDir;
    vec4 tint;
    vec4 up;
    vec4 extra; // x size, y sunlit fraction, z metres per model unit, w glint allowed
    vec4 crop;      // x model-space Y cutoff
};
layout(std430, set = 0, binding = 0) readonly buffer Crafts { Craft crafts[]; };
layout(set = 1, binding = 0) uniform sampler2D uTex[1024];

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 baseColor;
    ivec4 ids;
    vec4 material;
    mat4 shadowMat;
    vec4 shadowInfo;
    vec4 uvTransform;
    vec4 emissive;
} pc;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec3 vModelPos;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

uvec3 pcg3dC(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
vec3 hashC(vec3 cell) { return vec3(pcg3dC(uvec3(ivec3(cell) + 32768))) / 4294967295.0; }

// Crinkled multi-layer insulation: the foil is a patchwork of flat facets at random tilts with crisp
// creases between them. Nearest jittered point per cell picks the facet (two scales, in metres).
vec3 foilFacets(vec3 pm, out float roughJitter) {
    vec3 tilt = vec3(0.0);
    roughJitter = 0.0;
    float scales[2] = float[](0.09, 0.028);
    for (int s = 0; s < 2; ++s) {
        vec3 cq = pm / scales[s];
        vec3 base = floor(cq);
        vec3 lo = base + step(0.5, cq - base) - 1.0;
        float best = 1e9;
        vec3 pick = vec3(0.5);
        for (int i = 0; i < 8; ++i) {
            vec3 id = lo + vec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
            vec3 h = hashC(id + float(s) * 1000.0);
            float dist = length(cq - (id + 0.25 + 0.5 * h));
            if (dist < best) {
                best = dist;
                pick = hashC(id * 3.0 + 17.0 + float(s) * 555.0);
            }
        }
        tilt += (pick - 0.5) * (s == 0 ? 0.55 : 0.3);
        roughJitter += (pick.x - 0.5) * (s == 0 ? 0.25 : 0.15);
    }
    return tilt;
}

float craftShadow(vec3 worldPos, vec3 N) {
    int idx = int(pc.shadowInfo.x);
    if (idx < 0) return 1.0;
    float invScale = length(vec3(pc.shadowMat[0][0], pc.shadowMat[1][0], pc.shadowMat[2][0]));
    float texelWorld = 2.0 * pc.shadowInfo.y / max(invScale, 1e-30);
    vec4 sc = pc.shadowMat * vec4(worldPos + N * texelWorld * 1.2, 1.0);
    vec2 suv = sc.xy * 0.5 + 0.5;
    if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0))) || sc.z <= 0.0 || sc.z >= 1.0) return 1.0;
    float lit = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            float stored = textureLod(uTex[nonuniformEXT(idx)], suv + vec2(x, y) * pc.shadowInfo.y, 0.0).r;
            lit += (sc.z + pc.shadowInfo.z >= stored) ? 1.0 : 0.0;
        }
    return lit / 9.0;
}

// Tangent frame from screen-space derivatives (no vertex tangents needed).
mat3 cotangentFrame(vec3 N, vec3 p, vec2 uv) {
    vec3 dp1 = dFdx(p), dp2 = dFdy(p);
    vec2 duv1 = dFdx(uv), duv2 = dFdy(uv);
    vec3 dp2perp = cross(dp2, N), dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float m = max(dot(T, T), dot(B, B));
    if (m < 1e-30) return mat3(vec3(0.0), vec3(0.0), N); // degenerate UVs: no tangent frame, keep N
    float invmax = inversesqrt(m);
    return mat3(T * invmax, B * invmax, N);
}

void main() {
    Craft c = crafts[pc.ids.x];
    if (vModelPos.y > c.crop.x) discard;
    vec4 base = pc.baseColor;
    if (pc.ids.y >= 0) base *= texture(uTex[nonuniformEXT(pc.ids.y)], vUv);
    if (base.a < 0.35) discard; // cut-out materials (array blankets, grilles)
    vec3 albedo = base.rgb * c.tint.rgb;

    vec3 V = normalize(-vWorldPos);
    vec3 Ng = normalize(vNormal);
    if (dot(Ng, V) < 0.0) Ng = -Ng; // NASA meshes mix windings and are drawn double-sided
    vec3 N = Ng;
    if (pc.ids.z >= 0) {
        // Positions scaled up to metres so the derivative frame keeps its precision.
        vec3 tn = texture(uTex[nonuniformEXT(pc.ids.z)], vUv).xyz * 2.0 - 1.0;
        tn.xy *= pc.material.z;
        mat3 TBN = cotangentFrame(Ng, vWorldPos * pc.shadowInfo.w, vUv);
        vec3 mapped = TBN * tn;
        if (dot(mapped, mapped) > 1e-12) N = normalize(mapped);
    }

    float metallic = pc.material.x, rough = pc.material.y;
    // Material recognition for models with flat colours: warm saturated gold/amber is Kapton or aluminised
    // foil blanketing, bright neutral grey is bare or anodised metal. Both get the texture of the real thing.
    float warm = (albedo.r - albedo.b) / max(albedo.r, 1e-3);
    float foil = smoothstep(0.35, 0.6, warm) * smoothstep(0.08, 0.2, albedo.r);
    float lumA = dot(albedo, vec3(0.2126, 0.7152, 0.0722));
    float silver = (1.0 - foil) * smoothstep(0.35, 0.6, lumA) * (1.0 - smoothstep(0.08, 0.2, abs(albedo.r - albedo.b)));
    if (foil > 0.0 || silver > 0.0) {
        vec3 pm = vModelPos * c.extra.z; // metres in the model's own frame
        float rj;
        vec3 tilt = foilFacets(pm, rj);
        vec3 bent = normalize(N + (tilt - N * dot(tilt, N)) * (foil + 0.25 * silver));
        if (dot(bent, V) > 0.0) N = bent;
        metallic = mix(metallic, 1.0, max(foil, silver * 0.8));
        rough = mix(rough, clamp(0.22 + rj, 0.1, 0.5), foil);
        rough = mix(rough, clamp(0.3 + rj * 0.5, 0.15, 0.5), silver);
    }
    if (pc.ids.w >= 0) {
        vec4 mr = texture(uTex[nonuniformEXT(pc.ids.w)], vUv);
        rough *= mr.g;
        metallic *= mr.b;
    }
    rough = clamp(rough, 0.09, 1.0); // no perfect mirrors: a sun glint off one would flood the bloom
    float ao = 1.0;
    int aoIdx = int(pc.material.w);
    if (aoIdx >= 0) ao = texture(uTex[nonuniformEXT(aoIdx)], vUv).r;

    vec3 L = normalize(c.sunDir.xyz);
    float irr = c.sunDir.w;
    vec3 H = normalize(L + V);
    float ndl = max(dot(N, L), 0.0);
    float ndv = max(dot(N, V), 1e-3);
    float ndh = max(dot(N, H), 0.0);
    float vdh = max(dot(V, H), 0.0);

    float a = rough * rough;
    float a2 = a * a;
    float dd = ndh * ndh * (a2 - 1.0) + 1.0;
    float Dggx = a2 / (PI * dd * dd);
    float k = (rough + 1.0) * (rough + 1.0) / 8.0;
    float Vis = 1.0 / ((ndl * (1.0 - k) + k) * (ndv * (1.0 - k) + k) * 4.0);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - vdh, 5.0);
    vec3 spec = min(Dggx * Vis, 24.0) * F; // bright glints, but bounded
    vec3 kd = (1.0 - F) * (1.0 - metallic);

    // The Sun's direct light, scaled to the planets' convention (albedo * cos at irradiance 1).
    float shadow = craftShadow(vWorldPos, Ng);
    vec3 color = (kd * albedo + spec * PI) * ndl * irr * shadow;

    // Ground bounce under landers: the sunlit regolith lights surfaces that face down.
    if (c.up.w > 0.5 && c.tint.w > 0.0) {
        vec3 up = normalize(c.up.xyz);
        float groundLit = max(dot(up, L), 0.0);
        float facingDown = clamp(0.5 - 0.5 * dot(N, up), 0.0, 1.0);
        color += albedo * (1.0 - metallic * 0.6) * irr * c.tint.w * groundLit * facingDown * ao;
        // Rough metal and foil also pick up the ground in their reflections.
        color += F0 * irr * c.tint.w * groundLit * facingDown * (1.0 - rough) * 0.5 * ao;
    }
    // Reflections of the planet or ground below (metal and foil are mostly what they reflect).
    if (c.up.w > 0.5 && c.tint.w > 0.0) {
        vec3 up = normalize(c.up.xyz);
        vec3 R = reflect(-V, N);
        float toGround = smoothstep(0.1, -0.25, dot(R, up));
        float groundLit = max(dot(up, L), 0.0);
        color += F0 * (irr * c.tint.w * groundLit * 2.2) * toGround * mix(1.0, 0.55, rough) * ao;
    }
    // Faint fill (starlight, planet-shine) so unlit sides keep a little shape.
    color += albedo * 0.012 * irr * ao * (0.6 + 0.4 * max(dot(N, V), 0.0));

    vec3 emission = pc.emissive.rgb;
    if (pc.emissive.w >= 0.0) emission *= texture(uTex[nonuniformEXT(int(pc.emissive.w))], vUv).rgb;
    color += emission;
    outColor = vec4(min(color, vec3(60.0)), 1.0);
}
