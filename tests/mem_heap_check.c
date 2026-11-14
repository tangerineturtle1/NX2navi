// runtime/vk_mem_heap.h: when the memory report asks for the C runtime heap's summary, which holds every allocation of
// the game while it runs. Never in play, unless NX2NAVI_MEM_CENSUS asks for it. Built and run by
// tests/mem_heap_check.py; prints each failed check and exits 1 on any.
#include <stdio.h>

#include "vk_mem_heap.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// A session as the report sees it: a report every 3600 presents, play from `play_at` on; the summary takes `ms`
// milliseconds each time and counts as slow past 2 ms (vk_memory.c HEAP_SLOW_NS). Returns the summaries taken in play,
// and through *before those before it.
static int session(int census, int reports, int play_at, double ms, int *before) {
    int slow = 0, in_play_n = 0;
    *before = 0;
    for (int r = 1; r <= reports; r++) {
        int in_play = r * 3600 >= play_at;
        if (!mem_heap_summary_due(census, in_play, slow)) continue;
        if (in_play) in_play_n++;
        else ++*before;
        if (ms > 2.0 && !census) slow = 1;   // as system_log notes it
    }
    return in_play_n;
}

int main(void) {
    // ---- the rule
    CHECK(mem_heap_summary_due(0, 0, 0), "before play the summary is not asked for");
    CHECK(!mem_heap_summary_due(0, 1, 0), "the summary is asked for in play");
    CHECK(!mem_heap_summary_due(0, 0, 1) && !mem_heap_summary_due(0, 1, 1), "a slow summary is asked for again");
    CHECK(mem_heap_summary_due(1, 1, 0) && mem_heap_summary_due(1, 1, 1) && mem_heap_summary_due(1, 0, 1),
          "the census switch does not ask at every report");

    int before;
    // ---- the sessions of 2026-11-14: play from the first seconds on, the first report at present 3600 in play, the
    // summary 54 and 70 ms. The rule before asked there once (the hitch); now it asks never.
    CHECK(session(0, 6, 2400, 70.0, &before) == 0 && before == 0, "a session that plays before its first report asks");
    // ---- a long title screen: two reports before play, each fast there, then play: asked twice, both before play
    CHECK(session(0, 10, 9000, 0.4, &before) == 0 && before == 2, "a title screen of two reports: %d before play", before);
    // ---- a slow summary before play (a heap that is large already at the title screen): once, and no more
    CHECK(session(0, 10, 20000, 30.0, &before) == 0 && before == 1, "a slow summary before play: %d taken", before);
    // ---- with the census switch every report asks, in play too, slow or not
    CHECK(session(1, 6, 2400, 70.0, &before) == 6 && before == 0, "the census switch: not every report in play asks");
    CHECK(session(1, 6, 9000, 70.0, &before) == 4 && before == 2, "the census switch across a title screen");

    if (fails) printf("%d checks failed\n", fails);
    else printf("mem_heap_check: ok\n");
    return fails != 0;
}
