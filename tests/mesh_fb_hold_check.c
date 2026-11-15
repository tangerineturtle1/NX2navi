// runtime/mesh_fb_hold.h: the table behind the mesh fallback's depth hold (vk_exec_draw.c): a list whose colour draw
// was skipped for its pipeline holds its depth-only draw for a few presents, and no longer. Built and run by
// tests/mesh_fb_hold_check.py; prints each failed check and exits 1 on any.
#include <stdio.h>
#include <string.h>

#include "mesh_fb_hold.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static volatile uint64_t tab[MFB_HOLD_SLOTS];

int main(void) {
    const uint64_t grass = 0x59220f600ull, leaves = 0x26d876000ull;   // record addresses as the game's memory has them
    // ---- a frame's order is depth first, colour after: the stamp of one frame holds the depth draw of the next
    uint64_t present = 1000;
    CHECK(!mfb_hold_test(tab, grass, present), "a list nothing stamped is held");
    mfb_hold_mark(tab, grass, 1, present);   // the colour draw waits for its pipeline
    CHECK(mfb_hold_test(tab, grass, present), "not held in the present of the stamp");
    CHECK(mfb_hold_test(tab, grass, present + 1), "not held one present later");
    CHECK(mfb_hold_test(tab, grass, present + MFB_HOLD_PRESENTS), "not held at the last present of the hold");
    CHECK(!mfb_hold_test(tab, grass, present + MFB_HOLD_PRESENTS + 1), "still held after the hold");
    CHECK(!mfb_hold_test(tab, leaves, present), "another list is held");
    // ---- while the colour draw keeps waiting, each frame stamps again and the hold goes on
    for (int i = 1; i <= 600; i++) {
        CHECK(mfb_hold_test(tab, grass, present + (uint64_t)i), "frame %d of a wait: not held", i);
        mfb_hold_mark(tab, grass, 1, present + (uint64_t)i);
    }
    present += 600;
    // ---- the colour draw is drawn: the stamp goes, and the depth draw comes back in the next frame
    mfb_hold_mark(tab, grass, 0, present + 1);
    CHECK(!mfb_hold_test(tab, grass, present + 1), "held after the colour draw was drawn");
    CHECK(!mfb_hold_test(tab, grass, present + 2), "held in the frame after it");
    // ---- a colour draw that no longer comes (the list left the view): the hold lapses by itself
    mfb_hold_mark(tab, leaves, 1, present);
    CHECK(mfb_hold_test(tab, leaves, present + 2) && !mfb_hold_test(tab, leaves, present + 3), "the hold does not lapse");
    // ---- a draw of one list does not clear another list's stamp, in whatever slot each lies
    mfb_hold_mark(tab, grass, 1, present);
    mfb_hold_mark(tab, leaves, 0, present);
    CHECK(mfb_hold_index(grass) == mfb_hold_index(leaves) || mfb_hold_test(tab, grass, present),
          "another list's draw cleared the stamp");
    // ---- the present count's 24 bits wrap (77 hours at 60 fps): the hold counts across the wrap
    memset((void *)tab, 0, sizeof tab);
    mfb_hold_mark(tab, grass, 1, MFB_HOLD_MASK);
    CHECK(mfb_hold_test(tab, grass, MFB_HOLD_MASK + 1) && mfb_hold_test(tab, grass, MFB_HOLD_MASK + 2) &&
          !mfb_hold_test(tab, grass, MFB_HOLD_MASK + 3), "the hold is wrong across the wrap of the present count");
    // ---- the highest address the game's memory has keeps its bits above the count
    const uint64_t top = (1ull << 39) - 4;
    memset((void *)tab, 0, sizeof tab);
    mfb_hold_mark(tab, top, 1, 5);
    CHECK(mfb_hold_test(tab, top, 6) && !mfb_hold_test(tab, top - 4, 6), "the top address is not told from its neighbour");
    // ---- every index is inside the table, and a field of records spreads over it
    int used[MFB_HOLD_SLOTS] = {0}, most = 0, slots = 0;
    for (uint64_t a = 0x59220f600ull; a < 0x59220f600ull + 256 * 12; a += 12) {   // 256 records of 12 bytes
        uint32_t i = mfb_hold_index(a);
        CHECK(i < MFB_HOLD_SLOTS, "index %u out of the table", i);
        if (i < MFB_HOLD_SLOTS && !used[i]++) slots++;
    }
    for (int i = 0; i < MFB_HOLD_SLOTS; i++)
        if (used[i] > most) most = used[i];
    CHECK(slots >= 192 && most <= 4, "256 records fall into %d slots, up to %d in one", slots, most);
    printf("  256 neighbouring records fall into %d slots, at most %d in one\n", slots, most);

    printf("mesh_fb_hold_check: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
