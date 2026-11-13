// Exercise the real cache and fallback logic with an in-memory file and helper.
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../runtime/vk_spirv_opt.c"

static const char *setting = "1";
static int helper_available = 1, helper_fails, helper_calls;
static uint8_t *stored;
static size_t stored_size;

static int fake_optimize(const uint32_t *in, size_t words, void *(*allocate)(size_t), uint32_t **out, size_t *n) {
    (void)in; (void)words;
    helper_calls++;
    if (helper_fails) return 0;
    *n = 5;
    *out = allocate(*n * sizeof **out);
    assert(*out);
    (*out)[0] = 0x07230203;
    for (size_t i = 1; i < *n; i++) (*out)[i] = (uint32_t)i;
    return 1;
}
const char *cfg_get(const char *name) { (void)name; return setting; }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
int plat_exe_path(char *out, size_t cap) { return snprintf(out, cap, "/test/ocarina") < (int)cap; }
void *plat_lib_open(const char *path) { (void)path; return helper_available ? &helper_available : NULL; }
void *plat_lib_sym(void *lib, const char *name) { (void)lib; (void)name; return (void *)fake_optimize; }
void plat_file_mkdir_p(const char *path) { (void)path; }
uint8_t *plat_file_read(const char *path, uint64_t *size) {
    (void)path;
    if (!stored) return NULL;
    uint8_t *copy = malloc(stored_size);
    assert(copy);
    memcpy(copy, stored, stored_size);
    *size = stored_size;
    return copy;
}
int plat_file_write_cached(const char *path, const void *data, size_t n) {
    (void)path;
    free(stored);
    stored = malloc(n);
    assert(stored);
    memcpy(stored, data, n);
    stored_size = n;
    return 1;
}
uint64_t plat_now_ns(void) { return 0; }
void rt_log(const char *fmt, ...) { (void)fmt; }
static void reset_init(void) { initialized = 0; optimize = NULL; }

int main(void) {
    uint32_t input[4000] = {0x07230203};
    uint64_t n = 0;
    setting = NULL;
    assert(!vk_spirv_optimize(input, 4000, &n) && !helper_calls);
    setting = "1"; helper_available = 0; reset_init();
    assert(!vk_spirv_optimize(input, 4000, &n) && !helper_calls);
    helper_available = 1; reset_init();
    assert(!vk_spirv_optimize(input, 3999, &n) && !helper_calls);
    uint32_t *out = vk_spirv_optimize(input, 4000, &n);
    assert(out && n == 5 && helper_calls == 1 && stored);
    free(out);
    out = vk_spirv_optimize(input, 4000, &n);
    assert(out && n == 5 && helper_calls == 1); // cache hit
    free(out);
    stored[stored_size - 1] ^= 1;
    out = vk_spirv_optimize(input, 4000, &n);
    assert(out && helper_calls == 2); // damaged cached result is rebuilt
    free(out);
    input[1] = 1;
    out = vk_spirv_optimize(input, 4000, &n);
    assert(out && helper_calls == 3); // different input cannot use the record
    free(out);
    stored[stored_size - 1] ^= 1;
    helper_fails = 1;
    assert(!vk_spirv_optimize(input, 4000, &n) && helper_calls == 4);
    free(stored);
    puts("SPIR-V optimizer cache: fallback, hit and corruption checks passed");
    return 0;
}
