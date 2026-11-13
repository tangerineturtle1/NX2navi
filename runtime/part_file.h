// A file written piece by piece on a thread nobody waits for when the process ends.
//
// The window's close ends the process with plat_exit while the detached threads still run (gfx_present.c
// gfx_window_closed), and a kill or a crash ends it anywhere. A file such a thread was in the middle of then stays
// cut short under its own name, and whoever opens it later takes it for a whole one. So the file is written as
// <path>.part and gets its name with one rename once it is complete: a reader finds the whole file or none, and a
// .part file says what it is. A file made in memory first goes through plat_file_write_cached or
// plat_file_write_atomic instead, which do the same in one call.
#pragma once

#include <stdio.h>

#include "platform/platform.h"

#define PART_FILE_SUFFIX ".part"

// <path>.part into out; 0 when it does not fit.
static inline int part_file_name(const char *path, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s" PART_FILE_SUFFIX, path);
    return n > 0 && (size_t)n < cap;
}

// Opens <path>.part for writing (mode: "w" or "wb"). NULL when it cannot be made.
static inline FILE *part_file_open(const char *path, const char *mode) {
    char part[PLAT_PATH_MAX + 64];
    return part_file_name(path, part, sizeof part) ? fopen(part, mode) : NULL;
}

// Closes f, the file part_file_open(path) gave, and renames it to path, over an older file of that name. With ok 0,
// after a write error on f or when the close or the rename fails, the part is deleted and path stays as it was.
// 1 when path now holds the whole new file.
static inline int part_file_close(FILE *f, const char *path, int ok) {
    char part[PLAT_PATH_MAX + 64];
    if (!f) return 0;
    ok = ok && !ferror(f);
    ok = !fclose(f) && ok;
    if (!part_file_name(path, part, sizeof part)) return 0;
    if (ok && plat_file_replace(part, path, 0)) return 1;
    plat_file_delete(part);
    return 0;
}
