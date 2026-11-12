// Whether the machine has the memory the game needs, asked once at the start (memory.c mem_start_note). Own header so
// tests/mem_need_check.c checks it alone.
//
// What counts is commit: memory the system has promised to programs, out of RAM plus page file (on Linux the tightest
// of the limits platform.h plat_commit_info reads). An allocation fails when the commit in use reaches the limit,
// however much RAM is free, and on Windows the video driver charges the game's video memory to it too. Measured in
// play (RTX 5090, 3840x2160, three sessions of 19,000 to 50,000 frames on 2026-11-11): this process held 23,910 to
// 24,429 MB of commit, flat over the session, and the system stood at 65,777 to 65,914 of 73,154 MB with the other
// programs' 41,500 MB. A machine that cannot give the game that much pages, drops textures to make room (vk_texture.c
// commit_is_low) or loses the device.
#pragma once

// NX2NAVI_MEM_NEED_MB's default: the commit the game took in the sessions above, as 24 GiB.
#define MEM_NEED_DEFAULT_MB 24576.0

// The commit in MB the game can still get: what is free, and what this process already holds of its need.
static inline double mem_need_room(double proc_mb, double used_mb, double limit_mb) {
    return limit_mb - used_mb + (proc_mb > 0 ? proc_mb : 0);
}

// 1 when the game's need does not fit. need_mb 0 or less: not asked. A limit below 0 is an unknown one: not short.
static inline int mem_need_short(double proc_mb, double used_mb, double limit_mb, double need_mb) {
    if (need_mb <= 0 || limit_mb < 0) return 0;
    return mem_need_room(proc_mb, used_mb, limit_mb) < need_mb;
}
