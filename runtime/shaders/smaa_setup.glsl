// Settings and resources shared by the SMAA passes (runtime/vk_aa.c; smaa_*.comp). smaa.glsl holds the SMAA reference
// implementation, unmodified; its pixel- and vertex-shader functions run here inside compute shaders, one invocation
// per pixel, with the texture coordinate of the pixel's centre. Its "discard" of a pixel without edges becomes nothing:
// the edge pass then stores the zero edges it found, which is what the next pass reads for such a pixel anyway.
#extension GL_GOOGLE_include_directive : require
layout(local_size_x = 8, local_size_y = 8) in;
layout(push_constant) uniform Push {
    vec4 metrics;   // 1 / width, 1 / height, width, height
} pc;
#define SMAA_RT_METRICS pc.metrics
#define SMAA_GLSL_4 1
#define SMAA_PRESET_ULTRA 1
#define discard
#include "smaa.glsl"

// the pixel this invocation works on and its centre's texture coordinate; false past the image's edge
bool smaa_pixel(out ivec2 p, out vec2 uv) {
    p = ivec2(gl_GlobalInvocationID.xy);
    uv = (vec2(p) + 0.5) * pc.metrics.xy;
    return p.x < int(pc.metrics.z) && p.y < int(pc.metrics.w);
}
