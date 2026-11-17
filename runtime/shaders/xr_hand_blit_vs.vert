#version 450
// VR's hands on the desktop window (runtime/vk_xr.cpp hands_onto_window): a triangle covering the viewport, the picture's
// place in the window, its texture coordinates 0..1 over it
layout(location = 0) out vec2 uv;

void main() {
    const vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
