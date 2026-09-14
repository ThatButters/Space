#version 460
// Dust motes and comets: a Gaussian head at the leading end of the quad and a tail that fades toward
// the trailing end. vUV.x runs from -1 (head / current position) to +1 (tail / where it came from).

layout(location = 0) in vec2 vUV;
layout(location = 1) in vec3 vColor;
layout(location = 2) in float vRadius;
layout(location = 3) in float vTail; // 0 = plain mote, 1 = comet (bright tail)
layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

void main() {
    float across = exp(-vUV.y * vUV.y * 4.5);
    // Head: round Gaussian near x = -1 (compress x so the head is a disc, not a bar).
    float hx = (vUV.x + 1.0) * 2.0 - 1.0;
    float r2 = hx * hx + vUV.y * vUV.y;
    // Plain motes: a soft puff (wide Gaussian + faint halo); comets: a tight bright head.
    float head = vTail > 0.5 ? exp(-r2 * 4.5) * step(vUV.x, 0.0)
                             : (0.55 * exp(-r2 * 2.2) + 0.45 * exp(-r2 * 0.9)) * step(vUV.x, 0.0) * (1.0 - smoothstep(0.7, 1.0, r2));
    // Tail: brightest right behind the head, fading toward +1.
    float along = smoothstep(1.0, -0.8, vUV.x);
    float tail = across * along * along * (0.18 + 0.55 * vTail);
    float sigmaPx = max(vRadius, 0.6) / 3.0;
    float norm = 1.0 / (2.0 * PI * sigmaPx * sigmaPx);
    outColor = vec4(vColor * (head + tail) * norm, 1.0);
}
