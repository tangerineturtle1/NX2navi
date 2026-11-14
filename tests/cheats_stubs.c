// Services outside the policies under test must never be called by cheats_check.
#include <stdlib.h>
#include <string.h>
#include "host.h"
#include "cfg.h"
#include "mods.h"
#include "input.h"
#include "platform/platform.h"

int ldr_count(void) { abort(); }
const RtLoaded *ldr_module(int i) { (void)i; abort(); }
const RtConfig *rt_config(void) { abort(); }
const RtGuestLayout *mem_layout(void) { abort(); }
int plat_exe_path(char *out, size_t cap) { (void)out; (void)cap; abort(); }
PlatFile *plat_file_open(const char *path) { (void)path; abort(); }
void plat_file_close(PlatFile *f) { (void)f; abort(); }
int64_t plat_file_pread(PlatFile *f, void *buf, uint32_t n, uint64_t off) { (void)f; (void)buf; (void)n; (void)off; abort(); }
uint8_t *plat_file_read(const char *path, uint64_t *size) { (void)path; (void)size; abort(); }
int plat_file_write(const char *path, const void *data, size_t n) { (void)path; (void)data; (void)n; abort(); }
int plat_file_exists(const char *path) { (void)path; abort(); }
int plat_fs_unlink(const char *path) { (void)path; abort(); }
int plat_fs_list(const char *path, int (*cb)(const char *, int, uint64_t, void *), void *ctx) { (void)path; (void)cb; (void)ctx; abort(); }
int plat_spawn(const char *const *argv, const char *cwd) { (void)argv; (void)cwd; abort(); }
_Noreturn void plat_exit(int code) { (void)code; abort(); }
void mods_scan(void) { abort(); }
int mods_count(void) { abort(); }
ModInfo *mods_at(int i) { (void)i; abort(); }
int mods_exefs_pending(uint32_t *key) { (void)key; abort(); }
const char *cfg_get(const char *name) { (void)name; abort(); }
const char *test_spin_range = "normal";
const char *cfg_value(const char *name) {
    if (!strcmp(name, "NX2NAVI_SPIN_RANGE")) return test_spin_range;
    abort();
}
int cfg_bool(const char *name) { (void)name; abort(); }
void cfg_set(const char *name, const char *value) { (void)name; (void)value; abort(); }
int cfg_save(void) { abort(); }
int rt_patch_set(const char *name) { (void)name; abort(); }
int rt_patch_enabled(int set) { (void)set; abort(); }
void rt_patch_switch(int set, int on) { (void)set; (void)on; abort(); }
void *rt_patch_page(void) { abort(); }
void prof_write(void) { abort(); }
void rtss_limit_restore(void) { abort(); }
void input_poll(uint64_t *keys, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) { (void)keys; (void)lx; (void)ly; (void)rx; (void)ry; abort(); }
