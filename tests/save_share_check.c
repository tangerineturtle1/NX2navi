// runtime/fs_host.c, save_share.c and save_zip.c without the game, driven by tests/save_share_check.py. Each run of
// this program is one start of the game on the profile folder it is given: the first storage use runs storage_init,
// which makes a waiting import the save (fs_host.c import_pending). The rest of the runtime is stubbed below: only what
// the save paths call does anything.
//   save_share_check export PROFILE DIR [bug]   save_share_export into DIR; prints "result N path P"
//   save_share_check import PROFILE ZIP         save_share_import; prints "result N detail D waiting W"
//   save_share_check start PROFILE              a start and nothing else; prints "done D waiting W"
//   save_share_check cancel PROFILE             save_share_cancel; prints "result N waiting W"
// Backups go to saves/ beside this program (save_share_backup_dir), as the game's go beside the game.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "save_share.h"

// fs.h's own declarations of these would clash with the stubs below, which only have to link
void fs_host_set_profile(const char *dir);
const char *fs_save_import_done(void);
int fs_save_export(int (*visit)(void *ctx, const char *rel, const uint8_t *data, size_t n), void *ctx);

static int count_file(void *ctx, const char *rel, const uint8_t *data, size_t n) {
    (void)ctx, (void)rel, (void)data, (void)n;
    return 1;
}

// ---- stubs for the runtime around fs_host.c (host.h and the files that define these in the game)
const int rt_game_version = 111;
const char rt_port_version[] = "9.9.9-check";

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("log: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("FATAL: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(99);
}

const char *rt_log_path(void) { return getenv("SAVE_SHARE_CHECK_LOG"); }
void *rt_config(void) { return NULL; }   // fs_host_set_profile names the profile first
void account_uid(uint64_t uid[2]) { uid[0] = 0x1111222233334444ull, uid[1] = 0x5555666677778888ull; }
int folder_downloads(char *out, size_t cap) { (void)out, (void)cap; return 0; }

// never reached on the save paths: RomFS, the host function registry, guest memory, the probes
#define NEVER(name) \
    void name(void) { rt_fatal(#name " called"); }
NEVER(fs_init) NEVER(hostfn_override) NEVER(hostfn_register) NEVER(mem_kernel_alloc) NEVER(mem_kernel_free)
NEVER(mem_thaw) NEVER(romfs_close) NEVER(romfs_entry) NEVER(romfs_entry_type) NEVER(romfs_file_count)
NEVER(romfs_handle) NEVER(romfs_image_path) NEVER(romfs_list) NEVER(romfs_open) NEVER(romfs_overlay)
NEVER(romfs_overlay_count) NEVER(romfs_read) NEVER(romfs_size) NEVER(rt_call)
NEVER(perf_size)   // fs_host_sizes: only the perf trace's size lines call it
int romfs_owns(const char *path) { (void)path; return 0; }
int mem_probing(void) { return 0; }   // write_file asks before each write: no probe, no lazy heap here
void mem_kernel_access(uint64_t a, uint64_t n) { (void)a, (void)n; }
void fs_trace(const char *what, const char *path) { (void)what, (void)path; }

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    fs_host_set_profile(argv[2]);
    const char *mode = argv[1];
    if (!strcmp(mode, "export") && argc >= 4) {
        char path[2048];
        SaveShareResult r = save_share_export(argv[3], argc > 4 && !strcmp(argv[4], "bug"), path, sizeof path);
        printf("result %d path %s\n", (int)r, path);
    } else if (!strcmp(mode, "import") && argc == 4) {
        char detail[256];
        SaveShareResult r = save_share_import(argv[3], detail, sizeof detail);
        printf("result %d detail %s waiting %d\n", (int)r, detail, save_share_pending());
    } else if (!strcmp(mode, "start")) {
        fs_save_export(count_file, NULL);   // the first storage use: storage_init and its import_pending
        printf("done %s waiting %d\n", fs_save_import_done(), save_share_pending());
    } else if (!strcmp(mode, "cancel")) {
        int r = save_share_cancel();
        printf("result %d waiting %d\n", r, save_share_pending());
    } else {
        return 2;
    }
    return 0;
}
