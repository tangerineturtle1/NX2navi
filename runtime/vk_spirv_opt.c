// Optional optimization of final Vulkan modules, after binding and capability fixups.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk_spirv_opt.h"

typedef int (*Optimize)(const uint32_t *, size_t, void *(*)(size_t), uint32_t **, size_t *);
static Optimize optimize;
static PlatMutex init_lock = PLAT_MUTEX_INIT;
static int initialized;
static char cache_dir[PLAT_PATH_MAX * 2];

static uint64_t hash_bytes(const void *data, size_t bytes) {
    const uint8_t *p = data;
    uint64_t h = 0xcbf29ce484222325ull;
    while (bytes--) h = (h ^ *p++) * 0x100000001b3ull;
    return h;
}

static void initialize(void) {
    plat_mutex_lock(&init_lock);
    if (!initialized) {
        initialized = 1;
        const char *on = cfg_get("NX2NAVI_SPIRV_OPT");
        char path[PLAT_PATH_MAX * 2];
        if (on && !strcmp(on, "1") && plat_exe_path(path, sizeof path)) {
            char *cut = plat_last_sep(path);
            if (cut) {
                *cut = 0;
                snprintf(cache_dir, sizeof cache_dir, "%s/spirv-opt-363-perf-v1", path);
#ifdef _WIN32
                const char *name = "nx2navi_spirv_opt.dll";
#else
                const char *name = "libnx2navi_spirv_opt.so";
#endif
                char library[PLAT_PATH_MAX * 2];
                snprintf(library, sizeof library, "%s/%s", path, name);
                void *lib = plat_lib_open(library);
                optimize = lib ? (Optimize)plat_lib_sym(lib, "nx2navi_spirv_opt_v1") : NULL;
                if (optimize) plat_file_mkdir_p(cache_dir);
                rt_log("spirv opt: %s; preserves bindings and specialization constants", optimize ? "enabled" : "helper missing, using original modules");
            }
        }
    }
    plat_mutex_unlock(&init_lock);
}

// Disk records hold the original bytes as well as a checksum of the result. A hash collision or a corrupt record
// therefore cannot substitute a different module. Files are published by rename, never written over in place.
uint32_t *vk_spirv_optimize(const uint32_t *input, uint64_t words, uint64_t *output_words) {
    initialize();
    if (!optimize || words < 4000 || words > SIZE_MAX / 8) return NULL;
    size_t bytes = (size_t)words * 4;
    uint64_t hash = hash_bytes(input, bytes);
    char path[PLAT_PATH_MAX * 2 + 80];
    snprintf(path, sizeof path, "%s/%016llx.bin", cache_dir, (unsigned long long)hash);
    uint64_t size = 0;
    uint8_t *record = plat_file_read(path, &size);
    if (record && size >= 40 && size >= 40 + bytes) {
        uint64_t h[5]; memcpy(h, record, sizeof h);
        uint64_t available = (size - 40 - bytes) / 4;
        if (h[0] == 0x4e584f5054433031ull && h[1] == words && h[2] >= 5 && h[2] == available &&
            size == 40 + bytes + h[2] * 4 && h[3] == hash && !memcmp(record + 40, input, bytes) &&
            hash_bytes(record + 40 + bytes, (size_t)h[2] * 4) == h[4]) {
            uint32_t *copy = malloc((size_t)h[2] * 4);
            if (copy) {
                memcpy(copy, record + 40 + bytes, (size_t)h[2] * 4);
                if (copy[0] == 0x07230203) {
                    *output_words = h[2]; free(record); return copy;
                }
                free(copy);
            }
        }
    }
    free(record);
    uint32_t *result = NULL;
    size_t n = 0;
    uint64_t t0 = plat_now_ns();
    if (!optimize(input, (size_t)words, malloc, &result, &n) || !result || n < 5 || result[0] != 0x07230203) {
        free(result);
        rt_log("spirv opt: %016llx declined; using original module", (unsigned long long)hash);
        return NULL;
    }
    uint64_t h[5] = {0x4e584f5054433031ull, words, n, hash, hash_bytes(result, n * 4)};
    if (n <= (SIZE_MAX - sizeof h - bytes) / 4) {
        size_t record_size = sizeof h + bytes + n * 4;
        uint8_t *data = malloc(record_size);
        if (data) {
            memcpy(data, h, sizeof h);
            memcpy(data + sizeof h, input, bytes);
            memcpy(data + sizeof h + bytes, result, n * 4);
            plat_file_write_cached(path, data, record_size);
            free(data);
        }
    }
    rt_log("spirv opt: %016llx: %llu -> %zu words in %.1f ms", (unsigned long long)hash,
           (unsigned long long)words, n, (plat_now_ns() - t0) / 1e6);
    *output_words = n;
    return result;
}
