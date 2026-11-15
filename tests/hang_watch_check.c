// runtime/hang_watch.c's decisions on made-up readings, a second apart, with stand-ins for what it reads and calls.
// Built and run by tests/hang_watch_check.py; prints each failed check and exits 1 on any.
#include "hang_watch.c"

#include <stdarg.h>
#include <stdio.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the runtime hang_watch.c reaches
GfxStats gfx_counters;
uint64_t rt_guest_start_ns = 1;
static int focused = 1, minimized, ending, flushes, syncs, wait_reports;
static uint64_t issued_v, done_v;
void vk_sync_progress(uint64_t *issued, uint64_t *done) { *issued = issued_v, *done = done_v; }
int gfx_window_in_front(void) { return focused && !minimized; }
int rt_exit_started(void) { return ending; }
int64_t frame_step_world_us(void) { return 1000000; }
void gfx_lock_holders(GfxLockHolders *h) { *h = (GfxLockHolders){7, 1, 0, 0, 0}; }
void kern_wait_report(void) { wait_reports++; }
void logq_flush(void) { flushes++; }
void logq_sync(void) { syncs++; }
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("  log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
int cfg_int(const char *name) { (void)name; return 20; }

// ---- the readings: t seconds, the frames shown so far, focus, and the GPU's issued and done values
static HangWatch w;
static uint64_t t_s;
static int run(int seconds, int frames_move, int gpu_move) {
    int reports = 0;
    for (int i = 0; i < seconds; i++) {
        t_s++;
        if (frames_move) gfx_counters.frames++;
        if (gpu_move) issued_v++, done_v++;
        HwBeat b;
        hw_read(&b);
        b.now_ns = t_s * 1000000000ull;
        int kind = hw_step(&w, &b);
        if (kind) {
            hw_report(kind, &b, &w);
            reports++;
        }
    }
    return reports;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    w = (HangWatch){.limit_ns = 20000000000ull};
    CHECK(!run(100, 1, 1), "a report while frames and the GPU move");
    focused = 0;
    CHECK(!run(100, 0, 0), "a report while the window is not in front");
    focused = 1, minimized = 1;
    CHECK(!run(100, 0, 0), "a report while the window is minimized");
    minimized = 0, ending = 1;
    CHECK(!run(100, 0, 0), "a report while the process ends");
    ending = 0;
    int r = run(19, 0, 0);
    CHECK(!r, "a report before 20 s without a frame in front");
    r = run(60, 0, 0);
    CHECK(r == 1, "%d reports for one stall of the frames, want 1", r);
    CHECK(flushes == 1 && wait_reports == 1 && !syncs, "the report wrote %d flushes, %d wait lists and %d syncs", flushes,
          wait_reports, syncs);
    r = run(5, 1, 1) + run(30, 0, 0);
    CHECK(r == 1, "%d reports for a second stall after progress, want 1", r);
    focused = 0;   // the GPU stops: reported whatever the focus
    issued_v += 3;
    r = run(30, 0, 0);
    CHECK(r == 1, "%d reports for submissions not done, want 1", r);
    issued_v = done_v;
    r = run(5, 1, 1);
    focused = 1;
    r += run(60, 0, 0);
    CHECK(r == 0, "%d reports after the third: at most 3 a run", r);
    CHECK(w.reports == 3, "%d reports in all, want 3", w.reports);
    // a fresh watch: the GPU idle (nothing outstanding) is no stall
    w = (HangWatch){.limit_ns = 20000000000ull};
    focused = 0;
    issued_v = done_v;
    CHECK(!run(100, 0, 0), "a report for an idle GPU");
    printf("hang_watch_check: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
