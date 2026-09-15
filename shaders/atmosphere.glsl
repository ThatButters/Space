// Physically based planetary atmospheres, after Hillaire, "A Scalable and Production Ready Sky and
// Atmosphere Rendering Technique" (EGSR 2020). Everything here is in kilometres. Each atmosphere class
// has two lookup tables baked once at startup (atmo_lut_*.comp): the transmittance from a point to the top
// of the atmosphere by (height, zenith cosine), 256x64, and the multiple-scattering contribution by
// (height, Sun zenith cosine), 32x32. The sky itself is ray marched per pixel against them (atmo.frag).
// Include after common.glsl (PI).
#ifndef SPACE_ATMOSPHERE_GLSL
#define SPACE_ATMOSPHERE_GLSL

struct AtmoClass {
    float radiusKm;
    float topKm;        // height of the top of the atmosphere above radiusKm
    vec3 rayleigh;      // Rayleigh scattering at the reference surface, 1/km
    float rayleighH;    // Rayleigh scale height, km
    vec3 mieScatter;    // aerosol / haze scattering at the reference surface, 1/km
    float mieH;         // aerosol scale height, km
    vec3 mieAbsorb;     // aerosol absorption, 1/km
    float mieG;         // Henyey-Greenstein asymmetry of the aerosols
    vec3 absorb;        // extra absorber at its peak (ozone on Earth, methane on the ice giants), 1/km
    float absorbCentre; // km; < 0: follows the Rayleigh density instead of forming a layer
    float absorbWidth;  // km, half width of the tent-shaped layer
    vec3 groundAlbedo;  // for light bounced back up into the atmosphere
};

const int ATMO_CLASSES = 8;
// 0 Earth, 1 Mars, 2 Venus (reference surface: the cloud tops), 3 Titan, 4 Jupiter, 5 Saturn, 6 Uranus,
// 7 Neptune. Earth is Hillaire's reference atmosphere; the top sits high enough to hold the aurora.
const AtmoClass kAtmo[ATMO_CLASSES] = AtmoClass[ATMO_CLASSES](
    AtmoClass(6371.0, 320.0, vec3(5.802e-3, 13.558e-3, 33.1e-3), 8.0, vec3(3.996e-3), 1.2, vec3(4.4e-4), 0.8,
              vec3(0.650e-3, 1.881e-3, 0.085e-3), 25.0, 15.0, vec3(0.3)),
    AtmoClass(3389.5, 60.0, vec3(2.3e-4, 5.4e-4, 1.3e-3), 11.1, vec3(0.022, 0.018, 0.014), 8.0,
              vec3(0.004, 0.007, 0.012), 0.76, vec3(0.0), 0.0, 1.0, vec3(0.28, 0.18, 0.12)),
    AtmoClass(6051.8, 90.0, vec3(2.9e-3, 6.8e-3, 1.65e-2), 15.9, vec3(0.004, 0.0035, 0.0025), 15.0,
              vec3(0.0005, 0.001, 0.003), 0.7, vec3(0.0), 0.0, 1.0, vec3(0.8, 0.75, 0.6)),
    AtmoClass(2574.7, 400.0, vec3(1.0e-4, 2.4e-4, 6.0e-4), 40.0, vec3(0.012, 0.009, 0.005), 65.0,
              vec3(0.0006, 0.0012, 0.0026), 0.65, vec3(0.0), 0.0, 1.0, vec3(0.2, 0.15, 0.1)),
    // The giants' textures already show their cloud tops as seen through the air above them, so only a thin
    // veil of haze is modelled (a full column would wash the disc blue); it still brightens the limb.
    AtmoClass(69911.0, 350.0, vec3(3.5e-4, 8.2e-4, 2.0e-3), 27.0, vec3(0.0006), 20.0, vec3(0.00006), 0.7,
              vec3(0.0), 0.0, 1.0, vec3(0.6, 0.5, 0.4)),
    AtmoClass(58232.0, 500.0, vec3(2.5e-4, 5.8e-4, 1.4e-3), 59.5, vec3(0.0005, 0.00047, 0.0004), 40.0, vec3(0.00006), 0.7,
              vec3(0.0), 0.0, 1.0, vec3(0.7, 0.62, 0.5)),
    AtmoClass(25362.0, 400.0, vec3(8.7e-4, 2.0e-3, 5.0e-3), 27.7, vec3(0.0003), 20.0, vec3(0.00004), 0.7,
              vec3(0.002, 0.0004, 0.00007), 0.0, 55.0, vec3(0.5, 0.7, 0.75)),
    AtmoClass(24622.0, 400.0, vec3(8.7e-4, 2.0e-3, 5.0e-3), 19.7, vec3(0.0003), 20.0, vec3(0.00004), 0.7,
              vec3(0.003, 0.0005, 0.0001), 0.0, 40.0, vec3(0.3, 0.45, 0.8))
);

// One class's constants. Always go through this rather than kAtmo[i] with a runtime index: the compute
// bakes read garbage for the last classes that way (the ice giants' tables came out NaN), while constant
// indices fold cleanly.
AtmoClass atmoClass(int c) {
    switch (c) {
    case 1: return kAtmo[1];
    case 2: return kAtmo[2];
    case 3: return kAtmo[3];
    case 4: return kAtmo[4];
    case 5: return kAtmo[5];
    case 6: return kAtmo[6];
    case 7: return kAtmo[7];
    default: return kAtmo[0];
    }
}

// Extinction at a height (km), with the scattering split into its Rayleigh and aerosol parts.
vec3 atmoExtinction(AtmoClass a, float hKm, out vec3 scatR, out vec3 scatM) {
    float h = max(hKm, 0.0);
    float dR = exp(-h / a.rayleighH);
    float dM = exp(-h / a.mieH);
    float dA = a.absorbCentre < 0.0 ? dR : max(0.0, 1.0 - abs(h - a.absorbCentre) / a.absorbWidth);
    scatR = a.rayleigh * dR;
    scatM = a.mieScatter * dM;
    return scatR + scatM + a.mieAbsorb * dM + a.absorb * dA;
}

// Near and far distances along a ray to a sphere at the origin (far < near: miss). Uses the closest
// approach, which stays stable when the ray starts very far away.
vec2 atmoRaySphere(vec3 ro, vec3 rd, float radius) {
    float b = dot(ro, rd);
    vec3 closest = ro - b * rd;
    float disc = radius * radius - dot(closest, closest);
    if (disc < 0.0) return vec2(1.0, -1.0);
    float s = sqrt(disc);
    return vec2(-b - s, -b + s);
}

float atmoPhaseRayleigh(float mu) { return 3.0 / (16.0 * PI) * (1.0 + mu * mu); }

float atmoPhaseMie(float g, float mu) { // Cornette-Shanks
    float gg = g * g;
    return 3.0 / (8.0 * PI) * ((1.0 - gg) * (1.0 + mu * mu)) / ((2.0 + gg) * pow(max(1.0 + gg - 2.0 * g * mu, 1e-4), 1.5));
}

const vec2 ATMO_T_SIZE = vec2(256.0, 64.0);
const vec2 ATMO_MS_SIZE = vec2(32.0, 32.0);

// Transmittance LUT coordinates (Bruneton's non-linear mapping: resolution where the horizon needs it).
vec2 atmoTransmittanceUv(float R, float Rt, float r, float mu) {
    float H = sqrt(max(Rt * Rt - R * R, 0.0));
    float rho = sqrt(max(r * r - R * R, 0.0));
    float disc = r * r * (mu * mu - 1.0) + Rt * Rt;
    float d = max(0.0, -r * mu + sqrt(max(disc, 0.0)));
    float dMin = max(Rt - r, 0.0), dMax = rho + H;
    vec2 x = clamp(vec2((d - dMin) / max(dMax - dMin, 1e-6), rho / max(H, 1e-6)), 0.0, 1.0);
    return (x * (ATMO_T_SIZE - 1.0) + 0.5) / ATMO_T_SIZE;
}

// The inverse, for baking: height (as a radius) and zenith cosine at an integer texel.
void atmoTransmittanceParams(float R, float Rt, vec2 texel, out float r, out float mu) {
    vec2 x = texel / (ATMO_T_SIZE - 1.0);
    float H = sqrt(Rt * Rt - R * R);
    float rho = H * x.y;
    r = sqrt(rho * rho + R * R);
    float dMin = Rt - r, dMax = rho + H;
    float d = dMin + x.x * (dMax - dMin);
    mu = d <= 0.0 ? 1.0 : clamp((H * H - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
}

vec2 atmoMsUv(float R, float Rt, float r, float muS) {
    vec2 x = clamp(vec2(muS * 0.5 + 0.5, (r - R) / (Rt - R)), 0.0, 1.0);
    return (x * (ATMO_MS_SIZE - 1.0) + 0.5) / ATMO_MS_SIZE;
}

#endif
