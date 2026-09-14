#version 460
#include "frame.glsl"
// Constellation figures: camera-relative line segments, brightness per constellation.

FRAME_DATA_BLOCK(0)

layout(push_constant) uniform PushConstants {
    mat4 viewProj; // rotation-only view * reversed-Z projection
    vec4 camPos;
    vec4 params;   // x base intensity
} pc;

layout(location = 0) in vec4 inVertex; // xyz position (parsecs), w constellation index

layout(location = 0) out vec3 vColor;

void main() {
    int c = int(inVertex.w + 0.5);
    float h = frame.highlight[c];
    vec3 rel = inVertex.xyz - pc.camPos.xyz;
    gl_Position = pc.viewProj * vec4(rel, 1.0);
    // Each figure pulses with its own band when music is playing.
    float pulse = frameReact() * (0.7 * frameSkyBand(rel) + 1.5 * frameWave(rel, 25.0, 6.0));
    // Faint steel blue when idle, warm white when highlighted.
    vec3 idle = vec3(0.25, 0.42, 0.70), hot = vec3(1.0, 0.92, 0.75);
    vColor = (mix(idle, hot, h) * (0.35 + 2.2 * h) + frameMood() * pulse) * pc.params.x;
}
