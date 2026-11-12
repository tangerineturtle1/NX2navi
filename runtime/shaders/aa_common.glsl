// Shared by the anti-aliasing passes (runtime/vk_aa.c) that stand in for the game's SMAA. The game's composition pass
// stores the post-processed picture into its R11G11B10F scene target as raw words: each channel is the square root of
// its linear value, the three packed together as RGB9E5 (red in bits 0-8, green 9-17, blue 18-26, the shared exponent
// in 27-31, Vulkan's E5B9G9R9 layout). The passes here write the same words through an R32_UINT view of that target.

// RGB9E5 of a non-negative colour: the shared exponent of the largest channel, each channel rounded to 9 bits of it.
uint aa_pack_rgb9e5(vec3 c) {
    const float MAXV = 65408.0;   // (2^9 - 1) / 2^9 * 2^15: the largest value the format holds
    c = clamp(c, vec3(0.0), vec3(MAXV));
    float m = max(max(c.r, c.g), c.b);
    float e = max(-16.0, floor(log2(max(m, 1e-30)))) + 16.0;   // biased by 15, plus one
    float scale = exp2(e - 24.0);                               // 2^(e - 15 - 9)
    if (floor(m / scale + 0.5) >= 512.0) e += 1.0, scale *= 2.0;
    uvec3 q = uvec3(floor(c / scale + 0.5));
    return q.r | (q.g << 9) | (q.b << 18) | (uint(e) << 27);
}

// The stored form of a linear colour: its square root, which is also the space the edges are looked for in.
vec3 aa_encode(vec3 linear) { return sqrt(max(linear, vec3(0.0))); }
