// Whether a start does the whole warm on the boot screen (vk_pipeline.c bf_decide, where the reasons are). Own header
// so tests/boot_warm_check.c checks it alone.
//
// <cache folder>/boot-warm-owed.txt is "<GPU and driver> <starts that began the warm>" or "<GPU and driver> done".
// The start that finds the pipeline cache written by another device or driver writes it with 1; a start that finds
// its own GPU and driver in it with a count goes on with the warm and counts up; the start whose warm ends writes
// "done". The pipeline cache's file keeps the earlier driver's header for some starts after that (what a run compiles
// goes to the play cache until that is folded in), and "done" is what keeps those starts ordinary ones. After
// BOOT_WARM_TRIES starts that began the warm and did not end it the file stays as it is, and starts on that GPU and
// driver are ordinary ones too.
//
// boot_end_decide: what a marker means that a start left behind while the boot built pipelines
// (<cache folder>/pipeline-prewarm-running.txt, "<GPU and driver> <ends in a row before it>"; vk_pipeline.c
// pm_skipped). With the crash handler's note the driver crashed in those builds and the prebuild is skipped on that
// GPU and driver. Without it the start was closed or stopped, which only counts; BOOT_QUIET_ENDS such ends in a row
// skip the prebuild as well, for the driver that ends the process without a fault the handler sees.
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BOOT_WARM_TRIES 3

// NO: nothing owed (a file there is another GPU's or driver's, or not one: the caller deletes it). YES: this start
// does the warm. GAVE_UP and DONE: an ordinary start, and the file stays.
enum { BOOT_WARM_NO, BOOT_WARM_YES, BOOT_WARM_GAVE_UP, BOOT_WARM_DONE };

// text: the file's content, NULL without the file. key: this GPU and driver, without spaces. foreign: the pipeline
// cache was written by another device or driver. *tries: the count to write with BOOT_WARM_YES, 0 otherwise.
static inline int boot_warm_decide(const char *text, const char *key, int foreign, int *tries) {
    char was[64] = "", tok[16] = "", *end = tok;
    long n = -1;
    *tries = 0;
    if (text && sscanf(text, "%63s %15s", was, tok) == 2 && !strcmp(was, key)) {
        if (!strcmp(tok, "done")) return BOOT_WARM_DONE;
        n = strtol(tok, &end, 10);
        if (end == tok || *end || n < 0) n = -1;
    }
    if (n < 0 && !foreign) return BOOT_WARM_NO;
    if (n < 0) n = 0;                             // this GPU and driver's first start
    if (n >= BOOT_WARM_TRIES) return BOOT_WARM_GAVE_UP;
    *tries = (int)n + 1;
    return BOOT_WARM_YES;
}

#define BOOT_QUIET_ENDS 3

// OTHER: the marker is another GPU's or driver's and means nothing here. SKIP: the prebuild is skipped on this GPU
// and driver from now on. COUNTED: this start builds again, and writes *quiet into its own marker.
enum { BOOT_END_OTHER, BOOT_END_SKIP, BOOT_END_COUNTED };

// text: the marker's content (a marker of 0.9.2 holds the GPU and driver alone). crashed: the crash handler's note was
// there. *quiet: the ends in a row without a note, this one counted; 0 with BOOT_END_OTHER.
static inline int boot_end_decide(const char *text, const char *key, int crashed, int *quiet) {
    char was[64] = "";
    int n = 0;
    if (sscanf(text, "%63s %d", was, &n) < 2 || n < 0) n = 0;
    *quiet = 0;
    if (strcmp(was, key)) return BOOT_END_OTHER;
    *quiet = n + 1;
    return crashed || n + 1 >= BOOT_QUIET_ENDS ? BOOT_END_SKIP : BOOT_END_COUNTED;
}
