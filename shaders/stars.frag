#version 460
// Energy-conserving Gaussian point-spread function. Additively blended into the HDR target.

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vColor;
layout(location = 2) in float vRadius;
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

void main() {
    float r2 = dot(vUV, vUV);
    if (r2 > 1.0) discard;

    // sigma = radius/3 so the quad edge sits at 3 sigma.
    float sigmaPx = vRadius / 3.0;
    float gaussian = exp(-r2 * 4.5); // exp(-(r/sigma)^2 / 2) with r in units of radius
    float norm = 1.0 / (2.0 * PI * sigmaPx * sigmaPx);

    // A faint wider halo gives bright stars a soft glow before real bloom lands.
    float halo = exp(-sqrt(r2) * 3.0) * 0.015;

    // Diffraction: the brightest stars grow faint four-point spikes (a lens aperture's signature); the
    // energy comes out of the core so the total stays put. Dim stars get none.
    float bright = smoothstep(4.0, 9.0, vRadius);
    float spikes = (exp(-abs(vUV.y) * 28.0) * exp(-abs(vUV.x) * 3.5) +
                    exp(-abs(vUV.x) * 28.0) * exp(-abs(vUV.y) * 3.5)) * 0.03 * bright;
    gaussian *= 1.0 - 0.05 * bright;

    outColor = vec4(vColor * (gaussian * norm + (halo + spikes) * norm), 1.0);
}
