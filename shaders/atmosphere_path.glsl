// Aerial perspective along a straight path through a body's atmosphere (kilometres, planet-centred): the
// light scattered in toward the camera and the per-channel transmittance, from the tables baked at
// startup. Surfaces and clouds apply it themselves, where the depth is known, so the ground is dimmed and
// hazed wavelength by wavelength. Include after atmosphere.glsl and after the bindless uTex[] declaration.
#ifndef SPACE_ATMOSPHERE_PATH_GLSL
#define SPACE_ATMOSPHERE_PATH_GLSL

// Sample positions along [tA, tB], densest at tc (the deepest point of the path, or the ground).
float atmoPathEdge(float u, float tA, float tc, float tB) {
    float w = (tc - tA) / max(tB - tA, 1e-6);
    if (u <= w) {
        float k = 1.0 - u / max(w, 1e-6);
        return tc - (tc - tA) * k * k;
    }
    float k = (u - w) / max(1.0 - w, 1e-6);
    return tc + (tB - tc) * k * k;
}

void atmoPathScatter(ivec4 atmoTex, vec3 ro, vec3 rd, float tEnd, vec3 sunDir, int steps, float jitter,
                     out vec3 inscatter, out vec3 transmittance) {
    inscatter = vec3(0.0);
    transmittance = vec3(1.0);
    AtmoClass a = kAtmo[atmoTex.z];
    float R = a.radiusKm, Rt = R + a.topKm;
    vec2 hit = atmoRaySphere(ro, rd, Rt);
    float tA = max(hit.x, 0.0), tB = min(hit.y, tEnd);
    if (hit.y <= 0.0 || tB <= tA) return;
    float mu = dot(rd, sunDir);
    float phaseR = atmoPhaseRayleigh(mu), phaseM = atmoPhaseMie(a.mieG, mu);
    float tc = clamp(-dot(ro, rd), tA, tB);
    float tPrev = tA;
    for (int i = 0; i < steps; ++i) {
        float tNext = atmoPathEdge(float(i + 1) / float(steps), tA, tc, tB);
        float dt = tNext - tPrev;
        float t = tPrev + dt * jitter;
        tPrev = tNext;
        if (dt <= 0.0) continue;
        vec3 p = ro + rd * t;
        float pr = length(p);
        float muS = dot(p / pr, sunDir);
        vec3 sR, sM;
        vec3 ext = atmoExtinction(a, pr - R, sR, sM);
        float along = dot(p, sunDir);
        float lit = along > 0.0 ? 1.0 : smoothstep(R - 0.5 * a.rayleighH, R + 0.5 * a.rayleighH, length(p - along * sunDir));
        vec3 sunT = textureLod(uTex[nonuniformEXT(atmoTex.x)], atmoTransmittanceUv(R, Rt, pr, muS), 0.0).rgb * lit;
        vec3 ms = textureLod(uTex[nonuniformEXT(atmoTex.y)], atmoMsUv(R, Rt, pr, muS), 0.0).rgb;
        vec3 S = (sR * phaseR + sM * phaseM) * sunT + (sR + sM) * ms;
        vec3 stepT = exp(-ext * dt);
        inscatter += transmittance * S * (vec3(1.0) - stepT) / max(ext, vec3(1e-12));
        transmittance *= stepT;
    }
}

#endif
