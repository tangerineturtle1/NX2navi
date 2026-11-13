// How much room the card and the system have, as one level per side (mem_budget.c): roomy, normal, tight or critical.
// Owners of memory that can be given back read the level and give back earlier when room is short; what they keep
// while there is room is theirs to decide. Header-only part: the arithmetic, which tests/mem_budget_check.c runs.
//
// A side's room is its free share in per mille: for the video memory the driver's budget less this process's use of
// the device-local heaps (vk_mem_vram); for the system the lower of the commit's headroom (the commit limit less the
// system's commit in use: an allocation fails at the limit) and the physical memory free (plat_commit_info,
// plat_mem_info). The levels begin at fixed shares going down, and a side climbs back to a better level only once its
// share is MEM_HYST_PM above the line it fell through, so a share that hovers at a line does not flip the level at
// every sample. Below MEM_FLOOR_MB free a side is critical whatever its share.
#pragma once
#include <stdint.h>

enum { MEM_ROOMY, MEM_NORMAL, MEM_TIGHT, MEM_CRITICAL, MEM_LEVELS };
enum { MEM_VRAM, MEM_RAM, MEM_SIDES };

// The free share (per mille) above which a level holds, from the best down: roomy above 400 (40% free), normal above
// 200, tight above 80; critical at 80 or less. The 20% line is where vk_texture.c's VRAM pressure rule
// (tex_vram_pressure, 80% of the budget in use) starts to act on released textures.
static const uint32_t mem_level_line[MEM_LEVELS - 1] = {400, 200, 80};
#define MEM_HYST_PM 50       // a level is left for a better one 5 points above the line it fell through
#define MEM_FLOOR_MB 256     // less free than this is critical at any share

static const char *const mem_level_name[MEM_LEVELS] = {"roomy", "normal", "tight", "critical"};

typedef struct {
    uint32_t free_pm;   // free share of the limit, per mille
    uint64_t free_mb;   // free MiB
} MemRoom;

// The room of a side with used of limit in use (any one unit, here MiB); none when used reaches the limit.
static inline MemRoom mem_room(uint64_t used, uint64_t limit) {
    MemRoom r = {0, 0};
    if (!limit || used >= limit) return r;
    r.free_mb = limit - used;
    r.free_pm = (uint32_t)(r.free_mb * 1000 / limit);
    return r;
}

// The tighter of two measures of one side: the lower share, and the lower free amount.
static inline MemRoom mem_room_min(MemRoom a, MemRoom b) {
    MemRoom r = {a.free_pm < b.free_pm ? a.free_pm : b.free_pm, a.free_mb < b.free_mb ? a.free_mb : b.free_mb};
    return r;
}

// The level a side moves to from `level` at room r: down to a worse level at once, up only past the hysteresis.
static inline int mem_level_next(int level, MemRoom r) {
    int raw = 0;
    while (raw < MEM_LEVELS - 1 && r.free_pm <= mem_level_line[raw]) raw++;
    if (r.free_mb < MEM_FLOOR_MB) raw = MEM_CRITICAL;
    if (raw >= level) return raw;
    int n = level;   // climb one level at a time while the share clears the line above with room to spare
    while (n > raw && r.free_pm > mem_level_line[n - 1] + MEM_HYST_PM) n--;
    return n;
}

// ---- mem_budget.c: the levels, sampled every MEM_BUDGET_PERIOD_MS on a thread of their own, off the drawing threads.
// Each change of a side's level is logged with its figures. MEM_BUDGET_TEST_VRAM_MB and MEM_BUDGET_TEST_RAM_MB (0:
// off) pretend a side is that small, for testing what gives memory back on a large machine: the video memory budget
// becomes at most that many MiB against this process's use, and the system's room becomes that many MiB less this
// process's commit. They change the levels only, never the texture cache's own rules.
#define MEM_BUDGET_PERIOD_MS 2000
void mem_budget_vram(int on);      // vk_device.c: 1 once the device is up (the first call starts the thread), 0 before
                                   // it goes (waits for a sample under way)
int mem_budget_level(int side);    // MEM_ROOMY .. MEM_CRITICAL of MEM_VRAM or MEM_RAM; MEM_NORMAL before its first sample
void mem_budget_text(char *out, unsigned cap);   // "video memory normal (31.2% free), system memory roomy (55.0% free)"
