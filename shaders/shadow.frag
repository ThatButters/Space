#version 460
// Depth-only pass for spacecraft shadow maps: the rasteriser stores depth; cropped parts cast nothing.
struct Craft { mat4 model; vec4 sunDir; vec4 tint; vec4 up; vec4 extra; vec4 crop; };
layout(std430, set = 0, binding = 0) readonly buffer Crafts { Craft crafts[]; };
layout(push_constant) uniform PushConstants { mat4 viewProj; vec4 baseColor; ivec4 ids; } pc;
layout(location = 3) in vec3 vModelPos;
void main() { if (vModelPos.y > crafts[pc.ids.x].crop.x) discard; }
