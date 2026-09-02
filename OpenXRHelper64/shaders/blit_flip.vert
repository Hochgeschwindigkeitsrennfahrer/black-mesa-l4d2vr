#version 450

// Fullscreen triangle matching kOpenXrBlitVertSpvBase64, with NDC Y negated.
// SteamVR+Touch stayed inverted after swapping blit UV 0/1 (2026-09-01).
// Do not use a negative viewport (G2/SteamVR yellow bands).

const vec2 kPos[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
const vec2 kUv[3] = vec2[](vec2(0.0, 0.0), vec2(2.0, 0.0), vec2(0.0, 2.0));

layout(location = 0) out vec2 vUv;

void main()
{
    vec2 p = kPos[gl_VertexIndex];
    p.y = -p.y;
    gl_Position = vec4(p, 0.0, 1.0);
    vUv = kUv[gl_VertexIndex];
}
