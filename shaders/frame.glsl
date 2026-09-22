// Per-frame data shared by several passes: music features + constellation highlights.
// Must match render::FrameGpu in Renderer.h. Use FRAME_DATA_BLOCK(set) once per shader.
#ifndef SPACE_FRAME_GLSL
#define SPACE_FRAME_GLSL

#define FRAME_DATA_BLOCK(SET) \
    layout(std430, set = SET, binding = 0) readonly buffer FrameData { \
        vec4 bands[8];        /* 32 spectrum bands, 0..1 */ \
        vec4 features;        /* bass, mid, treble, beat envelope */ \
        vec4 misc;            /* beat age (s), level, reactivity, time */ \
        vec4 viewForward;     /* xyz camera forward */ \
        vec4 mood;            /* rgb pulse colour from the music character, w saturation */ \
        float highlight[128]; /* constellation highlight */ \
    } frame; \
    float frameBand(int i) { return frame.bands[(i & 31) >> 2][i & 3]; } \
    float frameBass() { return frame.features.x; } \
    float frameMid() { return frame.features.y; } \
    float frameTreble() { return frame.features.z; } \
    float frameBeat() { return frame.features.w; } \
    float frameBeatAge() { return frame.misc.x; } \
    float frameLevel() { return frame.misc.y; } \
    float frameReact() { return frame.misc.z; } \
    vec3 frameMood() { return frame.mood.rgb; } \
    /* Frequency mapped to direction: the 32 bands sit on a Fibonacci sphere with bass below and */ \
    /* highs above (galactic frame), so each region of sky breathes with its own part of the mix. */ \
    float frameSkyBand(vec3 dir) { \
        vec3 d = normalize(dir); \
        float sum = 0.0, norm = 0.0; \
        for (int i = 0; i < 32; ++i) { \
            float y = -1.0 + 2.0 * (float(i) + 0.5) / 32.0; \
            float rr = sqrt(max(1.0 - y * y, 0.0)); \
            float az = 2.39996323 * float(i); \
            vec3 c = vec3(rr * cos(az), y, rr * sin(az)); \
            float w = exp(5.0 * (dot(d, c) - 1.0)); \
            sum += w * frameBand(i); \
            norm += w; \
        } \
        return sum / max(norm, 1e-6); \
    } \
    /* Spherical shockwave expanding from the camera through space: rel = camera-relative position, */ \
    /* speed in world units per second, width in world units. 1 on the wavefront, 0 elsewhere. */ \
    float frameWave(vec3 rel, float speed, float width) { \
        float d = length(rel); \
        float t = frame.misc.x; \
        float r = speed * t * (1.0 + 0.5 * t); /* a slow tide, not a flash */ \
        float w = width * (1.0 + 0.08 * r / max(width, 1e-6)); \
        float fade = frame.mood.w * exp(-t * 0.35); \
        float u = (d - r) / w; /* squared by hand: pow of a negative base is NaN */ \
        return exp(-u * u) * fade; \
    } \
    /* Expanding ring from the view centre after each beat: 1 on the wavefront, 0 elsewhere. */ \
    float frameBeatRing(vec3 dir) { \
        float a = acos(clamp(dot(normalize(dir), frame.viewForward.xyz), -1.0, 1.0)); \
        float r = frame.misc.x * 1.4; \
        float w = 0.10 + 0.05 * r; \
        float fade = exp(-frame.misc.x * 1.1); \
        float u = (a - r) / w; \
        return exp(-u * u) * fade; \
    }

#endif
