#version 450
// VR's hands (runtime/vk_xr.cpp make_hands): a model of the game's files held at a hand, drawn in the head's view as
// the game's picture was. Out: its normal in the view, its texture's place and its distance along the view.
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 normal;
layout(location = 2) in vec2 uv;
layout(push_constant) uniform Push {
    mat4 clip;   // the model's place to the picture's clip space
    mat4 view;   // the model's place to the head's view (x right, y up, z back), metres
} p;
layout(location = 0) out vec3 n_view;
layout(location = 1) out vec2 tex;
layout(location = 2) out float dist;

void main() {
    gl_Position = p.clip * vec4(pos, 1.0);
    n_view = mat3(p.view) * normal;
    tex = uv;
    dist = -(p.view * vec4(pos, 1.0)).z;
}
