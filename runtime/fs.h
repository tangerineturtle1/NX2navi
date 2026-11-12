// Contains code Copyright (c) 2026 MacNativeGames contributors (MIT; see THIRD_PARTY_NOTICES.md).
// The nn::fs host layer: the read-only RomFS (romfs.c) and writable save, temporary and
// cache storage under the profile directory (fs_host.c). Both register their nn::fs overrides with HOST_OVERRIDE;
// this header is what the two files and the probe share.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

// ---- nn::fs results
#define FS_R_PATH_NOT_FOUND 0x202u
#define FS_R_ALREADY_EXISTS 0x402u
#define FS_R_NOT_EMPTY 0x1002u
#define FS_R_UNSUPPORTED_OPEN 0x6402u    // RomFS: any open mode but read
#define FS_R_INVALID 0xe402u             // RomFS: bad handle, offset or argument
#define FS_R_TARGET_NOT_FOUND 0x7d402u
#define FS_R_INVALID_ARGUMENT 0x2ee202u
#define FS_R_INVALID_PATH 0x2ee602u
#define FS_R_OPEN_MODE 0x307402u
#define FS_R_APPEND_REQUIRED 0x2f5e02u

enum { FS_OPEN_READ = 1, FS_OPEN_WRITE = 2, FS_OPEN_APPEND = 4 };   // nn::fs::OpenMode
enum { FS_ENTRY_DIRECTORY = 0, FS_ENTRY_FILE = 1 };                 // nn::fs::DirectoryEntryType

// ---- romfs.c
// Handles 0x1_0000_0000 + j; host storage uses 0x2_0000_0000 + j (files) and 0x3_0000_0000 + j (directories).
#define ROMFS_HANDLE_BASE 0x100000000ull
int romfs_owns(const char *path);                          // path is on the mounted RomFS ("<mount>:...")
int romfs_handle(uint64_t h);                              // h is a RomFS file handle
uint32_t romfs_mount(const char *name);                    // nn::fs::MountRom; a second name is fatal
uint32_t romfs_open(uint64_t *out, const char *path, int mode);
uint32_t romfs_read(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size);
uint32_t romfs_size(int64_t *out, uint64_t h);
void romfs_close(uint64_t h);
uint8_t *romfs_load(const char *rel, uint64_t *size_out);   // whole file by RomFS-relative path, malloc'd (or NULL)
uint32_t romfs_entry_type(int *out, const char *path);
// The children of RomFS directory path: fn(name, is_dir, size, ctx) once per file and once per subdirectory, in
// index order; fn returns 0 to stop. FS_R_PATH_NOT_FOUND when path is not a directory.
uint32_t romfs_list(const char *path, int (*fn)(const char *name, int is_dir, uint64_t size, void *ctx), void *ctx);
uint64_t romfs_file_count(void);                           // files in the index (fs_init first)
const char *romfs_entry(uint64_t i, uint64_t *offset, uint64_t *size);   // path, image offset and size of index row i
const char *romfs_image_path(void);                        // the romfs.bin path fs_init opened
size_t romfs_overlay_count(void);
int romfs_patches_loaded(void);                            // a romfs-patches manifest gave at least one file
// The button prompts (input_prompts_folder): the folder loaded at start, NULL for the game's own prompts; whether a
// folder had a manifest at start; and why the folder the settings asked for at start was not loaded (ROMFS_PROMPTS_OK
// for any other folder: it was not asked for). romfs-patches-pc goes by that name also where romfs-patches-pc-<w>x<h>
// stands in for it (romfs.c overlay_init).
enum { ROMFS_PROMPTS_OK, ROMFS_PROMPTS_MISSING, ROMFS_PROMPTS_FAILED, ROMFS_PROMPTS_SHAPE, ROMFS_PROMPTS_MODS,
       ROMFS_PROMPTS_PATCHES };
const char *romfs_prompts(void);
int romfs_prompts_ready(const char *folder);
int romfs_prompts_off(const char *folder);
// The pad layout (PAD_LAYOUT_*, input_map.h) the action prompt's diamond of face buttons has from start
// (action_prompt.c); -1 when the layout by label was asked for but the layout file could not be changed.
int romfs_action_prompt(void);
const char *romfs_overlay(size_t k, const char **file, uint64_t *size, int *is_added);   // RomFS path of overlay k
void fs_trace(const char *what, const char *path);         // NX2NAVI_TRACE=fs: one line per open

// ---- fs_host.c
void fs_host_set_profile(const char *dir);                 // profile directory; before the first storage mount only
                                                           // (default rt_config()->profile)
void fs_host_lock_profile(void);                           // the one-game-per-profile lock; fatal when another copy
                                                           // holds it (rt_boot, before the prewarm; storage_init)
int fs_save_busy(void);                                    // a save is being written: a save file open for writing,
                                                           // or save data changed in the last 3 s and not committed
void fs_save_flush(void);                                  // wait for a save Commit still running in the background;
                                                           // every exit path calls it before the process ends
// A shared save (save_share.c). fs_save_export: visit(ctx, path below the save with '/', bytes, size) for every file of
// the save the game would load next, under the commit lock; visit returns 0 to fail. The number of files (0: no save
// yet), -1 when one could not be read or visit failed.
typedef int (*FsSaveVisit)(void *ctx, const char *rel, const uint8_t *data, size_t n);
int fs_save_export(FsSaveVisit visit, void *ctx);
// The menu's import: count files (relative '/' paths, bytes) become <profile>/save-import, which the next start makes
// the save, backing up the save it replaces; one waiting already is replaced. 0 on success.
int fs_save_import_stage(int count, const char *const *rel, const uint8_t *const *data, const size_t *size);
int fs_save_import_waiting(void);                          // 1 while an import waits for the next start (0 before
                                                           // the first storage use)
int fs_save_import_cancel(void);                           // drops it; 0 on success
const char *fs_save_import_done(void);                     // the backup of the save an import replaced at this start,
                                                           // "-" when there was none, "" when no import ran
// A copy of the newest committed save (every slot) in <profile>/<folder>/<UTC time>/, flushed, taken while no Commit
// runs; the newest keep copies stay. prefix (NULL: none) goes before the time, and a copy whose name it begins is
// never pruned: the copy from before a co-op room, say, outlasts the ones made later. 1 and the copy's path in out
// (empty when nothing is saved yet: nothing to copy), 0 when it could not be made (logged). Slow (the whole save): not
// on the game's thread.
int fs_save_snapshot(const char *folder, const char *prefix, int keep, char *out, size_t cap);
// <profile>/rel into out: a file of the profile folder beside the saves (the co-op maker's room keys). 0 when there is
// no profile folder or the path does not fit.
int fs_profile_path(const char *rel, char *out, size_t cap);
