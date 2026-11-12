#version 450
// build_shaders: keep functions
//
// Copyright (c) 2025 Carlos Lopez Jr.
// Copyright (C) 2026 Carlos Lopez
// SPDX-License-Identifier: MIT
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
// Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
// WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// The HDR tonemap, as a function that runtime/vk_hdr.c (vk_hdr_shader_swap) splices into the game's own tonemap
// shader. nx_tonemap takes x, the three values the game feeds its fitted ACES curve (its scene light after its colour
// matrix, bloom, exposure and, in the gameplay shader, local exposure), and scene, the same values without local
// exposure (rgb) with the exposure gain in them (a; 1 where the splice found none). It returns the colour the shader
// then stores, linear Rec. 709 relative to the HUD's white, which the present encodes to PQ (shaders/hdr_encode.comp).
// main only exists so the compiler keeps the functions; vk_hdr.c drops it. build_shaders.py compiles this file without
// optimisation ("keep functions" above), as inlining would leave no function to splice.
//
// The HDR picture (scene_tonemap, psycho35): the scene light goes through a finite cone response in LMS, anchored so
// that scene grey 0.18 lands where the game's curve puts it, and rising through it as steeply as the game's curve
// does at its steepest; the response saturates at the display's peak. Chroma is the response's residual around its
// mean, scaled back just enough to fit Rec. 709 (the game's scene target is r11g11b10f, which holds no negative
// component, so a wider fit would be clipped channel by channel). Bright colours get half of the hue bend the game's
// clamp gives them. It takes the scene without local exposure. The present then shows the picture as a 2.2 display
// would (hdr_encode.comp); the peak given to the response is lowered by that correction's inverse, so that the
// brightest light still lands on the display's peak.
// Settings (vk_hdr.c vk_hdr_support, support buffer bytes 1440..1455): p0 = peak nits, game nits (white of the
// picture), HUD nits, and -1 for the game's own SDR picture (as when the swapchain is not HDR10) or 1 for the HDR one.
layout(location = 0) in vec4 a0;
layout(location = 1) in vec4 a1;
layout(location = 2) in vec4 a2;
layout(location = 0) out vec4 o;

// Matrices are written row by row; mulm(m, v) is the row-vector product of that layout.
vec3 mulm(mat3 m, vec3 v) { return v * m; }

float y709(vec3 c) { return dot(c, vec3(0.2126390059, 0.7151686788, 0.0721923154)); }

vec3 copy_sign(vec3 mag, vec3 s) {
    vec3 a = abs(mag);
    return mix(a, -a, lessThan(s, vec3(0.0)));
}

float srgb_decode(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }

// A value of a gamma-g picture as the sRGB value of the same code (the peak's correction).
float gamma_to_srgb(float c, float g) { return srgb_decode(pow(c, 1.0 / g)); }

vec3 aces_fitted(vec3 x) {   // the game's curve and clamp
    return clamp(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

// ---- the observer-response mapper

// Rec. 709 (D65) to the cone fundamentals LMS of the two-degree observer and back; rows. The products of the XYZ
// matrices of each, worked out in double precision.
const mat3 PSY_BT709_TO_LMS = mat3(0.2896057766, 0.6972466442, 0.0763712786,
                                   0.0901837576, 0.7073490257, 0.1122031464,
                                   0.0155287401, 0.0536093073, 0.5097978305);
const mat3 PSY_LMS_TO_BT709 = mat3(4.9676196505, -4.9223795294, 0.3391991812,
                                   -0.6196828788, 2.0517506515, -0.3587439844,
                                   -0.0861520040, -0.0658193830, 1.9889544833);
const vec3 PSY_D65_WHITE_LMS = vec3(1.0632236993, 0.9097359297, 0.5789358779);   // PSY_BT709_TO_LMS of white

// Exact mean on equal-component vectors.
float psy_mean(vec3 v) { return v.x + ((v.y - v.x) + (v.z - v.x)) / 3.0; }

// A residual in white-normalised LMS as the displacement it is in Rec. 709.
vec3 psy_rgb_residual(vec3 normalized_lms) { return mulm(PSY_LMS_TO_BT709, normalized_lms * PSY_D65_WHITE_LMS); }

// The finite response u of one cone in the log domain: log_u, and log_deficit = log(1 - u), kept as logarithms so
// that neither end underflows.
void psy_response_logs(float log_q, float k, float n, out float log_u, out float log_deficit) {
    float latent = log(k) - log(1.0 - k) + n * log_q;
    float correction = log(1.0 + exp(-abs(latent)));
    log_u = min(latent, 0.0) - correction;
    float log_tail = -max(latent, 0.0) - correction;
    log_deficit = latent > 20.0 ? log_tail
                                : log(abs(log_u) < 0.01
                                          ? -log_u * (1.0 + log_u * (0.5 + log_u * (1.0 / 6.0 + log_u / 24.0)))
                                          : 1.0 - exp(log_u));
}

// Extended linear Rec. 709 in, relative to reference white; the same out, inside Rec. 709 and below peak.
// anchor_in is the scene grey the eye is adapted to, anchor_out where it lands; cone is the response's exponent.
vec3 psycho35(vec3 bt709, float peak, float cone, float anchor_in_gray, float anchor_out_gray) {
    vec3 anchor_in = mulm(PSY_BT709_TO_LMS, vec3(anchor_in_gray));
    vec3 anchor_out = mulm(PSY_BT709_TO_LMS, vec3(anchor_out_gray));
    vec3 source_ratio = mulm(PSY_BT709_TO_LMS, bt709) / anchor_in;   // the input against its adaptation
    vec3 anchor_power = anchor_out / (PSY_D65_WHITE_LMS * peak);
    vec3 input_exponent = cone / (1.0 - anchor_power);
    if (all(equal(source_ratio, vec3(0.0)))) return vec3(0.0);
    // the exposure factored out: source_ratio = drive * ratio, the largest component of ratio 1
    float drive = max(abs(source_ratio.x), max(abs(source_ratio.y), abs(source_ratio.z)));
    vec3 ratio = source_ratio / drive;
    vec3 log_ratio = log(vec3(ratio.x != 0.0 ? ratio.x : 1.0, ratio.y != 0.0 ? ratio.y : 1.0, ratio.z != 0.0 ? ratio.z : 1.0));
    vec3 response, deficit;
    for (int i = 0; i < 3; ++i) {
        if (ratio[i] == 0.0) {
            response[i] = 0.0;
            deficit[i] = 1.0;
        } else {
            float log_u, log_deficit;
            psy_response_logs(log_ratio[i] + log(drive), anchor_power[i], input_exponent[i], log_u, log_deficit);
            response[i] = exp(log_u);
            deficit[i] = exp(log_deficit);
        }
    }
    float response_mean = psy_mean(response);
    vec3 response_residual = response_mean > 0.5 ? psy_mean(deficit) - deficit : response - response_mean;
    // normalise before squaring: the length can underflow where the vector itself does not
    float residual_scale = max(abs(response_residual.x), max(abs(response_residual.y), abs(response_residual.z)));
    vec3 scaled_residual = residual_scale > 0.0 ? response_residual / residual_scale : vec3(0.0);
    float scaled_radius = length(scaled_residual);
    float radius = residual_scale * scaled_radius;
    vec3 direction = scaled_radius > 0.0 ? scaled_residual / scaled_radius : vec3(0.0);
    // the chroma's direction half way to the source's
    vec3 source = ratio - psy_mean(ratio);
    float source_radius = length(source);
    if (source_radius > 0.0 && radius > 0.0) {
        vec3 midpoint_direction = source / source_radius + direction;
        float midpoint_length = length(midpoint_direction);
        if (midpoint_length > 0.0) direction = midpoint_direction / midpoint_length;
    }
    float mean = peak * response_mean, headroom = peak * psy_mean(deficit);
    vec3 residual = peak * radius * direction;
    if (all(equal(residual, vec3(0.0)))) return vec3(mean);   // neutral: nothing to fit
    // the exact fit: the chroma scaled until the nearest face of the cube 0..peak is touched
    vec3 displacement = psy_rgb_residual(residual);
    float scale = 1.0;
    for (int channel = 0; channel < 3; ++channel) {
        if (displacement[channel] > 0.0) scale = min(scale, headroom / displacement[channel]);
        else if (displacement[channel] < 0.0) scale = min(scale, mean / -displacement[channel]);
    }
    return mean + psy_rgb_residual(residual * scale);
}

// The bend the game's clamp gives a bright colour (a clipped channel stops, the others catch up, the hue turns towards
// yellow, cyan or white), given to the unclamped colour: the curve's result, stretched back over the colour's own
// range of magnitudes, at the colour's luminance.
vec3 hue_clip(vec3 untonemapped, float amount) {
    vec3 magnitude = abs(untonemapped);
    vec3 reference = aces_fitted(magnitude);
    float low = min(reference.x, min(reference.y, reference.z));
    float range = max(reference.x, max(reference.y, reference.z)) - low;
    bool neutral = range == 0.0;
    float mag_low = min(magnitude.x, min(magnitude.y, magnitude.z));
    float mag_high = max(magnitude.x, max(magnitude.y, magnitude.z));
    vec3 transferred = mix(vec3(mag_low), vec3(mag_high), (reference - low) / (neutral ? 1.0 : range));
    transferred = copy_sign(transferred, untonemapped);
    if (neutral) transferred = untonemapped;
    vec3 bent = mix(untonemapped, transferred, amount);
    float original_y = y709(untonemapped), bent_y = y709(bent);
    if (original_y > 0.0 && bent_y > 0.0) untonemapped = bent * (original_y / bent_y);
    return untonemapped;
}

// The scene (before the exposure gain) mapped onto the display, in picture whites. peak: the display's peak in
// picture whites, as the response is to reach it.
vec3 scene_tonemap(vec3 untonemapped, float gain, float peak) {
    untonemapped = hue_clip(untonemapped, 0.5);
    // the log-log slope x V'(x) / V(x) of the game's curve where V''(x) = 0, at x = 0.120305925
    const float cone = 1.3355037565264822;
    // scene grey where the game's curve puts it: the curve's value at 0.18 times the gain
    float anchor_out = aces_fitted(vec3(0.18)).x * gain;
    return psycho35(untonemapped, peak, cone, 0.18, anchor_out);
}

vec3 nx_tonemap(vec3 x, vec4 scene, vec4 p0) {
    vec3 vanilla = aces_fitted(x);
    if (p0.w < 0.0) return vanilla;   // the game's own SDR picture
    // the mapper takes the light before the gain; without a gain there is no light
    if (!(scene.a > 0.0)) return vec3(0.0);
    float peak = gamma_to_srgb(max(p0.x, p0.y) / p0.y, 2.2);   // in picture whites, before the present's correction
    vec3 color = scene_tonemap(scene.rgb / scene.a, scene.a, peak);
    color *= p0.y / p0.z;   // the picture's white against the HUD's
    return mix(color, vec3(0.0), isnan(color));
}

void main() { o = vec4(nx_tonemap(a0.xyz, a1, a2), 1.0); }
