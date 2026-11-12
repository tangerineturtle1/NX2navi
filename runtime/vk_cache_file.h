// Reads pipeline-cache.bin into memory for cache_load (runtime/vk_pipeline.c), with 64-bit sizes on every platform.
// ftell returns a long, which is 32 bits on Windows: a file past 2 GB measured as -1 there, and the run started with an
// empty cache without a word in the log. The length comes from the 64-bit tell, the read runs in pieces no C library
// call has to take whole, and every refusal names its reason and the file's size, so cache_load can log it.
// tests/pipeline_cache_file_check.py reads a sparse file just over 2 GB through it.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

enum {
    CACHE_FILE_OK,
    CACHE_FILE_NONE,       // no such file: a first run
    CACHE_FILE_UNSIZED,    // the file's length could not be measured
    CACHE_FILE_SHORT,      // not longer than the header every cache starts with
    CACHE_FILE_OVER_CAP,   // longer than the limit; not read
    CACHE_FILE_NO_MEMORY,  // no block of the file's size
    CACHE_FILE_READ_FAIL,  // the read ended early
};

#define CACHE_FILE_PIECE ((size_t)64 << 20)

static inline const char *cache_file_why(int why) {
    switch (why) {
    case CACHE_FILE_UNSIZED: return "its size cannot be measured";
    case CACHE_FILE_SHORT: return "it is shorter than a pipeline cache header";
    case CACHE_FILE_OVER_CAP: return "it is over the size limit";
    case CACHE_FILE_NO_MEMORY: return "there is no memory for a block of its size";
    case CACHE_FILE_READ_FAIL: return "reading it ended early";
    default: return "";
    }
}

// The whole file in a malloc block, or NULL with *why set. *len is the file's length in bytes whenever it could be
// measured, read or not. min: a file of at most this many bytes is refused; cap: so is one longer than cap (0: no limit),
// before any of it is read.
static inline void *cache_file_read(const char *path, uint64_t min, uint64_t cap, uint64_t *len, int *why) {
    *len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return *why = CACHE_FILE_NONE, NULL;
#ifdef _WIN32
    int64_t n = _fseeki64(f, 0, SEEK_END) ? -1 : _ftelli64(f);
    if (n >= 0 && _fseeki64(f, 0, SEEK_SET)) n = -1;
#else
    int64_t n = fseeko(f, 0, SEEK_END) ? -1 : (int64_t)ftello(f);
    if (n >= 0 && fseeko(f, 0, SEEK_SET)) n = -1;
#endif
    void *data = NULL;
    if (n < 0) *why = CACHE_FILE_UNSIZED;
    else if (*len = (uint64_t)n, (uint64_t)n <= min) *why = CACHE_FILE_SHORT;
    else if (cap && (uint64_t)n > cap) *why = CACHE_FILE_OVER_CAP;
    else if ((uint64_t)n > SIZE_MAX || !(data = malloc((size_t)n))) *why = CACHE_FILE_NO_MEMORY;
    else {
        size_t got = 0, want = (size_t)n;
        while (got < want) {
            size_t piece = want - got < CACHE_FILE_PIECE ? want - got : CACHE_FILE_PIECE;
            size_t r = fread((char *)data + got, 1, piece, f);
            got += r;
            if (r != piece) break;
        }
        if (got == want) *why = CACHE_FILE_OK;
        else free(data), data = NULL, *why = CACHE_FILE_READ_FAIL;
    }
    fclose(f);
    return data;
}
