#version 450
// VR's hands on the desktop window (runtime/vk_xr.cpp hands_onto_window): the hands' overlay (sRGB-encoded bytes,
// alpha 1 where a hand is in front) blended over the window's picture; decoded first when the window's format is an
// sRGB one, which encodes again on the write
layout(binding = 0) uniform sampler2D overlay;
layout(push_constant) uniform Push {
    int srgb_target;
} p;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

vec3 decode(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }

void main() {
    vec4 c = texture(overlay, uv);
    if (c.a <= 0.0) discard;
    color = vec4(p.srgb_target != 0 ? decode(c.rgb) : c.rgb, c.a);
}
