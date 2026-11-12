// The SDL front end of folder_pick.h: SDL_ShowOpenFolderDialog and SDL_ShowOpenFileDialog (the desktop portal, else
// zenity), started on the window thread, which runs SDL's event loop that delivers the portal's answer, and SDL's user
// folders.
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "folder_pick.h"
#include "host.h"
#include "input_sdl.h"

typedef struct {
    char start[PATH_MAX];                // absolute, or empty: the dialog's own default
    char ext[16];                        // a file of this extension; empty: a folder
    SDL_DialogFileFilter filters[2];     // read by SDL until the answer comes
    FolderPickDone done;
    void *ctx;
} PickReq;

static void SDLCALL on_pick(void *userdata, const char *const *list, int filter) {
    (void)filter;
    PickReq *r = userdata;
    if (!list) rt_log("folder_pick: the folder dialog failed (%s)", SDL_GetError());
    r->done(list && list[0] ? list[0] : NULL, r->ctx);
    free(r);
}

static void show(void *arg) {
    PickReq *r = arg;
    if (!r->ext[0]) {
        SDL_ShowOpenFolderDialog(on_pick, r, gfx_window_sdl(), r->start[0] ? r->start : NULL, false);
        return;
    }
    r->filters[0] = (SDL_DialogFileFilter){r->ext, r->ext};   // the extension is its own name: no text to translate
    r->filters[1] = (SDL_DialogFileFilter){"*", "*"};
    SDL_ShowOpenFileDialog(on_pick, r, gfx_window_sdl(), r->filters, 2, r->start[0] ? r->start : NULL, false);
}

static void pick(const char *start, const char *ext, FolderPickDone done, void *ctx) {
    PickReq *r = calloc(1, sizeof *r);
    if (!r) {
        done(NULL, ctx);
        return;
    }
    r->done = done;
    r->ctx = ctx;
    if (ext) snprintf(r->ext, sizeof r->ext, "%s", *ext ? ext : "*");
    if (start && *start && !realpath(start, r->start)) r->start[0] = 0;
    gfx_window_sdl_run(show, r);
}

void folder_pick(const char *start, FolderPickDone done, void *ctx) { pick(start, NULL, done, ctx); }

void file_pick(const char *start, const char *ext, FolderPickDone done, void *ctx) { pick(start, ext, done, ctx); }

int folder_downloads(char *out, size_t cap) {
    const char *p = SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS);
    size_t n = p ? strlen(p) : 0;
    while (n > 1 && p[n - 1] == '/') n--;   // SDL ends it with a separator
    if (!n || n >= cap) return 0;
    memcpy(out, p, n);
    out[n] = 0;
    return 1;
}
