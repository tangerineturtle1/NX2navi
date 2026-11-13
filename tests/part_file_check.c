// runtime/part_file.h: a file written piece by piece gets its name only when it is complete. Built with the platform
// layer and run by tests/part_file_check.py.
//
//   part_file_check <folder>         the checks below, in <folder>
//   part_file_check <folder> hang    writes half of <folder>/killed.tsv, prints "writing" and sleeps: the Python side
//                                    kills it there, as a closed game ends the thread that writes a dump
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "part_file.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

void rt_log(const char *fmt, ...) {   // the platform layer's lines
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

// The text of the file at path ("" when it holds none), or NULL when there is no such file.
static char *text_of(const char *path) {
    if (!plat_file_exists(path)) return NULL;
    uint64_t size = 0;
    char *t = (char *)plat_file_read(path, &size);
    return t ? t : calloc(1, 1);
}

static int holds(const char *path, const char *want) {
    char *t = text_of(path);
    int same = t && !strcmp(t, want);
    free(t);
    return same;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    char path[PLAT_PATH_MAX], part[PLAT_PATH_MAX + 64];
    if (argc > 2 && !strcmp(argv[2], "hang")) {
        snprintf(path, sizeof path, "%s/killed.tsv", argv[1]);
        FILE *f = part_file_open(path, "wb");
        if (!f) return 3;
        fputs("the first half of the new file\n", f);
        fflush(f);
        puts("writing");
        fflush(stdout);
        for (;;) plat_sleep_ms(1000);
    }

    // a new file: nothing under its name while it is written, the whole file after
    snprintf(path, sizeof path, "%s/new.tsv", argv[1]);
    CHECK(part_file_name(path, part, sizeof part), "the part name fits");
    FILE *f = part_file_open(path, "wb");
    CHECK(f != NULL, "the part file opens");
    if (!f) return 1;
    fputs("line 1\n", f);
    fflush(f);
    CHECK(!plat_file_exists(path), "no file under the name while it is written");
    CHECK(holds(part, "line 1\n"), "what was written so far is in the part file");
    fputs("line 2\n", f);
    CHECK(part_file_close(f, path, 1) == 1, "the complete file gets its name");
    CHECK(holds(path, "line 1\nline 2\n"), "the file holds everything written");
    CHECK(!plat_file_exists(part), "no part file stays");

    // over an older file: the old one stays whole until the new one is complete
    f = part_file_open(path, "wb");
    CHECK(f != NULL, "the part file opens beside the older file");
    if (!f) return 1;
    fputs("the new text\n", f);
    fflush(f);
    CHECK(holds(path, "line 1\nline 2\n"), "the older file is untouched while the new one is written");
    CHECK(part_file_close(f, path, 1) == 1, "the new file replaces the older one");
    CHECK(holds(path, "the new text\n"), "the name holds the new file");

    // given up (the writer met an error): the older file stays, the part goes
    f = part_file_open(path, "wb");
    if (!f) return 1;
    fputs("half of a third", f);
    CHECK(part_file_close(f, path, 0) == 0, "a file given up is not put in place");
    CHECK(holds(path, "the new text\n"), "the older file stays after a write that was given up");
    CHECK(!plat_file_exists(part), "the part of a write that was given up is deleted");

    // a folder that is not there: nothing opens, and closing nothing is no success
    snprintf(path, sizeof path, "%s/no such folder/x.tsv", argv[1]);
    f = part_file_open(path, "wb");
    CHECK(f == NULL, "no part file in a folder that does not exist");
    CHECK(part_file_close(f, path, 1) == 0, "closing a file that never opened reports failure");

    // a name that does not fit the path buffers
    static char longp[PLAT_PATH_MAX + 200];
    memset(longp, 'a', sizeof longp - 1);
    CHECK(part_file_open(longp, "wb") == NULL, "a path too long for its part name is refused");

    printf(fails ? "part_file_check: %d FAILED\n" : "part_file_check: all checks passed\n", fails);
    return fails != 0;
}
