// Shared hashing / noise utilities.
#ifndef SPACE_COMMON_GLSL
#define SPACE_COMMON_GLSL

const float PI = 3.14159265359;

// Integer-quality hashes (Dave Hoskins style) that are stable across GPUs.
float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

vec3 hash33(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

// Smooth value noise in 3D.
float noise3(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash13(i + vec3(0, 0, 0)), hash13(i + vec3(1, 0, 0)), f.x),
                   mix(hash13(i + vec3(0, 1, 0)), hash13(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(hash13(i + vec3(0, 0, 1)), hash13(i + vec3(1, 0, 1)), f.x),
                   mix(hash13(i + vec3(0, 1, 1)), hash13(i + vec3(1, 1, 1)), f.x), f.y),
               f.z);
}

float fbm(vec3 p, int octaves) {
    float sum = 0.0, amp = 0.5, norm = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * noise3(p);
        norm += amp;
        p = p * 2.02 + vec3(17.1, 9.3, 4.7);
        amp *= 0.5;
    }
    return sum / norm;
}

// Approximate blackbody colour (linear sRGB) for a star temperature in Kelvin.
vec3 blackbody(float kelvin) {
    float t = clamp(kelvin, 1500.0, 40000.0) / 100.0;
    vec3 c;
    c.r = t <= 66.0 ? 1.0 : clamp(1.2929 * pow(t - 60.0, -0.1332), 0.0, 1.0);
    c.g = t <= 66.0 ? clamp(0.3901 * log(t) - 0.6318, 0.0, 1.0) : clamp(1.1299 * pow(t - 60.0, -0.0755), 0.0, 1.0);
    c.b = t >= 66.0 ? 1.0 : (t <= 19.0 ? 0.0 : clamp(0.5432 * log(t - 10.0) - 1.1962, 0.0, 1.0));
    return c * c; // rough sRGB -> linear
}

#endif
