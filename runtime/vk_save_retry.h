// When the autosave thread saves the pipeline caches (vk_pipeline.c autosave_once), and when it tries again after a
// save that did not happen. Own header so tests/save_retry_check.c checks it without Vulkan.
//
// A save is due when something new entered the caches since the last try. A try that did not save (no copy of the
// data for want of memory, a file that could not be written, a build that began between the look and the copy) used to
// be the end of it: the next save waited for the next new pipeline, and a run that built none, then ended without its
// orderly exit, lost what it had compiled since the last file. Now the try is repeated at the save interval, up to
// SAVE_RETRIES times in a row, so a folder that stays unwritable costs that many log lines and no more until a new
// pipeline is built. The same count bounds the save that is pending from the start (the play file folded into the
// main one, `pending`), which before was tried at every interval for as long as it failed.
#pragma once

#include <stdint.h>

#define SAVE_EVERY_MS 20000u   // between two tries
#define SAVE_RETRIES 6         // tries in a row after one that did not save

typedef struct {
    uint64_t n;        // what had entered the caches at the last try
    uint64_t at_ms;    // when that try was made
    uint32_t failed;   // tries in a row that did not save
} SaveRetry;

// 1 when a save is to be tried now. n: the count of what entered the caches so far (it only grows); pending: a save
// is owed whatever n says.
static inline int save_due(const SaveRetry *s, uint64_t n, uint64_t now_ms, int pending) {
    if (now_ms - s->at_ms < SAVE_EVERY_MS) return 0;
    if (n != s->n) return 1;
    return (pending || s->failed) && s->failed <= SAVE_RETRIES;
}

// A try was made at now_ms with n in the caches; saved: the files hold the caches' data now. Returns 1 when this was
// the last try in a row (the caller says so once): the next is made for new content.
static inline int save_tried(SaveRetry *s, uint64_t n, uint64_t now_ms, int saved) {
    if (n != s->n) s->failed = 0;   // new content starts a row of its own
    s->n = n, s->at_ms = now_ms;
    s->failed = saved ? 0 : s->failed + 1;
    return s->failed == SAVE_RETRIES + 1;
}
