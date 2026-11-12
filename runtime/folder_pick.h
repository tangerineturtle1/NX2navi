// The host's own folder and file dialogs, for menu pages that point at a folder (the Amiibo tab) or a file (a shared
// save's zip, the Game tab), and the player's Downloads folder: folder_pick_win.cpp with Win32, folder_pick_sdl.c with
// SDL. A folder is chosen here or by walking the page's list, not typed.
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// done gets the chosen folder (UTF-8) or NULL when the player cancelled or the host has no dialog.
typedef void (*FolderPickDone)(const char *path, void *ctx);

// Opens the dialog at start (may be relative or missing) and returns at once; done runs later on another thread, once.
// The caller keeps one dialog open at a time.
void folder_pick(const char *start, FolderPickDone done, void *ctx);
// The same for one file: the dialog lists the files whose name ends in "." ext ("zip") and offers all files too.
void file_pick(const char *start, const char *ext, FolderPickDone done, void *ctx);
// The player's Downloads folder (where the system keeps it, also when moved) into out, UTF-8. 0 when there is none.
int folder_downloads(char *out, size_t cap);

#ifdef __cplusplus
}
#endif
