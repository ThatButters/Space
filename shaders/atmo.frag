#version 460
#include "common.glsl"
#include "frame.glsl"
FRAME_DATA_BLOCK(1)
// Atmosphere shell: single-scattering Rayleigh (plus a little Mie forward scatter) integrated along
// the view ray through a thin shell around the body. Drawn on a sphere slightly larger than the
// planet, premultiplied over: rgb = in-scattered sunlight, a = transmittance applied to what is behind.

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
};
layout(std430, set = 0, binding = 0) readonly buffer Bodies { Body bodies[]; };

layout(push_constant) uniform PushConstants {
    mat4 viewProj;
    vec4 sunPos;
    vec4 params; // x time, y sun radiance, z shell scale, w aurora strength
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

const int VIEW_STEPS = 12;
const int SUN_STEPS = 4;

// Returns (near, far) intersections of a ray with a sphere at the origin, or far < near if none.
vec2 raySphere(vec3 ro, vec3 rd, float r) {
    float b = dot(ro, rd);
    float c = dot(ro, ro) - r * r;
    float d = b * b - c;
    if (d < 0.0) return vec2(1.0, -1.0);
    float s = sqrt(d);
    return vec2(-b - s, -b + s);
}

void main() {
    Body b = bodies[vBody];
    int type = int(b.params.x + 0.5);
    float strength = b.params.z;

    // Per-body scattering: coefficients per planet radius, scale height as a fraction of the radius.
    // Exaggerated relative to reality (Earth's is 0.13%) so the shell reads at explorer distances.
    vec3 betaR;
    float betaM, H;
    // Scale height 1.2% of the radius (10x Earth's, so the shell reads at explorer distances); the
    // coefficients give a vertical optical depth of ~0.1 in the blue for Earth, ~2 at the grazing limb.
    if (type == 2)      { betaR = vec3(1.4, 3.3, 8.0);  betaM = 0.4; H = 0.012; } // Earth
    else if (type == 3) { betaR = vec3(0.5, 0.7, 1.0);  betaM = 0.3; H = 0.008; } // gas giants: thin, warm haze
    else if (type == 4) { betaR = vec3(0.8, 1.8, 3.2);  betaM = 0.5; H = 0.012; } // ice giants
    else {
        // Dusty air (Venus / Mars / Titan): scattering takes the planet's own tint, so Mars gets its
        // butterscotch daytime sky and blue-tinged sunsets, Titan its orange haze.
        vec3 tint = normalize(b.color.rgb + 0.05) * 1.7;
        betaR = vec3(3.0, 2.4, 1.6) * mix(vec3(1.0), tint, 0.7);
        betaM = 1.0; H = 0.010;
    }
    betaR *= strength;
    betaM *= strength;

    // Geometry in planet-radius units, camera-relative.
    float R = b.posRadius.w;
    vec3 center = b.posRadius.xyz;
    vec3 ro = -center / R;              // camera position relative to the planet centre
    vec3 rd = normalize(vWorldPos); // view ray from the camera through this fragment
    float Ra = pc.params.z;             // shell radius (planet = 1)

    // From outside only the near (front) faces of the shell are drawn; from inside only the far faces, so
    // the sky over a planet's surface is the same integral seen from within.
    bool inside = dot(ro, ro) < Ra * Ra;
    if (inside == gl_FrontFacing) discard;
    vec2 hit = raySphere(ro, rd, Ra);
    if (hit.y <= 0.0) discard;
    float tA = max(hit.x, 0.0), tB = hit.y;
    vec2 ground = raySphere(ro, rd, 1.0);
    bool hitsGround = ground.x > 0.0 && ground.x < tB;
    if (hitsGround) tB = ground.x;

    vec3 sunDir = normalize(pc.sunPos.xyz - center);
    float ds = (tB - tA) / float(VIEW_STEPS);
    vec3 inscatter = vec3(0.0);
    vec3 auroraLight = vec3(0.0);
    float odView = 0.0;
    float mu = dot(rd, sunDir);
    float phaseR = 3.0 / (16.0 * 3.14159265) * (1.0 + mu * mu);
    float g = 0.76;
    float phaseM = 3.0 / (8.0 * 3.14159265) * (1.0 - g * g) * (1.0 + mu * mu) /
                   ((2.0 + g * g) * pow(1.0 + g * g - 2.0 * g * mu, 1.5));

    for (int i = 0; i < VIEW_STEPS; ++i) {
        float t = tA + (float(i) + 0.5) * ds;
        vec3 p = ro + rd * t;
        float h = max(length(p) - 1.0, 0.0);
        float dens = exp(-h / H);
        odView += dens * ds;

        // Optical depth toward the Sun from this sample.
        vec2 sunHit = raySphere(p, sunDir, Ra);
        float sunLen = max(sunHit.y, 0.0);
        float dsSun = sunLen / float(SUN_STEPS);
        float odSun = 0.0;
        bool shadowed = raySphere(p, sunDir, 1.0).y > 0.0 && raySphere(p, sunDir, 1.0).x > 0.0;
        for (int j = 0; j < SUN_STEPS; ++j) {
            vec3 q = p + sunDir * (float(j) + 0.5) * dsSun;
            odSun += exp(-max(length(q) - 1.0, 0.0) / H) * dsSun;
        }
        vec3 transmittance = exp(-(betaR * (odView + odSun) + betaM * (odView + odSun) * 1.1));
        if (shadowed) transmittance *= 0.05; // night side: only a little multiple scattering
        inscatter += transmittance * dens * ds * (betaR * phaseR + betaM * phaseM);

        if (type == 2 && pc.params.w > 0.0) {
            // Aurora is self-emissive: not scaled by sunlight, brighter on the night side.
            vec3 pl = rotateInv(b.rotation, p);
            vec3 sl = normalize(rotateInv(b.rotation, sunDir));
            float night = shadowed ? 1.0 : 0.25;
            auroraLight += exp(-betaR * odView) * aurora(pl, sl, h, pc.params.x, pc.params.w) * ds * night;
            // Airglow: the faint green oxygen layer near 90 km that rims the night limb in photos from orbit.
            float kmA = h * 6371.0;
            float glow = exp(-pow((kmA - 92.0) / 16.0, 2.0)) * (shadowed ? 1.0 : 0.15); // wide enough not to band
            auroraLight += exp(-betaR * odView) * vec3(0.18, 0.55, 0.28) * glow * ds * 0.005;
        }
    }

    vec3 T = exp(-(betaR + betaM * 1.1) * odView);
    // Sunlight strength: the same compressed inverse-square the surfaces use.
    const float AU = 4.848e-6;
    float sunDist2 = max(dot(pc.sunPos.xyz - center, pc.sunPos.xyz - center), 1e-30);
    float irradiance = pow((AU * AU) / sunDist2, 0.3);
    // The limb needs a boost to read at explorer distances, but the same boost turns the ground blue
    // when looking down from orbit: scale it by the optical depth of the path (~0.012 vertical, ~0.27
    // grazing, in radius units) so nadir views keep the surface and the limb keeps its glow.
    float boost = mix(1.0, 5.0, smoothstep(0.02, 0.16, odView));
    vec3 sunColor = vec3(1.0, 0.98, 0.95) * irradiance * boost;

    outColor = vec4(inscatter * sunColor + auroraLight * 40.0, dot(T, vec3(0.3333)));
}
