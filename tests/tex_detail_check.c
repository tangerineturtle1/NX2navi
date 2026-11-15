// runtime/vk_tex_detail.h: the level texture_detail = auto picks (vk_texture.c vk_texture_detail_auto), for the cards and
// budgets players reported. Built and run by tests/tex_detail_check.py; prints each failed check and exits 1 on any.
#include <stdio.h>

#include "vk_tex_detail.h"

static int fails;
static const char *const names[] = {"full", "reduced", "low"};

static void expect(long long mb, int shared, int rgba8, int want, const char *what) {
    int got = tex_detail_auto(mb, shared, rgba8);
    if (got != want) {
        fails++;
        printf("FAIL: %s (budget %lld MiB, integrated %d, uncompressed ASTC %d): %s, not %s\n", what, mb, shared, rgba8,
               names[got], names[want]);
    } else {
        printf("  ok: %-62s budget %5lld MiB -> %s\n", what, mb, names[got]);
    }
}

int main(void) {
    // ---- the budgets in players' logs of 0.10.1
    expect(10361, 1, 0, 1, "ROG Ally X, integrated (0.10.1: full, ended at save load)");
    expect(7365, 0, 0, 1, "RX 7600, 8 GiB");
    expect(11313, 0, 0, 0, "RX 7700 XT, 12 GiB");
    expect(15569, 0, 0, 0, "RX 6800 XT, 16 GiB");
    expect(31419, 0, 0, 0, "RTX 5090, 32 GiB");
    // ---- between the old 8,192 MiB and the new 10,240: the cards of 10 GiB
    expect(9300, 0, 0, 1, "a 10 GiB card (0.10.1: full)");
    expect(8192, 0, 0, 1, "8,192 MiB, where full started in 0.10.1");
    expect(10239, 0, 0, 1, "one MiB under the new start of full");
    expect(10240, 0, 0, 0, "the new start of full");
    // ---- low under 6,144 MiB, as before
    expect(6144, 0, 0, 1, "6,144 MiB");
    expect(6143, 0, 0, 2, "one MiB under it");
    expect(4730, 0, 0, 2, "an RTX 3060 Laptop, 6 GiB");
    // ---- an integrated GPU never gets full, and follows the budget below that
    expect(16000, 1, 0, 1, "integrated with a 16 GiB budget");
    expect(8034, 1, 0, 1, "Intel Iris Xe (8,034 MiB)");
    expect(5000, 1, 0, 2, "integrated with a 5 GiB budget");
    // ---- no budget reported: full, as before, also on an integrated GPU
    expect(0, 0, 0, 0, "no budget reported");
    expect(0, 1, 0, 0, "no budget reported, integrated");
    // ---- uncompressed ASTC textures: one step lower under 20,480 MiB
    expect(15569, 0, 1, 1, "a 16 GiB card with uncompressed ASTC");
    expect(7365, 0, 1, 2, "an 8 GiB card with uncompressed ASTC");
    expect(4730, 0, 1, 2, "a 6 GiB card with uncompressed ASTC (already low)");
    expect(24000, 0, 1, 0, "a 24 GiB card with uncompressed ASTC");
    expect(10361, 1, 1, 2, "integrated with uncompressed ASTC");

    printf("tex_detail_check: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
