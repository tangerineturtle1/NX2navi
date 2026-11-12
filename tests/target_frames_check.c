// runtime/vk_target_frames.h: which passes count as rendered frame after frame, so that a draw into them is skipped
// while its pipeline builds instead of holding the replay thread. Built and run by tests/target_frames_check.py;
// prints each failed check and exits 1 on any.
// The frame numbers of the first block are those of a player's log: an area change after 238 frames of loading
// screen, where every pass read as one-shot and six mesh draws waited 52 to 660 ms each.
#include <stdio.h>
#include <string.h>

#include "vk_target_frames.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static TfrTarget T(uint64_t image, uint32_t level, uint32_t layer, uint64_t owner) {
    return (TfrTarget){image, level, layer, owner};
}

// The verdict with the steady rule and without it; *by: only the steady rule made it per-frame.
static int pass(const TfrTarget *c, int n, const TfrTarget *d, uint64_t stamp, int any, int steady, int *by) {
    int b = 0, r = tfr_pass_recent(c, n, d, stamp, any, steady, &b);
    if (by) *by = b;
    return r;
}

int main(void) {
    int by;
    // ---- the area change of the log
    TfrTarget scene = T(0x1000, 0, 0, 11), scene_d = T(0x1001, 0, 0, 12);          // drawn every frame
    TfrTarget probe = T(0x2000, 0, 0, 21), probe_d = T(0x2001, 0, 0, 22);          // every second frame
    TfrTarget probe_l3 = T(0x2000, 0, 3, 21);                                      // a layer drawn every 15th frame
    TfrTarget half = T(0x3000, 0, 0, 31), half_d = T(0x3001, 0, 0, 32);            // the half-resolution buffers
    for (uint64_t f = 16000; f <= 17925; f++) {
        tfr_note(&scene, f), tfr_note(&scene_d, f), tfr_note(&half, f), tfr_note(&half_d, f);
        if (f % 2) tfr_note(&probe, f), tfr_note(&probe_d, f);
        if (f % 15 == 0) tfr_note(&probe_l3, f);
    }
    // the frame after: per-frame by being recent, with or without the rule
    CHECK(pass(&scene, 1, &scene_d, 17926, 1, 0, NULL) == 1, "a pass drawn the frame before is per-frame");
    CHECK(pass(&scene, 1, &scene_d, 17926, 0, 0, NULL) == 1, "strict: every target recent");
    CHECK(pass(&scene, 1, &scene_d, 17926, 1, 1, &by) == 1 && !by, "recent passes are not counted as steady ones");
    // 238 frames of loading screen later (its own targets are other images)
    const uint64_t now = 18163;
    TfrTarget load = T(0x9000, 0, 0, 91);
    for (uint64_t f = 17926; f < now; f++) tfr_note(&load, f);
    CHECK(pass(&scene, 1, &scene_d, now, 1, 0, NULL) == 0, "without the rule the scene pass reads as one-shot after the pause");
    CHECK(pass(&scene, 1, &scene_d, now, 1, 1, &by) == 1 && by, "the scene pass is per-frame after the pause");
    CHECK(pass(&scene, 1, &scene_d, now, 0, 1, &by) == 1 && by, "strict: the scene pass too");
    CHECK(pass(&probe, 1, &probe_d, now, 1, 1, NULL) == 1, "the probe pass (128x128, seen 17925/17923) is per-frame");
    CHECK(pass(&probe_l3, 1, &probe_d, now, 1, 0, NULL) == 0, "a layer drawn every 15th frame is never recent");
    CHECK(pass(&probe_l3, 1, &probe_d, now, 1, 1, NULL) == 1, "a layer drawn every 15th frame, in 128 frames, is steady");
    // the 960x540 pass of the log: one colour target made new, one steady, the depth target drawn earlier this frame
    TfrTarget fresh = T(0x4000, 0, 0, 41), cc[2] = {fresh, half};
    tfr_note(&fresh, now), tfr_note(&half, now), tfr_note(&half_d, now);   // the pass begins: its targets are noted
    CHECK(pass(cc, 2, &half_d, now, 1, 0, NULL) == 0, "without the rule: no recent colour target, a new one beside");
    CHECK(pass(cc, 2, &half_d, now, 1, 1, &by) == 1 && by, "a steady colour target beside a new one: per-frame");
    CHECK(pass(cc, 2, &half_d, now, 0, 1, NULL) == 0, "strict: the new colour target keeps the pass one-shot");
    // the next frame all of it is recent again
    tfr_note(&scene, now), tfr_note(&scene_d, now);
    CHECK(pass(&scene, 1, &scene_d, now + 1, 1, 0, NULL) == 1, "recent again in the next frame");
    CHECK(pass(&fresh, 1, &half_d, now + 1, 1, 0, NULL) == 1, "the new target is recent in its second frame");

    // ---- what stays one-shot
    TfrTarget lut = T(0x5000, 0, 0, 51);
    CHECK(pass(&lut, 1, NULL, now, 1, 1, NULL) == 0, "a target never rendered is one-shot");
    tfr_note(&lut, now);
    CHECK(pass(&lut, 1, NULL, now, 1, 1, NULL) == 0, "its first frame, already noted by its pass, is still one-shot");
    tfr_note(&lut, now);   // a second pass into it in the same frame
    for (int load_no = 1; load_no < TFR_STEADY; load_no++) {   // generated again at each area load, far apart
        uint64_t f = now + (uint64_t)load_no * 5000;
        CHECK(pass(&lut, 1, NULL, f, 1, 1, NULL) == 0, "a table generated at %d area loads is one-shot at the next", load_no);
        tfr_note(&lut, f);
    }
    CHECK(pass(&lut, 1, NULL, now + (uint64_t)TFR_STEADY * 5000, 1, 1, NULL) == 1,
          "after %d loads the table counts as steady: its pipelines are recorded by then", TFR_STEADY);
    // a volume generated a few slices per frame: each slice is its own target and is rendered once
    for (uint32_t z = 0; z < 64; z++) {
        TfrTarget slice = T(0x6000, 0, z, 61);
        CHECK(pass(&slice, 1, NULL, now + 10 + z / 4, 1, 1, NULL) == 0, "volume slice %u is one-shot", z);
        tfr_note(&slice, now + 10 + z / 4);
    }
    // the item icon capture: a new colour target with the per-frame depth buffer, once
    TfrTarget icon = T(0x7000, 0, 0, 71);
    uint64_t f2 = now + 1;
    tfr_note(&scene_d, f2), tfr_note(&icon, f2);
    CHECK(pass(&icon, 1, &scene_d, f2, 1, 1, NULL) == 0, "an icon's first capture is one-shot though its depth buffer is recent");
    CHECK(pass(&icon, 1, &scene_d, f2, 1, 0, NULL) == 0, "and was before the rule");
    // an image handed on to a texture at other storage starts again: the per-frame history of its handle is not the
    // new texture's
    TfrTarget reused = T(scene.image, 0, 0, 500);
    CHECK(tfr_steady(&scene, f2 + 300) == 1, "the scene target is steady");
    CHECK(pass(&reused, 1, NULL, f2 + 300, 1, 1, NULL) == 0, "the same image as another texture is not steady");
    tfr_note(&reused, f2 + 300);
    CHECK(tfr_steady(&scene, f2 + 301) == 0, "and the texture that had it lost its count with the image");
    CHECK(tfr_find(&reused, 0)->n == 1, "the new texture's count begins at one");
    // a pass with no target at all
    CHECK(pass(NULL, 0, NULL, now, 1, 1, NULL) == 0 && pass(NULL, 0, NULL, now, 0, 1, NULL) == 0, "no targets: not per-frame");

    // ---- the table keeps the steady targets through an area's first frame
    // 400 per-frame targets, a pause, then 600 targets made new are rendered before the old ones come round again
    memset(tfr, 0, sizeof tfr);
    for (uint64_t f = 1; f <= 100; f++)
        for (uint32_t i = 0; i < 400; i++) {
            TfrTarget t = T(0x7f3a00000000ull + (uint64_t)(i / 4) * 0x2c0, 0, i % 4, 1000 + i / 4);
            tfr_note(&t, f);
        }
    for (uint32_t i = 0; i < 600; i++) {
        TfrTarget t = T(0x7f3b00000000ull + (uint64_t)i * 0x2c0, 0, 0, 5000 + i);
        tfr_note(&t, 400);
    }
    int kept = 0;
    for (uint32_t i = 0; i < 400; i++) {
        TfrTarget t = T(0x7f3a00000000ull + (uint64_t)(i / 4) * 0x2c0, 0, i % 4, 1000 + i / 4);
        kept += tfr_steady(&t, 400);
    }
    CHECK(kept == 400, "%d of 400 steady targets still known after 600 new ones in one frame", kept);

    if (fails) return printf("%d checks failed\n", fails), 1;
    printf("target_frames_check: ok\n");
    return 0;
}
