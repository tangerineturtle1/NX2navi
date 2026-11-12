#version 450
// --probe-meshfb (mesh_fb_probe.c): the fragment stage of the probe's mesh programs: the interpolated generic vector 0.
layout(location = 0) in vec4 i_color;
layout(location = 0) out vec4 o_color;

void main() { o_color = i_color; }
