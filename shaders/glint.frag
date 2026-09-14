#version 460
// Gaussian point of light for an unresolved spacecraft (additive).
layout(location = 0) in vec2 vUV;    // in units of sigma
layout(location = 1) in vec3 vColor; // peak radiance
layout(location = 0) out vec4 outColor;

void main() {
    float g = exp(-0.5 * dot(vUV, vUV));
    outColor = vec4(vColor * g, 1.0);
}
