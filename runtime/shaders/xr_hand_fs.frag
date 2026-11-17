#version 450
// VR's hands (runtime/vk_xr.cpp make_hands): the model's colour texture lit by a light from above and in front of the
// head, its colour (linear) and its distance along the view (metres) out for shaders/xr_hand_mix.comp
layout(binding = 0) uniform sampler2D albedo;   // sRGB, read as linear
layout(location = 0) in vec3 n_view;
layout(location = 1) in vec2 tex;
layout(location = 2) in float dist;
layout(location = 0) out vec4 color;
layout(location = 1) out float dist_out;

void main() {
    vec3 n = normalize(n_view);
    if (!gl_FrontFacing) n = -n;
    const vec3 light = normalize(vec3(0.3, 0.8, 0.5));
    const vec3 c = texture(albedo, tex).rgb;
    color = vec4(c * (0.45 + 0.65 * max(dot(n, light), 0.0)), 1.0);
    dist_out = dist;
}
