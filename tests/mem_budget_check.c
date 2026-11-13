// runtime/mem_budget.h: a side's room as a free share and the level it gives, roomy, normal, tight or critical, with
// the hysteresis that keeps a share hovering at a line from flipping the level. Built and run by
// tests/mem_budget_check.py; prints each failed check and exits 1 on any failure.
#include <stdio.h>

#include "mem_budget.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static MemRoom room_pm(uint32_t pm) {   // a share on a 32 GiB side
    MemRoom r = {pm, 32768ull * pm / 1000};
    return r;
}

int main(void) {
    // the room itself
    MemRoom r = mem_room(24000, 32000);
    CHECK(r.free_pm == 250 && r.free_mb == 8000, "8000 of 32000 free: 250 per mille (%u, %llu)", r.free_pm,
          (unsigned long long)r.free_mb);
    r = mem_room(33000, 32000);
    CHECK(r.free_pm == 0 && r.free_mb == 0, "use past the limit: no room");
    r = mem_room(10, 0);
    CHECK(r.free_pm == 0 && r.free_mb == 0, "no limit known: no room");
    MemRoom a = {300, 9000}, b = {500, 4000};
    r = mem_room_min(a, b);
    CHECK(r.free_pm == 300 && r.free_mb == 4000, "the tighter share and the smaller amount");

    // levels from a first sample (from roomy, as mem_budget.c starts)
    CHECK(mem_level_next(MEM_ROOMY, room_pm(550)) == MEM_ROOMY, "55%% free: roomy");
    CHECK(mem_level_next(MEM_ROOMY, room_pm(400)) == MEM_NORMAL, "40%% free: normal (the line belongs below)");
    CHECK(mem_level_next(MEM_ROOMY, room_pm(201)) == MEM_NORMAL, "20.1%% free: normal");
    CHECK(mem_level_next(MEM_ROOMY, room_pm(200)) == MEM_TIGHT, "20%% free: tight");
    CHECK(mem_level_next(MEM_ROOMY, room_pm(81)) == MEM_TIGHT, "8.1%% free: tight");
    CHECK(mem_level_next(MEM_ROOMY, room_pm(80)) == MEM_CRITICAL, "8%% free: critical");
    CHECK(mem_level_next(MEM_ROOMY, room_pm(0)) == MEM_CRITICAL, "nothing free: critical");
    MemRoom small = {600, 200};   // a large share of a tiny side
    CHECK(mem_level_next(MEM_ROOMY, small) == MEM_CRITICAL, "less than MEM_FLOOR_MB free is critical at any share");

    // down at once, up only past the line by MEM_HYST_PM
    CHECK(mem_level_next(MEM_NORMAL, room_pm(150)) == MEM_TIGHT, "normal to tight at once");
    CHECK(mem_level_next(MEM_NORMAL, room_pm(50)) == MEM_CRITICAL, "normal to critical at once");
    CHECK(mem_level_next(MEM_TIGHT, room_pm(220)) == MEM_TIGHT, "tight stays tight at 22%%");
    CHECK(mem_level_next(MEM_TIGHT, room_pm(250)) == MEM_TIGHT, "tight stays tight at 25%% (the line plus 5 points)");
    CHECK(mem_level_next(MEM_TIGHT, room_pm(251)) == MEM_NORMAL, "tight to normal past 25%%");
    CHECK(mem_level_next(MEM_CRITICAL, room_pm(100)) == MEM_CRITICAL, "critical stays at 10%%");
    CHECK(mem_level_next(MEM_CRITICAL, room_pm(140)) == MEM_TIGHT, "critical to tight past 13%%");
    CHECK(mem_level_next(MEM_CRITICAL, room_pm(700)) == MEM_ROOMY, "critical to roomy in one step when there is room");
    CHECK(mem_level_next(MEM_CRITICAL, room_pm(420)) == MEM_NORMAL, "42%%: past normal's hysteresis, not roomy's");
    CHECK(mem_level_next(MEM_NORMAL, room_pm(430)) == MEM_NORMAL, "normal stays at 43%%");
    CHECK(mem_level_next(MEM_NORMAL, room_pm(451)) == MEM_ROOMY, "normal to roomy past 45%%");
    CHECK(mem_level_next(MEM_ROOMY, room_pm(390)) == MEM_NORMAL, "roomy to normal below 40%%");

    // a share that hovers around the 20% line changes the level once, not at every sample
    int level = MEM_NORMAL, changes = 0;
    for (int i = 0; i < 1000; i++) {
        int n = mem_level_next(level, room_pm(185 + (uint32_t)(i * 7 % 31)));   // 18.5% to 21.5%
        changes += n != level, level = n;
    }
    CHECK(changes == 1 && level == MEM_TIGHT, "hovering at 20%%: one change (%d), ends tight (%d)", changes, level);

    if (fails) printf("mem_budget_check: %d checks failed\n", fails);
    else printf("mem_budget_check: all checks passed\n");
    return fails != 0;
}
