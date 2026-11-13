// Widescreen (Ocarina of Time 1.1.1): the game draws a picture of another shape than 16:9, with the camera seeing more
// of the world at the sides (Hor+: the vertical view stays the game's) instead of a stretched 16:9 view.
//
// The game presents a 2560x1440 picture at every console mode and renders its scene at 1920x1080, which its upscaler
// takes to the picture's size; both sizes are constants in its code. The patch sets of recomp/patches-v111.txt read
// them from the patch page instead (wide_out: the picture, wide_scene: the scene), wide_aspect replaces a 16:9 ratio of
// sead's perspective projection (every 16:9 camera of the game) with the picture's, wide_hud keeps the HUD and menus
// in their 16:9 shape in the middle of the picture (the UI's full-screen projection squeezed horizontally; fades and
// the menu backgrounds that redraw a capture of the whole picture stay full width), wide_movie shows pre-rendered
// movies in the same middle 16:9, and wide_save draws that middle 16:9 into the picture a save file keeps. The heights
// stay the game's (1440 and 1080), so only the width grows, by the shape's ratio to 16:9; the present scales the
// picture to the window keeping its shape (gfx_present.c, NX2NAVI_ASPECT=fit).
//
// Setting NX2NAVI_GAME_ASPECT, read at start: "16:9" (the game's own, every set off), "window" (the shape of the window,
// or of the monitor in fullscreen), or a fixed shape "21:9" (2560x1080 monitors, 64:27), "32:9". A shape within 1% of
// 16:9 or narrower than it keeps the game's own sizes; the widest is 32:9.
//
// The page's layout is described at the top of recomp/patches-v111.txt.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx_wsys.h"
#include "host.h"
#include "layout.h"
#include "ui/menu.h"
#include "vk_dlaa.h"

#define GAME_W 2560   // the picture, main+0x1202ee0 and main+0x31f39f4
#define GAME_H 1440
#define SCENE_H 1080
#define GAME_DTA_KIB 312320.0     // the texture heap made for that picture (wide_heap_extra_kib)
#define SAVE_W 543                // the width of the target a save's picture is drawn into (save_extra)

// The window's size, or the monitor's in fullscreen; 0 when unknown.
static int screen_size(int *w, int *h) {
    *w = *h = 0;
    if (gfx_cfg_fullscreen()) return gfx_window_monitor_size(w, h) && *h > 0;
    gfx_cfg_window_size(w, h);
    return *h > 0;
}

// The shape the setting asks for as width / height; 0 for the game's own.
static double wanted_ratio(void) {
    const char *s = cfg_value("NX2NAVI_GAME_ASPECT");
    if (!strcmp(s, "21:9")) return 64.0 / 27.0;
    if (!strcmp(s, "32:9")) return 32.0 / 9.0;
    if (strcmp(s, "window")) return 0;
    int w, h;
    if (!screen_size(&w, &h)) {
        rt_log("widescreen: the screen's size is unknown; the game's own 16:9");
        return 0;
    }
    return (double)w / h;
}

// NX2NAVI_GAME_RES: the picture's height, 1080 up to 4320 ("window": the window's height, or the monitor's in
// fullscreen, never below the game's 1440). 1080 is the one height below the game's own: a lighter picture for slower
// GPUs, whose scene the game sizes against that picture itself, as the comment at scale_k says.
#define PICTURE_MIN_H 1080
static unsigned picture_height(void) {
    const char *s = cfg_value("NX2NAVI_GAME_RES");
    int w = 0, h = GAME_H, min = PICTURE_MIN_H;
    if (!strcmp(s, "window")) {
        if (!screen_size(&w, &h)) h = GAME_H;
        min = GAME_H;
    } else {
        h = atoi(s);
    }
    return h < min ? (unsigned)min : h > 4320 ? 4320 : (unsigned)h;
}



// ---- DLSS's render scale (recomp/patches-v111.txt, set res_fixed)
// While DLSS is on, the scene is the DLSS mode's share of the picture (NVIDIA's ratios: DLAA all of it, Quality 2/3,
// Balanced 0.58, Performance 1/2, Ultra Performance 1/3) instead of what the game's dynamic-resolution controller
// picks. The share is written to +240 at every change of the setting (the menu switches the mode live); the set goes on
// RES_FIXED_AFTER presents after the first, once the controller has filled the fields the fixed path keeps. Without
// DLSS or FSR the share is NX2NAVI_SCENE_SHARE's (plain_share): the game's own most unless set, all of the picture
// for the menu's Native.
#define RES_FIXED_AFTER 120
// The share follows the upscaler that runs, not the setting: DLSS starts only after a restart and only where NGX came up
// (vk_dlaa_available), and FSR 3.1 only where its library loads (vk_dlaa_fsr3), so a mode picked in the menu, or one
// this PC cannot run, no longer shrinks the scene with nothing to scale it back up. FSR 3.1 and a mode change while one
// runs apply at once.
// dlaa switched off in the menu stops DLSS at once (vk_dlaa.c vk_dlaa_frame reads it each frame), so the share follows
// the setting too: the scene goes back to the game's own scale instead of staying small with nothing to enlarge it.
static double dlss_share(void);
double wide_dlss_share(void) { return dlss_share(); }   // vk_dlaa.c: the mode's share, 0 off

// Without DLSS or FSR 3.1 (FSR 1, bilinear) the game's dynamic resolution took the scene down to a third of the picture
// under load, and a plain scale shows every step of that. The most the game's controller ever gives (wide_init) is held.
#define PLAIN_SHARE 0.75

static double heap_share(void);

// Set by wide_init when the grown heap table (wide_heap_extra_kib) did not reach the game: romfs.c could not build
// it (a table in a form it does not rewrite, as a mod's may be) or could not write it to the profile folder. The game
// then has the shipped heap, made for its own picture with the scene at PLAIN_SHARE, and gets no more than that:
// a larger scene or picture in the shipped heap is the crash the growth is there to prevent.
static int heap_unserved;

// The most the scene can be of the picture in the heap the game has.
static double fit_share(void) {
    return heap_unserved ? PLAIN_SHARE : heap_share();
}

// The share of the picture the upscaler's mode renders (NX2NAVI_DLSS; NVIDIA's ratios).
static double mode_share(void) {
    const char *m = cfg_value("NX2NAVI_DLSS");
    return !strcmp(m, "quality") ? 2.0 / 3 : !strcmp(m, "balanced") ? 0.58 : !strcmp(m, "performance") ? 0.5
         : !strcmp(m, "ultra_performance") ? 1.0 / 3 : 1.0;
}

// DLSS or FSR 3.1 scales the scene in this run: the setting is on and the upscaler came up.
static int upscaler_runs(void) {
    return vk_dlaa_wanted() && cfg_bool("NX2NAVI_DLAA") && (vk_dlaa_fsr3() || vk_dlaa_available());
}

static double dlss_share(void) {
    if (!upscaler_runs()) return 0;
    double s = mode_share();
    // Never more than the texture heap the game has was made for (fit_share). A mode chosen in the menu that renders
    // more than the start's did gets its size after a restart; a heap at its limit never holds the whole picture.
    if (s > fit_share()) {
        static int said;
        if (!said++)
            rt_log("widescreen: the scene is %.0f%% of the picture, not the mode's %.0f%%: the game's render-target heap "
                   "%s. A mode chosen in the menu gets its size after a restart, unless the start's log says the heap "
                   "is at its limit", fit_share() * 100, s * 100,
                   heap_unserved ? "is the shipped one (the start's log says why the grown one was not served)"
                                 : "was sized at the start for that much");
        s = fit_share();
    }
    return s;
}

// The share without DLSS or FSR 3.1 (NX2NAVI_SCENE_SHARE, 1 at most): PLAIN_SHARE unset, 1 for the menu's Native
// (ui/menu.cpp row_upscaling: the scene at the picture's size, which the stand-in then only decodes, vk_tonemap.c),
// 0 for the game's dynamic resolution, as with no share at all. Read at every call, so the menu's choice applies at
// once within the heap the start made (fit_share).
static double plain_share(void) {
    const char *e = cfg_get("NX2NAVI_SCENE_SHARE");
    double s = e && *e ? atof(e) : PLAIN_SHARE;
    return s > 1 ? 1 : s;
}

static double scene_share(void) {
    double s = dlss_share(), plain = plain_share();
    return s > 0 ? s : plain > fit_share() ? fit_share() : plain;   // NX2NAVI_SCENE_SHARE above the heap the game has
}

// The share the settings ask for, before the heap's limit: the upscaler's mode while one runs, else plain_share.
static double wanted_share(void) {
    return upscaler_runs() ? mode_share() : plain_share();
}

static int picture_size(unsigned *ow, unsigned *oh, int quiet);

// The picture's pixels over the game's own 2560x1440.
static double picture_ratio(void) {
    unsigned ow, oh;
    if (!wide_available() || !picture_size(&ow, &oh, 1)) ow = GAME_W, oh = GAME_H;
    return (double)ow * oh / ((double)GAME_W * GAME_H);
}

// The most share of the picture the application pool has room for at the picture the settings ask for (the heap
// grows with the share's square, see heap_share). Computed anew at each call, so the menu can tell whether a restart
// would give a larger scene (wide_scene_restart).
static double share_most(void) {
    double room = (double)mem_heap_extra_max_kib() / GAME_DTA_KIB + 1;   // the heap's most over the shipped one
    return sqrt(room / picture_ratio()) * PLAIN_SHARE;
}

// The largest share of the picture the scene can have in this run, which the texture heap is made for
// (wide_heap_extra_kib). Decided once, at the first call, which is the start (memory.c sizes the application pool
// from it before any device exists): the game's own most, PLAIN_SHARE, or more where the settings at the start ask
// for more: NX2NAVI_SCENE_SHARE, or the upscaler's mode when the upscaler is on (DLAA and FSR at native size render
// the whole picture; the modes below PLAIN_SHARE need nothing). From the settings, not from what runs: DLSS is known
// to run only once the device is up. A mode chosen in the menu later that renders more keeps the start's share until
// a restart (dlss_share).
// Never more than the application pool can take: the heap grows with the share's square (wide_heap_extra_kib), and
// past mem_heap_extra_max_kib the game's own SetHeapSize is refused and nothing starts. A 10240x2880 picture has room
// for 80% there, not for all of it, and the scene is then that much.
static double heap_share(void) {
    static double share = -1;
    if (share < 0) {
        double s = plain_share();
        if (s < PLAIN_SHARE) s = PLAIN_SHARE;
        if (rt_patch_set("res_fixed") >= 0 && cfg_bool("NX2NAVI_DLAA") && mode_share() > s) s = mode_share();
        double most = share_most();
        if (s > most) {
            if (most < PLAIN_SHARE) most = PLAIN_SHARE;   // wide_heap_extra_kib says what a picture this large lacks
            rt_log("widescreen: the scene can be %.0f%% of the picture at most, not %.0f%%: the game's render-target "
                   "heap for more does not fit the application pool (%llu MiB of growth at most)", most * 100, s * 100,
                   (unsigned long long)(mem_heap_extra_max_kib() >> 10));
            s = most;
        }
        share = s;
    }
    return share;
}

// The game turns the scale at +240 into a scene size against its own 1440-line picture while the picture is taller
// (a 2160-line picture at 1.0 gave a 1440-line scene), so the share goes there times the picture's height over 1440.
// Under a 1080-line picture the game measures against that picture instead (a share of 1.0 times 1080/1440 gave an
// 810-line scene, three quarters of the picture), so there the share goes in as it is: wide_init sets the factor.
static double scale_k = 1.0;

// One line for the scene in effect: its size, the picture's and how the scene reaches the picture's size. Written when
// the fixed share first takes over from the game's dynamic resolution and again when the share or the way it is scaled
// changes (a setting changed in the menu), not at every settings change. The scene's size is the share of the
// picture's as the game sizes it (a 50% scene of 5120x2160 is 2560x1080).
static void scene_log(double s, int first) {
    static double last_s = -1;
    static int last_how = -1;
    // 1 DLSS, 2 FSR, 3 not scaled (the stand-in decodes 1:1), 4 the stand-in's edge-adaptive filter, 5 its linear one
    int how = dlss_share() > 0 ? (vk_dlaa_fsr3() ? 2 : 1) : s >= 1 ? 3
            : !strcmp(cfg_value("NX2NAVI_STANDIN_FILTER"), "easu") ? 4 : 5;
    if (!first && how == last_how && fabs(s - last_s) < 0.001) return;
    last_s = s, last_how = how;
    static const char *const hows[] = {
        "", "DLSS scales it to the picture", "AMD FSR scales it to the picture",
        "native: no upscaling, the upscaler's stand-in only decodes it 1:1",
        "the upscaler's stand-in enlarges it with the edge-adaptive filter (no DLSS or FSR)",
        "the upscaler's stand-in enlarges it with the linear filter (no DLSS or FSR)"};
    unsigned pw, ph;
    wide_picture(&pw, &ph);
    rt_log("widescreen: scene %ux%u, picture %ux%u (%.0f%% of it): %s%s", (unsigned)lround(pw * s),
           (unsigned)lround(ph * s), pw, ph, s * 100, hows[how], first ? " (the game's dynamic resolution off)" : "");
    // The menu's Native, or another share, larger than the heap the start made: say why the scene is smaller
    double want = wanted_share();
    if (how >= 3 && want > s + 0.001)
        rt_log("widescreen: scene_share asks for %.0f%% of the picture; the game's render-target heap %s", want * 100,
               heap_unserved ? "is the shipped one (the start's log says why the grown one was not served)"
               : wide_scene_restart() ? "was sized at the start for less: the next start gives it"
                                      : "is at its limit for this picture (the start's log says so)");
}

void wide_present(void) {
    static int set = -2;
    static uint64_t presents;
    static unsigned seen = ~0u;
    if (set == -2) set = rt_patch_set("res_fixed");
    if (set < 0) return;
    unsigned v = cfg_version();
    int at = ++presents == RES_FIXED_AFTER, due = presents >= RES_FIXED_AFTER;
    if (v == seen && !at) return;
    seen = v;
    double s = scene_share();
    float page_s = (float)(s * scale_k);
    if (s > 0) memcpy((uint8_t *)rt_patch_page() + 240, &page_s, 4);
    int on = rt_patch_enabled(set);
    if (s <= 0) {
        if (on) rt_patch_switch(set, 0);
        return;
    }
    // On at RES_FIXED_AFTER, once the controller has filled its fields; later too, after a share of 0 (the game's
    // dynamic resolution) gave way to a fixed one
    if (!due) return;
    if (!on) rt_patch_switch(set, 1);
    scene_log(s, !on);
}

int wide_scene_settable(void) { return rt_patch_set("res_fixed") >= 0; }

double wide_scene_share(void) { return scene_share(); }

double wide_scene_wanted(void) { return wanted_share(); }

// The settings ask for a larger scene than the heap the start made holds, and the next start's heap holds more: the
// share it asks for within what the application pool has room for at the picture the settings ask for (share_most).
// A heap the game was not served (heap_unserved) stays the shipped one at the next start too, as far as is known here.
int wide_scene_restart(void) {
    double want = wanted_share(), now = fit_share(), next = share_most();
    if (want > 1) want = 1;
    return !heap_unserved && want > now + 0.001 && next > now + 0.001;
}

// The picture the settings ask for, *ow x *oh; 0 when it is the game's own 2560x1440 (every set stays off). Read at the
// start, before the window exists: the monitor's size is known then.
static int picture_size(unsigned *ow, unsigned *oh, int quiet) {
    double r = wanted_ratio(), game = (double)GAME_W / GAME_H;
    *oh = picture_height();
    if (r <= 0 || r < game * 1.01) {
        if (r > 0 && !quiet) rt_log("widescreen: a %.4f picture is not wider than 16:9; the game's 16:9", r);
        r = game;
    }
    if (r == game && *oh == GAME_H) return 0;
    if (r > 32.0 / 9.0) r = 32.0 / 9.0;
    *ow = (unsigned)lround(*oh * r / 8) * 8;   // whole multiples of 8 pixels
    return 1;
}

// The texture heap: the engine's render targets live in "DynamicTextureAllocatorMem" of the HeapDef table
// (System/HeapSize/Product.Nin_Ounce_NVN2.HeapDef.byml.zs; 312320 KiB, carved from "Graphics"), made for the 2560x1440
// picture with the scene at three quarters of it at most. Larger pictures' targets did not fit: the render-target
// manager's search for a free block (main+0x3ed00) fell to its out-of-memory path and read through a null block when
// the pause menu's blur needed one at 5120x2160. So did a larger scene in the game's own picture: with DLAA the scene
// is all of the picture, 2560x1440 where the game never goes past 1920x1080, and a player's game ended in that same
// function, reading address 0x48, each time a dialogue closed in Castle Town (0.8.0, 1440 lines, dlss = dlaa).
// The heap and its parent grow by the shipped size times the picture's pixels over 2560x1440 and times the scene's
// pixels over the game's most (heap_share: its targets are the picture's size or the scene's, so this is the most
// they can need), in whole MiB; resolution.c rewrites the table (res_heapdef_build) and memory.c adds the amount to
// the application pool, so no other heap shrinks. 0 for the game's picture and scene, and for a larger picture
// without the widescreen sets. In MiB, with the scene at the game's own most and with all of the picture (DLAA):
// 2560x1440 0 and 238, 3440x1440 105 and 424, 3840x2160 382 and 915, 5120x2160 610 and 1,322, 7680x2160 1,068 and
// 2,135. The heap is committed as the game first writes it (memory.c, NX2NAVI_HEAP_LAZY), so the room a run does not
// use costs address space and no memory.
// At most mem_heap_extra_max_kib: heap_share has taken the scene's share down to what fits, and a picture too large
// even with the game's own scene (above 10240x2880) gets the most there is, with a line that says so.
uint64_t wide_heap_extra_kib(void) {
    double k = heap_share() / PLAIN_SHARE;
    double grow = picture_ratio() * k * k - 1;
    uint64_t kib = grow > 0 ? (uint64_t)ceil(GAME_DTA_KIB * grow / 1024) * 1024 : 0, most = mem_heap_extra_max_kib();
    if (kib > most) {
        static int said;
        if (!said++)
            rt_log("widescreen: WARNING: a picture of %.1f times the game's own needs %llu MiB more render-target heap "
                   "and the application pool has room for %llu: the game may end when a screen needs more targets "
                   "than fit. A smaller game_res or a narrower game_aspect fits", picture_ratio(),
                   (unsigned long long)(kib >> 10), (unsigned long long)(most >> 10));
        kib = most;
    }
    return kib;
}

// The save's picture (recomp/patches-v111.txt, set wide_save): the game draws the whole picture into a target 543
// pixels wide, which squeezes a picture wider than 16:9. The viewport of that draw is made wider than its target by the
// picture's ratio over 16:9, half of it on each side, so the target, which the scissor keeps to, holds the middle 16:9
// of the picture. Returns the pixels added for a picture of ow x oh: an even number, so the picture's middle stays in
// the target's middle; 0 for a picture that is not wider than 16:9, taller or not.
static int32_t save_extra(unsigned ow, unsigned oh) {
    double wider = (double)ow * GAME_H / ((double)oh * GAME_W);
    return wider > 1 ? 2 * (int32_t)lround(SAVE_W * (wider - 1) / 2) : 0;
}

void wide_init(void) {
    int so = rt_patch_set("wide_out"), ss = rt_patch_set("wide_scene"), sa = rt_patch_set("wide_aspect"),
        sh = rt_patch_set("wide_hud"), sm = rt_patch_set("wide_movie"), sv = rt_patch_set("wide_save");
    // The scene's share of the picture is not a size in this set: the game's GPU-time dynamic resolution sets it each
    // frame (3/4 of the picture at most), and writing other sizes into wide_scene's constant or the 1920x1080 render
    // sizes left the light target where the controller put it. DLSS's modes fix the share through res_fixed instead
    // (wide_present).
    double game = (double)GAME_W / GAME_H, f = 0.75;
    unsigned ow, oh;
    // The grown heap table is served from here on, or is not (romfs.c heapdef_overlay says why in the log): before
    // the guest runs and before the first present asks for the scene's share.
    if (wide_heap_extra_kib()) {
        fs_init();
        if (!res_heapdef_ok()) {
            heap_unserved = 1;
            rt_log("widescreen: the game's render-target heap stays the shipped one (the grown table was not served, see "
                   "the RomFS and resolution lines above): the scene is %.0f%% of the picture at most and the picture "
                   "the game's own 2560x1440", PLAIN_SHARE * 100);
        }
    }
    if (!picture_size(&ow, &oh, 0)) return;   // the game's own sizes: every set stays off
    if (heap_unserved) return;                // a larger picture's targets do not fit the shipped heap
    if (so < 0 || ss < 0 || sa < 0 || sh < 0 || sm < 0) {
        rt_log("widescreen: the generated C has no widescreen patch sets (gen.py --patches recomp/patches-v111.txt); "
               "the game's own 2560x1440 picture");
        return;
    }
    // The scene 3/4 of both sides, its width in whole multiples of 8 pixels and its height even.
    unsigned sw = (unsigned)lround(ow * f / 8) * 8;
    unsigned sch = (unsigned)lround(oh * f / 2) * 2;
    uint64_t out = (uint64_t)oh << 32 | ow, scene = (uint64_t)sch << 32 | sw;
    float match = 16.0f / 9.0f, ratio = (float)ow / oh, tolerance = 0.01f, squeeze = match / ratio;
    float ui[2] = {2.0f / 3840.0f, 2.0f / 2160.0f}, squeeze4[4] = {squeeze, squeeze, squeeze, squeeze};
    float margin = (1.0f - squeeze) / 2;
    // the screens whose UI keeps the whole picture (recomp/patches-v111.txt, set wide_hud): the item-wheel, pause and
    // system menu backgrounds, the event and loading fades
    uint64_t keep[2] = {MOD_MAIN_BASE + 0x4bed098, MOD_MAIN_BASE + 0x4be4fb0},
             keep2[2] = {MOD_MAIN_BASE + 0x4be61c8, MOD_MAIN_BASE + 0x4be7b30};
    uint8_t *page = rt_patch_page();
    memcpy(page + 96, &out, 8);
    memcpy(page + 104, &scene, 8);
    memcpy(page + 112, &match, 4);
    memcpy(page + 116, &ratio, 4);
    memcpy(page + 120, &tolerance, 4);
    memcpy(page + 144, ui, 8);
    memcpy(page + 160, keep, 16);
    memcpy(page + 176, squeeze4, 16);
    memcpy(page + 192, &margin, 4);
    memcpy(page + 208, keep2, 16);
    // the save's picture: what its viewport gets added to its x and to its width. The set is not one wide_available
    // asks for: C generated before it existed keeps the other sets and squeezes the save's picture as before.
    int32_t extra = save_extra(ow, oh), save[2] = {-(extra / 2), extra};
    memcpy(page + 224, save, 8);
    rt_patch_switch(so, 1);
    rt_patch_switch(ss, 1);
    rt_patch_switch(sa, 1);
    rt_patch_switch(sh, 1);
    rt_patch_switch(sm, 1);
    if (extra > 0) rt_patch_switch(sv, 1);
    scale_k = oh > GAME_H ? (double)oh / GAME_H : 1.0;
    rt_log("widescreen: picture %ux%u, scene %ux%u (%.0f%% of it), camera aspect ratio %.4f (16:9 is %.4f), UI and movies "
           "in the middle %.1f%%", ow, oh, sw, sch, f * 100, ratio, game, squeeze * 100);
    if (extra > 0 && sv >= 0)
        rt_log("widescreen: a save's picture holds the middle 16:9 (its viewport %d pixels wide on the %d of its target)",
               SAVE_W + extra, SAVE_W);
    else if (extra > 0)
        rt_log("widescreen: the generated C has no set wide_save; a save's picture holds the whole picture, squeezed");
}

// The picture the game draws: the widescreen one wide_init set up, else the game's own 2560x1440. DLSS's largest mode
// (dlaa) renders the scene at this size.
void wide_picture(unsigned *w, unsigned *h) {
    static unsigned pw, ph;
    if (!pw) {
        if (!wide_available() || heap_unserved || !picture_size(&pw, &ph, 1)) pw = GAME_W, ph = GAME_H;
    }
    *w = pw, *h = ph;
}

// The generated C has the widescreen patch sets (the menu shows the row only then).
int wide_available(void) {
    static const char *const names[] = {"wide_out", "wide_scene", "wide_aspect", "wide_hud", "wide_movie"};
    for (int i = 0; i < 5; i++)
        if (rt_patch_set(names[i]) < 0) return 0;
    return 1;
}
