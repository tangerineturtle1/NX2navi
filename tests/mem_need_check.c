// runtime/mem_need.h: whether the machine has the commit the game needs (runtime/memory.c mem_start_note). Built and
// run by tests/mem_need_check.py; prints each failed check and exits 1 on any.
#include <stdio.h>

#include "mem_need.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    const double need = MEM_NEED_DEFAULT_MB;

    // The machine the need was measured on, before the game starts: 41,515 of 73,154 MB in use, the process at 369 MB.
    // 32,008 MB for the game: enough.
    CHECK(!mem_need_short(369, 41515, 73154, need), "short with 32 GB for the game");
    CHECK(mem_need_room(369, 41515, 73154) == 73154 - 41515 + 369, "room %.0f", mem_need_room(369, 41515, 73154));

    // The same machine with the game in play, 65,840 of 73,154 MB in use and 24,000 MB of them the game's: what the
    // game holds counts toward its need, so a check made late does not call a fitting game short.
    CHECK(!mem_need_short(24000, 65840, 73154, need), "short in play although the game's own commit fits");

    // A 32 GB machine with an 8 GB page file and 12 GB in use by other programs: 28 GB free, enough; with 20 GB in
    // use, 20 GB free: short.
    CHECK(!mem_need_short(300, 12000, 40960, need), "short with 29 GB free");
    CHECK(mem_need_short(300, 20000, 40960, need), "enough with 21 GB free");

    // A 16 GB machine with a 4 GB page file never fits.
    CHECK(mem_need_short(300, 6000, 20480, need), "enough on a 16 GB machine");

    // The boundary: exactly the need is enough, one MB less is short.
    CHECK(!mem_need_short(0, 1000, 1000 + need, need), "short with exactly the need free");
    CHECK(mem_need_short(0, 1001, 1000 + need, need), "enough one MB below the need");

    // Not asked (0), and a platform without a known limit (-1): never short. A process reading of -1 (unknown) adds
    // nothing to the room.
    CHECK(!mem_need_short(300, 20000, 20480, 0), "short with the check off");
    CHECK(!mem_need_short(300, 20000, -1, need), "short without a known limit");
    CHECK(mem_need_room(-1, 20000, 40960) == 20960, "an unknown process commit changed the room");

    printf("mem_need_check: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
