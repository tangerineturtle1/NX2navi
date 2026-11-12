// runtime/vk_save_retry.h: when the autosave thread saves the pipeline caches and when it tries again after a save
// that did not happen (runtime/vk_pipeline.c autosave_once). Built and run by tests/save_retry_check.py; prints each
// failed check and exits 1 on any.
#include <stdio.h>

#include "vk_save_retry.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void) {
    uint64_t t = 1000000;   // ms since the machine started, as the thread's clock gives them

    // Nothing built, nothing owed: no save, however long the run.
    SaveRetry s = {0};
    CHECK(!save_due(&s, 0, t, 0), "a save with nothing new in the caches");
    CHECK(!save_due(&s, 0, t + 3600000, 0), "a save after an hour with nothing new");

    // A new pipeline: a save, and after it none until the next one; never two within the interval.
    CHECK(save_due(&s, 1, t, 0), "no save for a new pipeline");
    CHECK(!save_tried(&s, 1, t, 1), "a good save counted as the last try");
    CHECK(!save_due(&s, 2, t + SAVE_EVERY_MS - 1, 0), "two saves within the interval");
    CHECK(save_due(&s, 2, t + SAVE_EVERY_MS, 0), "no save for the next pipeline once the interval passed");
    save_tried(&s, 2, t + SAVE_EVERY_MS, 1);
    CHECK(!save_due(&s, 2, t + 10 * SAVE_EVERY_MS, 0), "a save again with nothing new after a good one");

    // A save that did not happen is tried again at the interval with nothing new built, SAVE_RETRIES times; the
    // last of them says so, and then nothing is tried until a new pipeline.
    s = (SaveRetry){0};
    uint64_t now = t;
    CHECK(save_due(&s, 5, now, 0), "no first save");
    CHECK(!save_tried(&s, 5, now, 0), "the first failed try counted as the last");
    int tries = 0, last = 0;
    for (int i = 0; i < 100 && !last; i++) {
        CHECK(!save_due(&s, 5, now + SAVE_EVERY_MS - 1, 0), "a retry within the interval");
        now += SAVE_EVERY_MS;
        if (!save_due(&s, 5, now, 0)) break;
        tries++;
        last = save_tried(&s, 5, now, 0);
    }
    CHECK(tries == SAVE_RETRIES && last, "%d retries after a failed save, %d wanted, the last one said: %d", tries,
          SAVE_RETRIES, last);
    CHECK(!save_due(&s, 5, now + 100 * SAVE_EVERY_MS, 0), "a try after the last of the row with nothing new");
    CHECK(save_due(&s, 6, now + SAVE_EVERY_MS, 0), "no try for a new pipeline after the row ended");
    CHECK(!save_tried(&s, 6, now + SAVE_EVERY_MS, 0), "new content did not start a row of its own");
    CHECK(save_due(&s, 6, now + 2 * SAVE_EVERY_MS, 0), "no retry in the new row");

    // A retry that saves ends the row: nothing more until something new.
    s = (SaveRetry){0};
    save_tried(&s, 3, t, 0);
    CHECK(save_due(&s, 3, t + SAVE_EVERY_MS, 0), "no retry after one failed save");
    CHECK(!save_tried(&s, 3, t + SAVE_EVERY_MS, 1), "a good retry counted as the last try");
    CHECK(!save_due(&s, 3, t + 2 * SAVE_EVERY_MS, 0), "a save again after a good retry");

    // A save owed from the start (the play file folded in) is tried with nothing built, and bounded the same way.
    s = (SaveRetry){0};
    CHECK(save_due(&s, 0, t, 1), "no save for the one owed from the start");
    now = t, tries = 1;
    save_tried(&s, 0, now, 0);
    while (tries < 100 && save_due(&s, 0, now += SAVE_EVERY_MS, 1)) tries++, save_tried(&s, 0, now, 0);
    CHECK(tries == SAVE_RETRIES + 1, "%d tries of an owed save that keeps failing, %d wanted", tries, SAVE_RETRIES + 1);

    printf("save_retry_check: %s\n", fails ? "FAILED" : "passed");
    return fails ? 1 : 0;
}
