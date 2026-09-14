#version 460
// Instanced bodies: unit sphere (w = 0) or unit ring annulus (w = radial 0..1) scaled and rotated per body.

struct Body {
    vec4 posRadius; // xyz camera-relative, w radius
    vec4 rotation;  // quaternion
    vec4 color;
    vec4 params;    // type, seed, atmosphere or ring inner ratio, emissive
    ivec4 tex;      // day, night, clouds, unused
    ivec4 tiles; // tiled day map: first index (-1 = none), columns, rows
    ivec4 relief;       // normal map (east/north), height map, -1 = none
    vec4 reliefParams;  // height min km, height range km, normal strength, detail kind
    ivec4 patchTex;     // local terrain patch: albedo, normal, height (-1 = none)
    vec4 patchParams;   // height min m, height range m, metres per texel, 1 when active
};
layout(std430, set = 0, binding = 0) readonly buffer Bodies { Body bodies[]; };

layout(push_constant) uniform PushConstants {
    mat4 viewProj; // rotation-only view * reversed-Z projection
    vec4 sunPos;   // xyz camera-relative
    vec4 params;   // x time, y sun radiance, z shell scale (0 = surface pass)
} pc;

layout(location = 0) in vec4 inVertex;

layout(location = 0) out vec3 vWorldPos; // camera-relative
layout(location = 1) out vec3 vLocal;    // body-local unit position (for procedural surfaces / uv)
layout(location = 2) out vec3 vNormal;
layout(location = 3) flat out int vBody;
layout(location = 4) out float vRadial;

vec3 rotate(vec4 q, vec3 v) { return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v); }

void main() {
    Body b = bodies[gl_InstanceIndex];
    int type = int(b.params.x + 0.5);

    vec3 local = inVertex.xyz;
    float radial = inVertex.w;
    if (type == 5) {
        // Ring: annulus in the body's equatorial plane from inner ratio to 1.
        float r = mix(b.params.z, 1.0, radial);
        local = vec3(local.x * r, 0.0, local.z * r);
    }

    float shell = pc.params.z;
    if (shell > 0.0) {
        // Atmosphere pass: only bodies with an atmosphere, drawn on a slightly larger sphere.
        if (b.params.z <= 0.0 || type == 0 || type == 5) {
            gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
            vWorldPos = vec3(0.0); vLocal = vec3(0.0); vNormal = vec3(0.0, 1.0, 0.0); vBody = 0; vRadial = 0.0;
            return;
        }
        local *= shell;
    } else if (type != 5) {
        // Surface pass: a proxy just outside the true sphere (the 48x96 facets sit at most 0.11% inside
        // a sphere through their corners); the fragment shader ray-traces the exact surface.
        local *= 1.0015;
    }
    vec3 world = b.posRadius.xyz + rotate(b.rotation, local) * b.posRadius.w;
    vWorldPos = world;
    vLocal = inVertex.xyz;
    vNormal = rotate(b.rotation, type == 5 ? vec3(0.0, 1.0, 0.0) : normalize(inVertex.xyz));
    vBody = gl_InstanceIndex;
    vRadial = radial;
    gl_Position = pc.viewProj * vec4(world, 1.0);
}
