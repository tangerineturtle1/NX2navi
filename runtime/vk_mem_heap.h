// When the memory report asks for the C runtime heap's summary (vk_memory.c system_log, plat_heap_info). Header only,
// so that tests/mem_heap_check.c checks the rule without a device.
//
// The summary holds the heap's lock while it adds the heap up, and every malloc and free of every thread waits for
// it. With a heap of a few GiB that took 54 and 70 ms in two sessions (2026-11-14, RTX 5090 PC), and the frame of the
// first report, 30 seconds into each session, took 55 to 82 ms: the longest frame of play in both. One slow summary
// switched the question off for the rest of the run, so each session paid for it exactly once.
//
// The summary is therefore not asked for in play at all: only before play (the boot and the title screen, where a
// long frame is not seen and the heap is small), and no more once one was slow there. NX2NAVI_MEM_CENSUS=1, the
// switch that already accepts a late frame for the walk of the address space, asks for it at every report. In play
// the report gives what the runtime knows it allocated instead, the same figure from one report to the next, which is
// what the growth bookkeeping compares.
#pragma once

// census: NX2NAVI_MEM_CENSUS is on; in_play: a save's first frame was shown; slow: a summary took too long before.
static inline int mem_heap_summary_due(int census, int in_play, int slow) { return census || (!in_play && !slow); }
