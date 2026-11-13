// par_jobs.c: a small pool of persistent threads for CPU work the drawing threads would otherwise do alone, such as a
// large texture's copy out of the game's memory (tex_deswizzle.c tex_bl_copy_mt). One job at a time; the caller takes
// part and returns when every piece is done.
#pragma once
#include <stddef.h>

// Runs fn(arg, i) for every i in [0, n) on the pool's threads and the caller's, in no set order, and returns once all
// have returned. A second caller while the pool runs a job, a pool that could not start, NX2NAVI_PAR_JOBS=0 or n 1 run
// every piece on the calling thread instead. fn must not call par_run itself (it would run its pieces alone).
void par_run(int n, void (*fn)(void *arg, int i), void *arg);
// The threads a job can spread over, the caller counted; 1 when the pool is off.
int par_threads(void);
// memcpy with large copies (PAR_MEMCPY_MIN and up) split over the pool.
void par_memcpy(void *dst, const void *src, size_t n);
