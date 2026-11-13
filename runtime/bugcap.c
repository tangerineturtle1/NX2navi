// Bug captures (bugcap.h): what a player's F8 writes, and the run that starts from it. docs/bug-capture.md.
//
// A capture is one folder, <log folder>/captures/<number>-<YYYYMMDD-HHMMSS>/ (NX2NAVI_CAPTURE_DIR names another base):
//
//   capture.txt     written last, so a folder that has it is complete. "key = value" lines under [sections]: when,
//                   versions, the area, Link's position and rotation, the camera of the frame on screen and the
//                   game's own, the window and picture, frame times around the press, the machine, and the settings
//                   that differ from the defaults
//   frame.png       the presented frame as the window got it, overlay included, without this capture's own note
//   frames.tsv      the last RING presents before the press and the POST after it: frame, interval, replay time
//   log-tail.txt    the end of this run's log
//   settings.ini    every setting with a value, as nx2navi.ini lines, without the private ones (cfg_private.h): what a
//                   replay runs with
//   NX2navi-save-*.zip   the save the game would load next (save_share.c), which a replay starts from
//   flags.tsv, texts.tsv   the game's on/off flags and its 64-character texts, by the hash of their names
//   flight/         the flight recorder's dump of the same moment, when flight = 1 (flight.c)
//
// The press is taken on the render thread (bugcap_frame), which reads what cannot wait: the position, the camera of
// the frame being shown, the frame times. It asks the present path for the next frame's pixels (vk_present_grab) and
// the game's camera update for its own choice (dbg_camera_watch), and a thread of its own waits for both, writes the
// files and then lets the note show. One capture at a time: a press while one is being written is logged and dropped.
//
// A replay (NX2NAVI_CAPTURE_REPLAY=<folder>) is a test run: cfg.c takes the folder's settings.ini in place of
// nx2navi.ini, the environment still on top, and writes no settings file. On the game's thread bugcap_replay_pad
// presses A until the save is loaded and bugcap_replay_tick then goes where the capture was taken with the debug
// menu's own tools (debug_tools.c): the game's change of area to the start position nearest to the place, Link's
// walk to it, the free camera at the captured camera. Link is walked, not put: his position cannot be written
// (debug_tools.c). At the end the replay takes a capture of its own, into <folder>/replays unless NX2NAVI_CAPTURE_DIR
// says otherwise, whose capture.txt names the original and how far from it Link ended.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bugcap.h"
#include "cfg.h"
#include "cfg_private.h"
#include "debug_tools.h"
#include "flight.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "perfmon.h"
#include "platform/platform.h"
#include "save_share.h"
#include "sysinfo.h"
#include "ui/menu.h"
#include "vk.h"
#include "vk_dlaa.h"

#define RING 600   // presents kept before a press: ten seconds at 60 a second
#define POST 30    // presents recorded after it
#define NOTE_NS 6000000000ull
#define LOG_TAIL (256u << 10)

typedef struct { uint64_t frame; uint32_t interval_us, replay_us; } FrameRec;

// The capture being taken. Filled by the render thread at the press, then read by the writer; grab comes from the
// present path and post from the render thread meanwhile, each behind its own count.
static struct {
    int number;
    char name[64], dir[PLAT_PATH_MAX], why[160];
    struct tm local;
    uint64_t unix_ns, frame, start_ns;
    int have_pos, have_rot, have_area, have_cam, menu_open;
    float pos[3], rot[9], speed;
    char area[64];
    VkDlaaCamera cam;
    int fc_on, fc_sticks;
    float fc_speed, fc_yaw, fc_pitch;
    double fc_pos[3];
    uint64_t seen0[2];           // the game's camera updates seen before this capture watched them
    PerfStats perf;
    FrameRec pre[RING];
    int pre_n, flight;
    FrameRec post[POST];
    volatile int post_n;
    VkGrab *grab;
    volatile int grab_done;
    char replay_note[400];       // a replay's capture: what it was a replay of and how close it came
} job;
static volatile int32_t busy;    // a capture is being taken or written

static FrameRec ring[RING];      // the render thread's
static uint64_t ring_n;

static volatile uint64_t note_until;   // the note shows until then (plat_now_ns); the three below are set before it
static int note_number, note_ok;
static char note_name[64];

// ---- where captures go
static struct {
    int state;                   // 0 not looked at, 1 a replay, -1 none
    char dir[PLAT_PATH_MAX], name[64];
    char text[16384];            // the replayed capture.txt
} rp_src;

static void replay_source(void) {
    if (rp_src.state) return;
    const char *e = getenv("NX2NAVI_CAPTURE_REPLAY");   // the environment only: a replay is one run, never a saved setting
    if (!e || !*e) {
        rp_src.state = -1;
        return;
    }
    char path[PLAT_PATH_MAX + 16];
    snprintf(rp_src.dir, sizeof rp_src.dir, "%s", e);
    size_t n = strlen(rp_src.dir);
    if (n > 12 && !strcmp(rp_src.dir + n - 12, "/capture.txt")) rp_src.dir[n - 12] = 0;
    else if (n > 12 && !strcmp(rp_src.dir + n - 12, "\\capture.txt")) rp_src.dir[n - 12] = 0;
    for (n = strlen(rp_src.dir); n && (rp_src.dir[n - 1] == '/' || rp_src.dir[n - 1] == '\\'); n--) rp_src.dir[n - 1] = 0;
    const char *base = rp_src.dir;
    for (const char *p = rp_src.dir; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    snprintf(rp_src.name, sizeof rp_src.name, "%s", base);
    snprintf(path, sizeof path, "%s/capture.txt", rp_src.dir);
    uint64_t size = 0;
    uint8_t *data = plat_file_read(path, &size);
    if (data) snprintf(rp_src.text, sizeof rp_src.text, "%.*s", (int)(size < sizeof rp_src.text - 1 ? size : sizeof rp_src.text - 1), (const char *)data);
    free(data);
    rp_src.state = 1;
}

const char *bugcap_replay_dir(void) {
    replay_source();
    return rp_src.state > 0 ? rp_src.dir : NULL;
}

static void base_dir(char *out, size_t cap) {
    const char *e = cfg_get("NX2NAVI_CAPTURE_DIR");
    char logs[PLAT_PATH_MAX];
    if (e && *e) snprintf(out, cap, "%s", e);
    else if (bugcap_replay_dir()) snprintf(out, cap, "%s/replays", rp_src.dir);
    else if (sysinfo_log_folder(logs, sizeof logs)) snprintf(out, cap, "%s/captures", logs);
    else snprintf(out, cap, "captures");
}

static int highest(const char *name, int kind, uint64_t size, void *ctx) {
    (void)size;
    int n = 0, digits = 0;
    while (name[digits] >= '0' && name[digits] <= '9' && digits < 6) n = n * 10 + (name[digits++] - '0');
    if (kind == PLAT_FS_DIR && digits && name[digits] == '-' && n > *(int *)ctx) *(int *)ctx = n;
    return 1;
}

// ---- the writer
typedef struct { FILE *f; } Txt;

static void put(Txt *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(Txt *t, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(t->f, fmt, ap);
    va_end(ap);
}

static FILE *open_in(const char *name, const char *mode) {
    char path[PLAT_PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/%s", job.dir, name);
    FILE *f = fopen(path, mode);
    if (!f) rt_log("capture: cannot write %s", path);
    return f;
}

static int by_interval(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static void put_timing(Txt *t) {
    static uint32_t sorted[RING];
    int n = 0, over33 = 0, over50 = 0;
    uint64_t sum = 0, rep = 0;
    for (int i = 0; i < job.pre_n; i++) {
        uint32_t us = job.pre[i].interval_us;
        if (!us) continue;
        sorted[n++] = us, sum += us, rep += job.pre[i].replay_us;
        over33 += us > 33400, over50 += us > 50000;
    }
    put(t, "\n[timing]\n");
    if (!n) {
        put(t, "frames = 0\n");
        return;
    }
    qsort(sorted, (size_t)n, sizeof *sorted, by_interval);
    put(t, "frames = %d\nseconds = %.2f\nfps_average = %.1f\n", n, sum / 1e6, n * 1e6 / (double)sum);
    put(t, "frame_ms_median = %.2f\nframe_ms_95 = %.2f\nframe_ms_99 = %.2f\nframe_ms_longest = %.2f\n", sorted[n / 2] / 1e3,
        sorted[n * 95 / 100] / 1e3, sorted[n * 99 / 100] / 1e3, sorted[n - 1] / 1e3);
    put(t, "frames_over_33ms = %d\nframes_over_50ms = %d\nreplay_ms_average = %.2f\n", over33, over50, rep / 1e3 / n);
    put(t, "last_30_before_ms =");
    for (int i = job.pre_n > 30 ? job.pre_n - 30 : 0; i < job.pre_n; i++) put(t, " %.1f", job.pre[i].interval_us / 1e3);
    int post_n = __atomic_load_n(&job.post_n, __ATOMIC_ACQUIRE);
    put(t, "\nfirst_%d_after_ms =", post_n);
    for (int i = 0; i < post_n; i++) put(t, " %.1f", job.post[i].interval_us / 1e3);
    put(t, "\n");
    const PerfStats *p = &job.perf;
    if (p->fps > 0) {   // the performance overlay was on: its numbers of the last second
        put(t, "overlay_fps = %.1f\noverlay_1pct_low_fps = %.1f\noverlay_frame_ms_max = %.2f\n", p->fps, p->low1_fps, p->frame_ms_max);
        put(t, "overlay_cpu_pct = %.0f (the game %.0f)\noverlay_gpu_pct = %.0f\noverlay_vram_mb = %.0f of %.0f (the game %.0f)\n",
            p->cpu_total_pct, p->cpu_game_pct, p->gpu_pct, p->vram_used_mb, p->vram_total_mb, p->vram_game_mb);
        put(t, "overlay_replay_ms = %.2f\noverlay_gpu_ms = %.2f\noverlay_pipeline_wait_ms = %.2f\noverlay_texture_ms = %.2f\n",
            p->replay_ms, p->gpu_ms, p->pipe_wait_ms, p->tex_ms);
    }
}

static void write_frames(void) {
    FILE *f = open_in("frames.tsv", "w");
    if (!f) return;
    fprintf(f, "# presents around bug capture %d, taken at frame %llu\nframe\tinterval_ms\treplay_ms\twhen\n", job.number,
            (unsigned long long)job.frame);
    for (int i = 0; i < job.pre_n; i++)
        fprintf(f, "%llu\t%.2f\t%.2f\t%s\n", (unsigned long long)job.pre[i].frame, job.pre[i].interval_us / 1e3,
                job.pre[i].replay_us / 1e3, job.pre[i].frame == job.frame ? "press" : "before");
    int post_n = __atomic_load_n(&job.post_n, __ATOMIC_ACQUIRE);
    for (int i = 0; i < post_n; i++)
        fprintf(f, "%llu\t%.2f\t%.2f\tafter\n", (unsigned long long)job.post[i].frame, job.post[i].interval_us / 1e3,
                job.post[i].replay_us / 1e3);
    fclose(f);
}

// The end of the log: the last LOG_TAIL bytes from the first whole line on.
static void write_log_tail(void) {
    char path[PLAT_PATH_MAX];
    const char *log = rt_log_path();
    if (log) snprintf(path, sizeof path, "%s", log);
    else if (!plat_stderr_path(path, sizeof path)) return;
    rt_log_sync();
    // the game holds the log open for writing: the C library's open shares it, where plat_file_open is refused
    FILE *in = fopen(path, "rb");
    if (!in) return;
    plat_fseek64(in, 0, SEEK_END);
    int64_t size = plat_ftell64(in);
    int64_t from = size > (int64_t)LOG_TAIL ? size - (int64_t)LOG_TAIL : 0;
    size_t n = (size_t)(size - from);
    char *buf = n ? malloc(n) : NULL;
    int64_t got = buf && !plat_fseek64(in, from, SEEK_SET) ? (int64_t)fread(buf, 1, n, in) : -1;
    fclose(in);
    FILE *f = got > 0 ? open_in("log-tail.txt", "wb") : NULL;
    if (f) {
        const char *p = buf;
        if (from) {
            const char *line = memchr(buf, '\n', (size_t)got);
            if (line) p = line + 1;
        }
        fprintf(f, "# the last %lld of %lld bytes of %s\n", (long long)(got - (p - buf)), (long long)size, path);
        fwrite(p, 1, (size_t)(got - (p - buf)), f);
        fclose(f);
    }
    free(buf);
}

// Keys that are of one run and not of the game's settings: what a test run's harness sets (scripted input, shots,
// dumps, the window kept in the background). capture.txt lists them; settings.ini, which a replay runs with, leaves
// them out.
static int run_key(const char *key) {
    static const char *const starts[] = {"MENU_TEST", "SHOT_", "SHOTS", "DUMP", "PROFILE", "DBG_TEST"};
    static const char *const whole[] = {"INPUT", "INPUT_SCRIPT", "INPUT_RECORD", "ASSUME_FOCUS", "BACKGROUND_WINDOW",
                                        "FLIGHT_AT", "FLIGHT_EVERY", "FLIGHT_DIR", "TRACE"};
    for (size_t i = 0; i < sizeof starts / sizeof *starts; i++)
        if (!strncmp(key, starts[i], strlen(starts[i]))) return 1;
    for (size_t i = 0; i < sizeof whole / sizeof *whole; i++)
        if (!strcmp(key, whole[i])) return 1;
    return 0;
}

typedef struct { FILE *ini; Txt *txt; int differ; } Settings;

static void setting_cb(const char *key, const char *value, int from_env, void *ctx) {
    Settings *s = ctx;
    if (!strncmp(key, "CAPTURE_", 8)) return;   // a replay's own keys
    char lower[80], var[96];
    snprintf(lower, sizeof lower, "%s", key);
    for (char *p = lower; *p; p++) *p = *p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : *p;
    snprintf(var, sizeof var, "NX2NAVI_%s", key);
    if (!from_env && cfg_from_env(var)) return;   // the file's line of a key the environment sets too: that one counts
    int private = cfg_private_key(key);
    if (s->ini && !private && *value && !run_key(key)) fprintf(s->ini, "%s = %s\n", lower, value);
    if (!s->txt || !*value) return;
    const char *def = cfg_default(key);
    if (def && *def && !strcmp(def, value)) return;   // set, to what it would be anyway
    put(s->txt, "%s = %s%s\n", lower, private ? CFG_PRIVATE_SHOWN : value,
        from_env ? "   # from the environment" : def && *def ? "" : "   # no fixed default");
    s->differ++;
}

static void text_cb(uint32_t hash, const char *text, void *ctx) {
    static const char *const names[] = {"Sequence_CurrentBanc", "Sequence_ReservedNextBanc", "Sequence_ReservedNextStartPosName",
                                        "Sequence_ReservedBootEventName", "Sequence_ReservedEventStarterName"};
    const char *name = "";
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (dbg_name_hash(names[i]) == hash) name = names[i];
    if (*text || *name) fprintf(ctx, "%08x\t%s\t%s\n", hash, text, name);
}

static void write_game_data(void) {
    FILE *f = open_in("texts.tsv", "w");
    if (f) {
        fprintf(f, "# the game's 64-character texts that are not empty, by the hash of their names\nhash\ttext\tname\n");
        dbg_texts(text_cb, f);
        fclose(f);
    }
    int n = dbg_flags_refresh();
    if (n && (f = open_in("flags.tsv", "w"))) {
        fprintf(f, "# the game's on/off flags, by the hash of their names (a name: when the debug menu had read them)\nhash\tvalue\tname\n");
        for (int i = 0; i < n; i++) {
            const char *name = dbg_flag_name(i);
            fprintf(f, "%08x\t%d\t%s\n", dbg_flag_hash(i), dbg_flag_value(i), name ? name : "");
        }
        fclose(f);
    }
}

static void put_vec(Txt *t, const char *key, const double v[3]) { put(t, "%s = %.4f %.4f %.4f\n", key, v[0], v[1], v[2]); }

// A direction as the free camera's angles (debug_tools.c): it looks along (cos p sin y, sin p, cos p cos y).
static void angles_of(const double d[3], double *yaw, double *pitch) {
    double len = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    *yaw = atan2(d[0], d[2]);
    *pitch = len > 1e-9 ? asin(d[1] / len) : 0;
}

static void put_camera(Txt *t) {
    put(t, "\n[camera]\n");
    // the game's own cameras, as its update chose them in the frames after the press
    static const char *const which[2] = {"game_camera", "event_camera"};
    double gpos[2][3], gat[2][3];
    int fresh[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        float up[3];
        fresh[i] = dbg_camera_seen(i, gpos[i], gat[i], up) > job.seen0[i];
    }
    double pos[3], dir[3], yaw = 0, pitch = 0;
    const char *source = "none";
    if (job.have_cam) {   // what the frame on screen was drawn with
        for (int i = 0; i < 3; i++) pos[i] = job.cam.pos[i], dir[i] = job.cam.fwd[i];
        source = "the frame on screen (the upscaler's camera)";
    } else if (job.fc_on) {
        memcpy(pos, job.fc_pos, sizeof pos);
        yaw = job.fc_yaw, pitch = job.fc_pitch;
        dir[0] = cos(pitch) * sin(yaw), dir[1] = sin(pitch), dir[2] = cos(pitch) * cos(yaw);
        source = "the free camera";
    } else if (fresh[0] || fresh[1]) {
        int k = fresh[0] ? 0 : 1;
        for (int i = 0; i < 3; i++) pos[i] = gpos[k][i], dir[i] = gat[k][i] - gpos[k][i];
        source = k ? "the game's camera for events" : "the game's camera that follows Link";
    }
    put(t, "camera_source = %s\n", source);
    if (strcmp(source, "none")) {
        angles_of(dir, &yaw, &pitch);
        double len = sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        for (int i = 0; i < 3; i++) dir[i] = len > 1e-9 ? dir[i] / len : 0;
        put_vec(t, "camera_pos", pos);
        put_vec(t, "camera_dir", dir);
        put(t, "camera_yaw = %.5f\ncamera_pitch = %.5f\n", yaw, pitch);
    }
    if (job.have_cam) {
        put(t, "camera_up = %.4f %.4f %.4f\n", job.cam.up[0], job.cam.up[1], job.cam.up[2]);
        put(t, "camera_fov_y_degrees = %.2f\ncamera_aspect = %.4f\ncamera_near = %.4f\n", job.cam.fov_y * 57.29578f,
            job.cam.aspect, job.cam.znear);
        if (job.cam.infinite) put(t, "camera_far = none\n");
        else put(t, "camera_far = %.1f\n", job.cam.zfar);
    }
    for (int i = 0; i < 2; i++) {
        if (!fresh[i]) continue;
        char key[40];
        snprintf(key, sizeof key, "%s_pos", which[i]);
        put_vec(t, key, gpos[i]);
        snprintf(key, sizeof key, "%s_target", which[i]);
        put_vec(t, key, gat[i]);
    }
    put(t, "free_camera = %s\n", job.fc_on ? job.fc_sticks ? "on, flown with the sticks" : "on, standing" : "off");
    if (job.fc_on) {
        put_vec(t, "free_camera_pos", job.fc_pos);
        put(t, "free_camera_yaw = %.5f\nfree_camera_pitch = %.5f\n", job.fc_yaw, job.fc_pitch);
    }
}

static void grab_cb(VkGrab *g, void *ctx) {
    (void)ctx;
    job.grab = g;
    __atomic_store_n(&job.grab_done, 1, __ATOMIC_RELEASE);
}

static void writer(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_BELOW_NORMAL);
    plat_file_mkdir_p(job.dir);
    int ok = plat_dir_exists(job.dir);
    if (!ok) rt_log("capture: cannot create %s", job.dir);
    // the frame's pixels come with the next present, the game's camera with its next update, the frames after the
    // press as they are shown: a second at most for all three
    while (plat_now_ns() - job.start_ns < 2000000000ull && !__atomic_load_n(&job.grab_done, __ATOMIC_ACQUIRE)) plat_sleep_ms(2);
    if (!__atomic_load_n(&job.grab_done, __ATOMIC_ACQUIRE)) vk_present_grab_cancel();
    while (plat_now_ns() - job.start_ns < 700000000ull && __atomic_load_n(&job.post_n, __ATOMIC_ACQUIRE) < POST) plat_sleep_ms(5);
    char shot[160] = "none: the frame could not be read back";
    VkGrab *g = job.grab;
    if (ok && g) {
        uint8_t *rgba = malloc((size_t)g->w * (size_t)g->h * 4);
        if (rgba) {
            char path[PLAT_PATH_MAX + 16];
            vk_present_grab_rgba(g, rgba);
            snprintf(path, sizeof path, "%s/frame.png", job.dir);
            if (flight_write_png(path, rgba, (uint32_t)g->w, (uint32_t)g->h))
                snprintf(shot, sizeof shot, "frame.png (%dx%d%s)", g->w, g->h, g->hdr ? ", HDR10 converted to sRGB" : "");
            else snprintf(shot, sizeof shot, "none: frame.png could not be written");
            free(rgba);
        }
    }
    FILE *f = ok ? open_in("capture.txt.part", "w") : NULL;
    if (f) {
        Txt t = {f};
        struct tm u;
        plat_gmtime((int64_t)(job.unix_ns / 1000000000u), &u);
        put(&t, "# NX2navi bug capture %d. docs/bug-capture.md says how to start a run from it.\n[capture]\n", job.number);
        put(&t, "number = %d\nname = %s\n", job.number, job.name);
        put(&t, "time = %04d-%02d-%02d %02d:%02d:%02d\ntime_utc = %04d-%02d-%02dT%02d:%02d:%02dZ\n", job.local.tm_year + 1900,
            job.local.tm_mon + 1, job.local.tm_mday, job.local.tm_hour, job.local.tm_min, job.local.tm_sec, u.tm_year + 1900,
            u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec);
        put(&t, "trigger = %s\nnx2navi = %s\ngame = Ocarina of Time %d.%d.%d\n", job.why, rt_port_version, rt_game_version / 100,
            rt_game_version / 10 % 10, rt_game_version % 10);
        put(&t, "frame = %llu\nrun_seconds = %.1f\nmenu_open = %d\n", (unsigned long long)job.frame,
            plat_process_uptime_ns() / 1e9, job.menu_open);
        if (job.replay_note[0]) put(&t, "%s", job.replay_note);

        put(&t, "\n[place]\n");
        if (job.have_area) put(&t, "area = %s\n", job.area);
        else put(&t, "area = unknown\n");
        if (job.have_pos) {
            put(&t, "player_pos = %.4f %.4f %.4f\n", job.pos[0], job.pos[1], job.pos[2]);
            char start[64];
            float dist = 0;
            if (job.have_area && dbg_place_nearest(job.area, job.pos, start, &dist))
                put(&t, "nearest_start = %s\nnearest_start_metres = %.1f\n", start, dist);
        } else {
            put(&t, "player_pos = none\n# no player: the title screen, a loading screen, or a moment without Link\n");
        }
        if (job.have_rot) {
            put(&t, "player_rot =");
            for (int i = 0; i < 9; i++) put(&t, " %.5f", job.rot[i]);
            // the matrix's third column is the way he faces, as the free camera's yaw counts it
            put(&t, "\nplayer_yaw = %.5f\n", atan2(job.rot[2], job.rot[8]));
        }
        if (job.speed != 1.0f) put(&t, "game_speed = %.3f\n", job.speed);

        put_camera(&t);

        put(&t, "\n[picture]\nshot = %s\n", shot);
        if (g) {
            put(&t, "window = %dx%d\ngame_image = %dx%d\nhdr = %d\n", g->w, g->h, g->image_w, g->image_h, g->hdr);
            static const char *const scalers[] = {"none", "bilinear", "FSR 1", "bilinear and sharpening"};
            put(&t, "scaled_by = %s\n", scalers[g->scaler & 3]);
            put(&t, "picture_rect = %.4f %.4f %.4f %.4f\n", g->pic[0], g->pic[1], g->pic[2], g->pic[3]);
        }
        put(&t, "present_mode = %s\ndisplay_hz = %.1f\n", gfx_window_present_mode(), gfx_window_refresh_hz());
        if (gfx_output_fps() > 0) put(&t, "frame_generation_output_fps = %.1f\n", gfx_output_fps());
        put(&t, "flight_recorder = %s\n", job.flight ? "flight/ (frames.tsv, events.tsv, thumbnails)" : flight_on ? "on, busy with another dump" : "off");

        put_timing(&t);

        put(&t, "\n[system]\n");
        static char sys[16384];
        sysinfo_text(sys, sizeof sys);
        char *cut = strstr(sys, "Settings with a value:");
        if (cut) *cut = 0;
        for (char *line = sys, *end; line && *line; line = end ? end + 1 : NULL) {   // "OS: ..." as "os = ..."
            if ((end = strchr(line, '\n'))) *end = 0;
            char *colon = strstr(line, ": ");
            if (!colon) continue;
            *colon = 0;
            for (char *p = line; *p; p++) *p = *p >= 'A' && *p <= 'Z' ? (char)(*p + 32) : *p;
            put(&t, "%s = %s\n", line, colon + 2);
        }

        put(&t, "\n[settings that differ from the defaults]\n");
        Settings s = {open_in("settings.ini", "w"), &t, 0};
        if (s.ini)
            fprintf(s.ini, "# The settings bug capture %d ran with, as nx2navi.ini lines: every setting that had a value, from the\n"
                           "# file, the menu or the environment, without the private ones. NX2NAVI_CAPTURE_REPLAY reads it.\n", job.number);
        cfg_each_active(setting_cb, &s);
        if (s.ini) fclose(s.ini);
        if (!s.differ) put(&t, "# none\n");
        fclose(f);
    }
    if (ok) {
        write_frames();
        write_log_tail();
        write_game_data();
        char zip[PLAT_PATH_MAX];
        save_share_export(job.dir, 0, zip, sizeof zip);
        // capture.txt appears last and whole
        char part[PLAT_PATH_MAX + 32], done[PLAT_PATH_MAX + 32];
        snprintf(part, sizeof part, "%s/capture.txt.part", job.dir);
        snprintf(done, sizeof done, "%s/capture.txt", job.dir);
        ok = f && plat_file_replace(part, done, 0);
    }
    vk_present_grab_free(g);
    job.grab = NULL;
    if (ok) rt_log("capture: %d saved to %s (%s; %s; %.0f ms)", job.number, job.dir, job.why, shot, (plat_now_ns() - job.start_ns) / 1e6);
    else rt_log("capture: %d could not be written to %s", job.number, job.dir);
    note_number = job.number, note_ok = ok;
    snprintf(note_name, sizeof note_name, "%s", job.name);
    __atomic_store_n(&note_until, plat_now_ns() + NOTE_NS, __ATOMIC_RELEASE);
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}

int bugcap_note(int *number, int *ok, char name[64]) {
    uint64_t until = __atomic_load_n(&note_until, __ATOMIC_ACQUIRE);
    if (!until || plat_now_ns() >= until) return 0;
    *number = note_number, *ok = note_ok;
    snprintf(name, 64, "%s", note_name);
    return 1;
}

// ---- the press (render thread)
static volatile int32_t replay_wants;   // the replay's own capture, asked for by the game's thread
static char replay_note[400];           // ... with these lines for its capture.txt, written before replay_wants

static void start(uint64_t frame, uint64_t interval_ns, const char *why) {
    int32_t idle = 0;
    if (!__atomic_compare_exchange_n(&busy, &idle, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        rt_log("capture: %s at frame %llu while capture %d is being written: not taken", why, (unsigned long long)frame, job.number);
        return;
    }
    job.start_ns = plat_now_ns();
    job.frame = frame;
    job.unix_ns = plat_unix_time_ns();
    plat_local_time(&job.local);
    snprintf(job.why, sizeof job.why, "%s", why);
    char base[PLAT_PATH_MAX];
    base_dir(base, sizeof base);
    int top = 0;
    plat_fs_list(base, highest, &top);
    job.number = top + 1;
    snprintf(job.name, sizeof job.name, "%03d-%04d%02d%02d-%02d%02d%02d", job.number, job.local.tm_year + 1900,
             job.local.tm_mon + 1, job.local.tm_mday, job.local.tm_hour, job.local.tm_min, job.local.tm_sec);
    snprintf(job.dir, sizeof job.dir, "%s/%s", base, job.name);
    // what the game holds now: the writer comes later
    job.have_pos = dbg_player_pos(job.pos);
    job.have_rot = dbg_player_rot(job.rot);
    job.have_area = dbg_area(job.area);
    job.speed = dbg_speed_factor();
    job.have_cam = vk_dlaa_fg_camera(&job.cam);
    job.fc_on = dbg_freecam(&job.fc_sticks, &job.fc_speed, job.fc_pos);
    dbg_freecam_angles(&job.fc_yaw, &job.fc_pitch);
    job.menu_open = menu_is_open();
    perfmon_snapshot(&job.perf);
    job.pre_n = ring_n < RING ? (int)ring_n : RING;
    for (int i = 0; i < job.pre_n; i++) job.pre[i] = ring[(ring_n - (uint64_t)job.pre_n + (uint64_t)i) % RING];
    __atomic_store_n(&job.post_n, 0, __ATOMIC_RELEASE);
    job.replay_note[0] = 0;
    if (!strncmp(why, "replay", 6)) snprintf(job.replay_note, sizeof job.replay_note, "%s", replay_note);
    for (int i = 0; i < 2; i++) {
        double p[3], a[3];
        float up[3];
        job.seen0[i] = dbg_camera_seen(i, p, a, up);
    }
    dbg_camera_watch(1);
    job.grab = NULL;
    __atomic_store_n(&job.grab_done, 0, __ATOMIC_RELEASE);
    if (!vk_present_grab(grab_cb, NULL)) __atomic_store_n(&job.grab_done, 1, __ATOMIC_RELEASE);
    char fdir[PLAT_PATH_MAX + 16];
    snprintf(fdir, sizeof fdir, "%s/flight", job.dir);
    job.flight = flight_capture(frame, interval_ns, fdir);
    rt_log("capture: %d at frame %llu (%s): %s, Link at %.2f %.2f %.2f%s", job.number, (unsigned long long)frame, why,
           job.have_area ? job.area : "no area", job.pos[0], job.pos[1], job.pos[2], job.have_pos ? "" : " (no player)");
    if (!plat_thread_spawn(writer, NULL, "bug capture", 0)) {
        rt_log("capture: cannot start the thread that writes it");
        vk_present_grab_cancel();
        dbg_camera_watch(0);
        __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
    }
}

void bugcap_frame(uint64_t frame, uint64_t now_ns, uint64_t interval_ns, uint64_t replay_ns) {
    (void)now_ns;
    FrameRec r = {frame, (uint32_t)(interval_ns / 1000), (uint32_t)(replay_ns / 1000)};
    ring[ring_n++ % RING] = r;
    static int watch_frames;
    if (__atomic_load_n(&busy, __ATOMIC_ACQUIRE)) {
        int n = job.post_n;
        if (n < POST && frame > job.frame) job.post[n] = r, __atomic_store_n(&job.post_n, n + 1, __ATOMIC_RELEASE);
        // the game's camera update has had its frames: the patch site goes quiet again
        if (watch_frames >= 0 && ++watch_frames > 8) dbg_camera_watch(0), watch_frames = -1;
    } else {
        if (watch_frames > 0) dbg_camera_watch(0);
        watch_frames = 0;
    }
    static uint64_t at[16];
    static int n_at = -1;
    if (n_at < 0) {   // NX2NAVI_CAPTURE_AT=<frame>[,<frame>...]: captures without a key, for test runs
        n_at = 0;
        for (const char *p = cfg_get("NX2NAVI_CAPTURE_AT"); p && *p && n_at < 16;) {
            char *end;
            unsigned long long f = strtoull(p, &end, 10);
            if (end == p) break;
            at[n_at++] = f;
            p = *end == ',' ? end + 1 : end;
        }
    }
    static int key_was;
    int key = input_flight_key();
    const char *why = key && !key_was ? "F8" : NULL;
    key_was = key;
    for (int i = 0; i < n_at; i++)
        if (at[i] == frame) why = "NX2NAVI_CAPTURE_AT";
    if (__atomic_exchange_n(&replay_wants, 0, __ATOMIC_ACQ_REL)) why = "replay";
    if (why) start(frame, interval_ns, why);
}

// ---- the replay (the game's thread)
// The value of key in the replayed capture.txt ("key = value" at a line's start); 0 without it.
static int src_get(const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    for (const char *p = rp_src.text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL) {
        if (strncmp(p, key, kl) || strncmp(p + kl, " = ", 3)) continue;
        const char *v = p + kl + 3, *e = v;
        while (*e && *e != '\n' && *e != '\r') e++;
        snprintf(out, cap, "%.*s", (int)(e - v), v);
        return 1;
    }
    return 0;
}

enum { R_NEW, R_WAIT, R_LIST, R_WARP, R_ROUTE, R_ROUTING, R_CAMERA, R_CAMERA_PUT, R_SETTLE, R_DONE };
static struct {
    int state, tries, have_pos, have_cam, cam_on, routed, walks, held_seen;
    char area[64];
    float pos[3], yaw, pitch;
    double cam[3];
    uint64_t t_ns, tick;
    float still_at[3];        // R_ROUTING: where Link was when he last moved, and when
    uint64_t still_ns;
} rp;
static volatile int32_t rp_loaded;   // Link has appeared: bugcap_replay_pad stops pressing A
static volatile int32_t rp_held;     // Link stands still on his walk (a conversation holds him): it presses A again

void bugcap_replay_pad(uint64_t *buttons) {
    static uint64_t reads;
    if (!bugcap_replay_dir()) return;
    reads++;
    if (__atomic_load_n(&rp_loaded, __ATOMIC_RELAXED)) {
        // a conversation that began where the teleport arrived (the game's own, on a save that had not been there):
        // A takes it to its end
        if (__atomic_load_n(&rp_held, __ATOMIC_RELAXED) && reads % 60 < 4) *buttons |= 1ull << B_A;
        return;
    }
    // through the title screen and the file menu: A for four reads, every hundred, from the fifth second on
    if (reads > 300 && reads % 100 < 4) *buttons |= 1ull << B_A;
}

static void rp_go(int state) { rp.state = state, rp.t_ns = plat_now_ns(); }

static void rp_fail(const char *what) {
    rt_log("capture: replay of %s stopped: %s", rp_src.name, what);
    rp_go(R_DONE);
}

void bugcap_replay_tick(uint64_t tick) {
    if (rp.state == R_DONE || !bugcap_replay_dir()) return;
    uint64_t now = plat_now_ns();
    double in_state = (double)(now - rp.t_ns) / 1e9;
    char text[256], area[64] = "";
    float p[3] = {0, 0, 0};
    switch (rp.state) {
    case R_NEW:
        __atomic_store_n(&rp_loaded, 1, __ATOMIC_RELAXED);
        if (!rp_src.text[0]) {
            rp_fail("its capture.txt cannot be read");
            return;
        }
        if (src_get("area", rp.area, sizeof rp.area) && !strcmp(rp.area, "unknown")) rp.area[0] = 0;
        rp.have_pos = src_get("player_pos", text, sizeof text) && sscanf(text, "%f %f %f", &rp.pos[0], &rp.pos[1], &rp.pos[2]) == 3;
        rp.have_cam = src_get("camera_pos", text, sizeof text) && sscanf(text, "%lf %lf %lf", &rp.cam[0], &rp.cam[1], &rp.cam[2]) == 3 &&
                      src_get("camera_yaw", text, sizeof text) && sscanf(text, "%f", &rp.yaw) == 1 &&
                      src_get("camera_pitch", text, sizeof text) && sscanf(text, "%f", &rp.pitch) == 1;
        const char *cam = getenv("NX2NAVI_CAPTURE_REPLAY_CAMERA");   // 0: the game's own camera stays
        rp.cam_on = !(cam && !strcmp(cam, "0"));
        rt_log("capture: replay of %s: %s, Link at %.2f %.2f %.2f%s, camera %s", rp_src.name, rp.area[0] ? rp.area : "no area",
               rp.pos[0], rp.pos[1], rp.pos[2], rp.have_pos ? "" : " (not in the capture)",
               !rp.have_cam ? "not in the capture" : rp.cam_on ? "to be placed" : "left to the game");
        if (!rp.area[0]) {
            rp_fail("the capture names no area (taken at the title screen or while loading): only its settings apply");
            return;
        }
        rp_go(R_WAIT);
        break;
    case R_WAIT:   // the save has just loaded: the game takes no change of area in its first moments
        if (in_state < 3) break;
        dbg_flags_refresh(), dbg_names(NULL);   // the start positions, read with the flag names
        rp_go(R_LIST);
        break;
    case R_LIST:
        if (dbg_names(NULL) != 2) {
            dbg_flags_refresh();
            if (in_state > 60) rp_fail("the game's list of start positions was not read in a minute");
            break;
        }
        if (dbg_area(area) && !strcmp(area, rp.area) && rp.have_pos && dbg_player_pos(p) &&
            fabsf(p[0] - rp.pos[0]) + fabsf(p[1] - rp.pos[1]) + fabsf(p[2] - rp.pos[2]) < 60) {
            rt_log("capture: replay: the save starts in %s, %.1f %.1f %.1f: no teleport", area, p[0], p[1], p[2]);
            rp_go(R_ROUTE);
            break;
        }
        // fall through
    case R_WARP:
        if (rp.state == R_WARP) {
            if (dbg_busy() || in_state < 1) {
                if (in_state > 180) rp_fail("the teleport did not end in three minutes");
                break;
            }
            if (dbg_area(area) && !strcmp(area, rp.area)) {
                rp_go(R_ROUTE);
                break;
            }
            if (in_state < 4) break;   // a refusal (an event still runs after the load): asked again in a moment
            dbg_status(text);
            if (++rp.tries >= 6) {
                rt_log("capture: replay: the game's last word on the teleport: %s", text);
                rp_fail("the area could not be reached");
                break;
            }
        }
        {
            char start[64] = "";
            float dist = 0;
            if (rp.have_pos) dbg_place_nearest(rp.area, rp.pos, start, &dist);
            rt_log("capture: replay: teleport to %s, %s%s", rp.area, start[0] ? start : "its first start position",
                   rp.tries ? " (again)" : "");
            dbg_warp(rp.area, start);
            rp_go(R_WARP);
        }
        break;
    case R_ROUTE:
        if (in_state < 1) break;
        if (rp.have_pos && dbg_player_pos(p) && hypotf(p[0] - rp.pos[0], p[2] - rp.pos[2]) > 0.5f) {
            dbg_route_to(rp.pos[0], rp.pos[2], 0.5f);
            rp.routed = 1, rp.walks = 1;
            memcpy(rp.still_at, p, sizeof rp.still_at), rp.still_ns = now;
            rp_go(R_ROUTING);
        } else {
            rp_go(R_CAMERA);
        }
        break;
    case R_ROUTING:
        // Link not moving at all for two seconds is no wall (he would slide along it or sidestep): the game holds him,
        // for a conversation or an event. A is pressed until he moves again, and the walk is asked for once more
        // should the route have given up meanwhile.
        if (dbg_player_pos(p) && fabsf(p[0] - rp.still_at[0]) + fabsf(p[1] - rp.still_at[1]) + fabsf(p[2] - rp.still_at[2]) > 0.05f) {
            memcpy(rp.still_at, p, sizeof rp.still_at), rp.still_ns = now;
            __atomic_store_n(&rp_held, 0, __ATOMIC_RELAXED);
        } else if (now - rp.still_ns > 2000000000ull && !__atomic_load_n(&rp_held, __ATOMIC_RELAXED)) {
            __atomic_store_n(&rp_held, 1, __ATOMIC_RELAXED);
            if (!rp.held_seen++) rt_log("capture: replay: Link does not move (a conversation or an event holds him): pressing A until he does");
        }
        if (dbg_route_state() == 1 && in_state < 120) break;
        if (dbg_route_state() == 2 && rp.held_seen && rp.walks < 3 && in_state < 120) {   // given up while he was held
            dbg_route_to(rp.pos[0], rp.pos[2], 0.5f);
            rp.walks++;
            break;
        }
        if (dbg_route_state() == 1) dbg_route_stop();
        __atomic_store_n(&rp_held, 0, __ATOMIC_RELAXED);
        rp_go(R_CAMERA);
        break;
    case R_CAMERA:
        if (in_state < 1) break;   // Link has stopped
        if (rp.have_cam && rp.cam_on && dbg_freecam_available()) {
            dbg_freecam_set(1, 0, 8.0f);   // standing: the pad still plays the game
            rp.tick = tick;
            rp_go(R_CAMERA_PUT);
        } else {
            rp.cam_on = 0;
            rp_go(R_SETTLE);
        }
        break;
    case R_CAMERA_PUT:
        if (tick < rp.tick + 3) break;   // its first update took the game's view: now the captured one
        dbg_freecam_place(rp.cam, rp.yaw, rp.pitch);
        rp_go(R_SETTLE);
        break;
    case R_SETTLE: {
        if (in_state < 3) break;   // the picture's history and the area's detail settle
        int have = dbg_player_pos(p);
        float off = have && rp.have_pos ? sqrtf((p[0] - rp.pos[0]) * (p[0] - rp.pos[0]) + (p[1] - rp.pos[1]) * (p[1] - rp.pos[1]) +
                                                (p[2] - rp.pos[2]) * (p[2] - rp.pos[2])) : -1;
        dbg_area(area);
        snprintf(replay_note, sizeof replay_note,
                 "replay_of = %s\nreplay_area = %s\nreplay_player_off_metres = %.2f\nreplay_walk = %s\nreplay_camera = %s\n",
                 rp_src.name, !strcmp(area, rp.area) ? "the capture's" : "another", off,
                 !rp.routed ? "none needed" : dbg_route_state() == 2 ? "given up: something was in the way" : "arrived",
                 rp.cam_on ? "the free camera at the captured camera" : "the game's own");
        rt_log("capture: replay of %s ready: %s, Link at %.2f %.2f %.2f, %.2f m from the captured place (%s); camera: %s",
               rp_src.name, area, p[0], p[1], p[2], off,
               !rp.routed ? "no walk needed" : dbg_route_state() == 2 ? "the walk was given up" : "walked there",
               rp.cam_on ? "the free camera at the captured camera" : "the game's own");
        __atomic_store_n(&replay_wants, 1, __ATOMIC_RELEASE);
        rp_go(R_DONE);
        break;
    }
    }
}
