/* What the audio renderer does with values the game sends that it cannot use (runtime/audren.c update_check,
   request_update, audio_bad; runtime/audio_mixer.c splitters_walk, audio_mixer_routing_check,
   audio_mixer_mix_layout_check, audio_mixer_update_effects and the render-pass effects), with the rest of the runtime
   stubbed and the real effect DSPs (runtime/audio_effects.c, runtime/audio_reverb.c) linked. No game data and no audio
   device: NX2NAVI_AUDIO=none mixes without one.

   Every RequestUpdate goes through renderer_handler with a hand-built input. A refused update must answer 0x5299 (the
   SDK hands that result to the game, which ignores it; the SDK aborts on any result but 0x5299, 0x5499 and 0x5699),
   write no output and change no renderer state. A record, effect or sink the renderer cannot use is left out of an update that is
   otherwise applied, or passed through or skipped in the render pass. Each kind is logged once per renderer. rt_fatal
   prints "FATAL:" and exits with 2, so a value that still ends the process fails the run.

   audio_params_check [-v]                              every check below (-v prints the log lines)
   audio_params_check strict-effect | strict-update     NX2NAVI_AUDIO_STRICT=1: must end with "FATAL: audio:" */
#include "../runtime/audren.c"
#include "../runtime/audio_mixer.c"

#include <stdarg.h>

static int fails;
#define CHECK(ok, ...)                                                                                                 \
    do {                                                                                                               \
        if (!(ok)) {                                                                                                   \
            fails++;                                                                                                   \
            printf("FAIL %s:%d: ", __func__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

/* ---- the runtime around the renderer ---- */
GfxStats gfx_counters;
unsigned rt_trace;
static int strict_setting, verbose;
static int once_lines;                 /* log lines that end with "logged once per kind" */
static char last_once[512];
void rt_log(const char *fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (strstr(line, "logged once per kind")) once_lines++, snprintf(last_once, sizeof last_once, "%s", line);
    if (verbose) printf("  log: %s\n", line);
}
void rt_logf(const char *fmt, ...) { (void)fmt; }
void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("FATAL: ");
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    exit(2);
}
void ipc_backtrace(void) {}
int rt_knob(const char *name, const char *def) { (void)name, (void)def; return 0; }
uint64_t kern_now_ns(void) { return 1; }
uint32_t kern_event_new(void) { return 1; }
uint32_t kern_event_readable(uint32_t writable) { return writable; }
void kern_event_signal(uint32_t writable) { (void)writable; }
void kern_handle_close(uint32_t h) { (void)h; }
int kern_handle_copy(uint32_t h, int kind, void *out, size_t n) { (void)h, (void)kind, (void)out, (void)n; return 0; }
int kern_core_away(void) { return 0; }
void kern_core_back(int *away) { (void)away; }
/* the guest address check of ipc.c ipc_guest, ending the process as it does */
void *ipc_guest(uint64_t addr, uint64_t size) {
    if ((!addr && size) || addr + size < addr || addr + size > RT_ADDRESS_LIMIT)
        rt_fatal("ipc: client buffer %#llx+%#llx", (unsigned long long)addr, (unsigned long long)size);
    return (void *)(uintptr_t)addr;
}
uint32_t ipc_session_new(const char *name, IpcHandler handler, void *state, void (*close)(void *state)) {
    (void)name, (void)handler, (void)state, (void)close;
    return 1;
}
void ipc_unimplemented(const IpcSession *s, const IpcRequest *rq) { (void)s, (void)rq; exit(3); }
void ipc_out(IpcResponse *rs, const void *p, uint32_t n) { (void)rs, (void)p, (void)n; }
const void *ipc_arg(const IpcRequest *rq, uint32_t off, uint32_t n) { (void)rq, (void)off, (void)n; exit(3); }
void menu_sound_mix(float *out, uint32_t frames, int channels, int rate) {
    (void)out, (void)frames, (void)channels, (void)rate;
}
const char *cfg_get(const char *name) { return !strcmp(name, "NX2NAVI_AUDIO") ? "none" : NULL; }
const char *cfg_value(const char *name) { return !strcmp(name, "NX2NAVI_AUDIO_CHANNELS") ? "stereo" : "default"; }
int cfg_bool(const char *name) {
    if (!strcmp(name, "NX2NAVI_AUDIO_RAMP") || !strcmp(name, "NX2NAVI_AUDIO_CATCHUP")) return 1;
    if (!strcmp(name, "NX2NAVI_AUDIO_STRICT")) return strict_setting;
    printf("unexpected setting %s\n", name);
    exit(3);
}
double cfg_double(const char *name) { (void)name; return 100; }
unsigned cfg_version(void) { return 1; }
/* platform: one thread and no render thread (the checks render by hand) */
void plat_mutex_init(PlatMutex *m) { (void)m; }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }
int plat_thread_priority_get(void) { return 0; }
void plat_thread_priority(int p) { (void)p; }
PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    (void)fn, (void)arg, (void)name;
    return NULL;
}
void plat_thread_join(PlatThread *t) { (void)t; }
void plat_wait_ns(uint64_t ns) { (void)ns; }

#ifdef _WIN32
__declspec(dllimport) void *__stdcall VirtualAlloc(void *address, size_t size, unsigned long type,
                                                  unsigned long protect);
#else
#include <sys/mman.h>
#endif
/* `n` bytes of zeroed memory below RT_ADDRESS_LIMIT */
static void *low_alloc(size_t n) {
    for (uint64_t at = 1ull << 32; at + n <= RT_ADDRESS_LIMIT; at += 1ull << 30) {
#ifdef _WIN32
        void *p = VirtualAlloc((void *)(uintptr_t)at, n, 0x3000 /* MEM_COMMIT | MEM_RESERVE */,
                               0x04 /* PAGE_READWRITE */);
#else
        void *p = mmap((void *)(uintptr_t)at, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) p = NULL;
        else if ((uintptr_t)p + n > RT_ADDRESS_LIMIT) munmap(p, n), p = NULL;
#endif
        if (p) return p;
    }
    printf("no memory below the guest address limit\n");
    exit(3);
}

/* ---- a renderer: 2 voices, 2 effect slots, 1 sink, the final mix (2 buffers, the mixer's 0-1) and 1 submix (2
   buffers, 2-3), 1 splitter with 2 destinations, 8 mix buffers. Voice 0 plays a wave buffer of one constant sample
   (0.5) into final mix buffer 0, so an effect from buffer 0 to buffer 1 shows in buffer 1: 0.5 when it passes the sound
   through, 0 when it is skipped. ---- */
#define FRAME 240
#define VOICES 2
#define EFFECTS 2
static int16_t pcm[4 * FRAME];

static AudioRenderer *open_renderer(void) {
    AudrenParameter p = {.sample_rate = 48000, .sample_count = FRAME, .mix_buffer_count = 8, .submix_count = 1,
                         .voice_count = VOICES, .sink_count = 1, .effect_count = EFFECTS, .splitter_count = 1,
                         .splitter_destination_count = 2, .revision = AUDREN_REV(11)};
    for (size_t i = 0; i < sizeof pcm / sizeof *pcm; i++) pcm[i] = 16384;
    AudioRenderer *r = renderer_new(&p);
    r->strict = (uint8_t)cfg_bool("NX2NAVI_AUDIO_STRICT");
    audio_mixer_open(r);
    r->state = 0;
    audio_mixer_set_running(r, 1);
    return r;
}
static void close_renderer(AudioRenderer *r) {
    audio_mixer_close(r);
    renderer_free(r);
}
/* One render pass as audren.c render_thread makes it; the voices are not advanced (every sample is the same). */
static void pass(AudioRenderer *r) {
    voices_prepare(r);
    audio_mixer_render(r, (double)FRAME / 48000, 0);
    r->elapsed_frames++;
}
static const float *buf(AudioRenderer *r, int b) { return mixbuf(r->mixer, b); }
static int flat(const float *d, float level) {
    for (int k = 0; k < FRAME; k++)
        if (fabsf(d[k] - level) > 1e-6f) return 0;
    return 1;
}
static int finite_buffers(AudioRenderer *r) {
    Mixer *m = r->mixer;
    for (int i = 0; i < m->nbuf * m->frame; i++)
        if (!isfinite(m->buf[i])) return 0;
    return 1;
}
/* how many kinds audio_bad has logged for this renderer */
static int kinds_seen(const AudioRenderer *r) {
    int n = 0;
    for (int k = 0; k < AB_KINDS; k++) n += r->bad_seen[k] != 0;
    return n;
}

/* ---- an update ---- */
typedef struct {
    int rev;                              /* 11 or 15 */
    AudrenChannelIn chan[2];
    uint32_t nchan;
    AudrenVoiceIn voice[VOICES];          /* REV11 layout; at REV15 vfilter holds the voices' filters */
    AudrenBiquadFloat vfilter[VOICES][2];
    AudrenEffectIn effect[EFFECTS];
    uint8_t split[1024];
    uint32_t split_bytes;
    AudrenMixIn mix[2];
    uint8_t mix_raw[0x20 + 3 * sizeof(AudrenMixIn)];   /* when mix_raw_bytes: the mix section, not the two mixes */
    uint32_t mix_raw_bytes;
    AudrenSinkIn sink;
} Update;

static void update_default(Update *u, int rev) {
    memset(u, 0, sizeof *u);
    u->rev = rev;
    u->chan[0] = (AudrenChannelIn){.id = 0, .in_use = 1};
    u->chan[0].mix[0] = 1;
    u->nchan = 1;
    AudrenVoiceIn *v = &u->voice[0];
    v->id = 0, v->in_use = 1, v->play_state = PLAY_STARTED, v->sample_format = FMT_PCM16, v->sample_rate = 48000;
    v->channel_count = 1, v->pitch = 1, v->volume = 1, v->mix_id = 0, v->splitter_id = SPLITTER_UNUSED;
    v->channel_ids[0] = 0;
    v->wave_buffers[0] =
        (AudrenWaveBuffer){.address = (uintptr_t)pcm, .size = sizeof pcm, .start = 0, .end = 4 * FRAME};
    u->voice[1] = (AudrenVoiceIn){.id = 1, .splitter_id = SPLITTER_UNUSED};
    u->mix[0] = (AudrenMixIn){.volume = 1, .sample_rate = 48000, .buffer_count = 2, .in_use = 1, .mix_id = 0,
                              .dest_mix_id = -1, .dest_splitter_id = SPLITTER_UNUSED};
    u->mix[1] = (AudrenMixIn){.volume = 1, .sample_rate = 48000, .buffer_count = 2, .in_use = 1, .mix_id = 1,
                              .dest_mix_id = 0, .dest_splitter_id = SPLITTER_UNUSED};
    u->mix[1].mix[0][0] = u->mix[1].mix[1][1] = 1;
    u->sink = (AudrenSinkIn){.type = SINK_DEVICE, .in_use = 1, .input_count = 2, .inputs = {0, 1}};
}
/* voice 1 like voice 0, into `mix` through channel resource 6 (when splitter is SPLITTER_UNUSED) or into `splitter` */
static void voice1_on(Update *u, int32_t mix, uint32_t splitter) {
    u->voice[1] = u->voice[0];
    u->voice[1].id = 1, u->voice[1].mix_id = mix, u->voice[1].splitter_id = splitter, u->voice[1].channel_ids[0] = 6;
    u->chan[1] = (AudrenChannelIn){.id = 6, .in_use = 1};
    u->chan[1].mix[0] = u->chan[1].mix[1] = 1;
    u->nchan = 2;
}

/* Where build put each section, for the checks that change the input afterwards. */
static struct { uint32_t total, chans, voices, effects, splitters, mixes, sinks; } L;
/* The game's buffers: the input, the output, the auxiliary and capture rings and the circular sink's buffer. Guest
   memory lies below RT_ADDRESS_LIMIT, and ipc_guest and the mixer's ring checks hold the game's addresses to it; this
   process's own data lies above it, so main takes these from memory below the limit (low_alloc). */
#define IN_BYTES 0x8000u
#define OUT_BYTES 0x2000u
#define SINK_SAMPLES 4096u
static uint8_t *in, *out;
static int32_t *ring_send, *ring_ret;   /* 0x80-byte headers, then 4096 samples */
static int16_t *sink_data;

static uint32_t build(const Update *u) {
    memset(in, 0, IN_BYTES);
    const size_t stride = u->rev >= 15 ? sizeof(AudrenVoiceInV2) : sizeof(AudrenVoiceIn);
    const uint32_t npools = EFFECTS + 4 * VOICES;
    AudrenHeader h = {.revision = AUDREN_REV(u->rev), .behavior = 0x10, .memory_pools = npools * sizeof(AudrenPoolIn),
                      .voices = (uint32_t)(VOICES * stride), .voice_resources = u->nchan * sizeof(AudrenChannelIn),
                      .effects = EFFECTS * sizeof(AudrenEffectIn),
                      .mixes = u->mix_raw_bytes ? u->mix_raw_bytes : 2 * sizeof(AudrenMixIn),
                      .sinks = sizeof(AudrenSinkIn), .performance = 0x10, .splitters = u->split_bytes,
                      .render_info = 0x10};
    uint32_t at = sizeof h + h.behavior + h.memory_pools;
    L.chans = at;
    memcpy(in + at, u->chan, h.voice_resources);
    at += h.voice_resources;
    L.voices = at;
    for (int i = 0; i < VOICES; i++, at += (uint32_t)stride) {
        if (u->rev < 15) {
            memcpy(in + at, &u->voice[i], sizeof u->voice[i]);
            continue;
        }
        AudrenVoiceInV2 v2;
        memcpy(v2.prefix, &u->voice[i], sizeof v2.prefix);
        memcpy(v2.biquads, u->vfilter[i], sizeof v2.biquads);
        memcpy(v2.tail, (const uint8_t *)&u->voice[i] + 0x3c, sizeof v2.tail);
        memcpy(in + at, &v2, sizeof v2);
    }
    L.effects = at;
    memcpy(in + at, u->effect, h.effects);
    at += h.effects;
    L.splitters = at;
    memcpy(in + at, u->split, u->split_bytes);
    at += u->split_bytes;
    L.mixes = at;
    memcpy(in + at, u->mix_raw_bytes ? (const void *)u->mix_raw : (const void *)u->mix, h.mixes);
    at += h.mixes;
    L.sinks = at;
    memcpy(in + at, &u->sink, sizeof u->sink);
    at += h.sinks + h.performance + h.render_info;
    h.total = at;
    memcpy(in, &h, sizeof h);
    return L.total = at;
}
static AudrenHeader *header(void) { return (AudrenHeader *)in; }

/* A RequestUpdate (cmd 4; 10 takes the output in C buffers) of `size` input bytes into an output of `out_size`. */
static IpcResponse request(AudioRenderer *r, unsigned cmd, uint64_t size, uint64_t out_size) {
    IpcSession s = {.state = r};
    IpcRequest rq = {.cmd = cmd};
    IpcBuffer i = {(uintptr_t)in, size, 0}, o = {(uintptr_t)out, out_size, 0}, p = {(uintptr_t)out + 0x1800, 0x10, 1};
    if (cmd == 4) rq.na = 1, rq.a[0] = i, rq.nb = 2, rq.b[0] = o, rq.b[1] = p;
    else rq.nx = 1, rq.x[0] = i, rq.nc = 2, rq.c[0] = o, rq.c[1] = p;
    IpcResponse rs = {0};
    renderer_handler(&s, &rq, &rs);
    return rs;
}
static uint32_t apply(AudioRenderer *r, const Update *u) { return request(r, 4, build(u), OUT_BYTES).result; }

/* ---- the splitter section ---- */
typedef struct { uint32_t id; int32_t mix; int buffer; float volume; uint8_t used; AudrenBiquadFloat filter[2]; } Dest;
static const int32_t both_ids[2] = {0, 1};
static const Dest both_dests[2] = {{.id = 0, .mix = 0, .buffer = 0, .volume = 1, .used = 1},
                                   {.id = 1, .mix = 0, .buffer = 1, .volume = 1, .used = 1}};
/* splitter `splitter` with destination ids `ids` (`nids` of them; -1: no info record), then `ndest` destination
   records, padded to 16 bytes */
static uint32_t splitter_section(uint8_t *p, int rev, uint32_t splitter, int nids, const int32_t *ids, int ndest,
                                 const Dest *d) {
    const uint32_t stride = rev >= 15 ? 0xa8 : rev >= 12 ? 0x90 : 0x70;
    memset(p, 0, 1024);
    uint32_t head[3] = {0x48444e53u, nids >= 0 ? 1u : 0u, (uint32_t)ndest}, at = 0x20;
    memcpy(p, head, sizeof head);
    if (nids >= 0) {
        uint32_t info[4] = {0x49444e53u, splitter, 48000, (uint32_t)nids};
        memcpy(p + at, info, sizeof info);
        if (nids) memcpy(p + at + 16, ids, 4u * (uint32_t)nids);
        at += 16 + 4u * (uint32_t)nids + 12;
    }
    for (int i = 0; i < ndest; i++, at += stride) {
        uint32_t magic = 0x44444e53u;
        float vol[24] = {0};
        vol[d[i].buffer] = d[i].volume;
        memcpy(p + at, &magic, 4);
        memcpy(p + at + 4, &d[i].id, 4);
        memcpy(p + at + 8, vol, sizeof vol);
        memcpy(p + at + 0x68, &d[i].mix, 4);
        if (rev >= 15) memcpy(p + at + 0x6c, d[i].filter, sizeof d[i].filter);
        p[at + (rev >= 15 ? 0x9c : rev >= 12 ? 0x84 : 0x6c)] = d[i].used;
    }
    return (at + 15) & ~15u;
}
static int split_is(AudioRenderer *r, int32_t a, int32_t b) {
    const SplitInfo *s = &((Mixer *)r->mixer)->split[0];
    return s->ndest == 2 && s->ids && s->ids[0] == a && s->ids[1] == b;
}

/* ---- the state a refused update must leave as it was ---- */
typedef struct {
    ServerVoice voices[VOICES];
    AudrenEffectIn effects[EFFECTS];
    AudrenMixIn mixes[2];
    AudrenSinkIn sinks[1];
    AudrenChannelIn channels[VOICES * 6];
    AudrenPoolIn pools[EFFECTS + 4 * VOICES];
    uint32_t revision;
    uint64_t behavior;
    int32_t split_n, split_ids[8];
    SplitDest dest[2];
} Snap;
static void snap(AudioRenderer *r, Snap *s) {
    Mixer *m = r->mixer;
    memset(s, 0, sizeof *s);
    memcpy(s->voices, r->voices, sizeof s->voices);
    memcpy(s->effects, r->effects, sizeof s->effects);
    memcpy(s->mixes, r->mixes, sizeof s->mixes);
    memcpy(s->sinks, r->sinks, sizeof s->sinks);
    memcpy(s->channels, r->channels, sizeof s->channels);
    memcpy(s->pools, r->pools, sizeof s->pools);
    s->revision = r->revision, s->behavior = r->behavior_flags;
    s->split_n = m->split[0].ndest;
    for (int k = 0; k < s->split_n && k < 8; k++) s->split_ids[k] = m->split[0].ids[k];
    memcpy(s->dest, m->dest, sizeof s->dest);
}
static int out_untouched(void) {
    for (size_t i = 0; i < OUT_BYTES; i++)
        if (out[i] != 0xa5) return 0;
    return 1;
}

/* 1. Valid updates answer 0, and the output's total is the sum of its sections (the SDK aborts on a mismatch). */
static void valid_update(void) {
    for (int rev = 11; rev <= 15; rev += 4) {
        AudioRenderer *r = open_renderer();
        Update u;
        update_default(&u, rev);
        u.split_bytes = splitter_section(u.split, rev, 0, 2, both_ids, 2, both_dests);
        memset(out, 0xa5, OUT_BYTES);
        IpcResponse rs = request(r, 4, build(&u), OUT_BYTES);
        const AudrenHeader *o = (const AudrenHeader *)out;
        CHECK(rs.result == 0, "REV%d: a valid update answers %#x", rev, rs.result);
        CHECK(o->total == sizeof *o + o->memory_pools + o->voices + o->effects + o->sinks + o->performance +
                              o->behavior + o->render_info &&
                  o->total == output_header(r, AUDREN_REV(rev)).total,
              "REV%d: the output's total %u is the sum of its sections", rev, o->total);
        CHECK(r->voices[0].in.in_use && split_is(r, 0, 1), "REV%d: the voice and the splitter are applied", rev);
        pass(r);
        CHECK(flat(buf(r, 0), 0.5f), "REV%d: the voice plays into buffer 0 (%g)", rev, buf(r, 0)[0]);
        rs = request(r, 10, L.total, OUT_BYTES);
        CHECK(rs.result == 0 && rs.nxout == 2, "REV%d: RequestUpdateAuto answers 0 and places its two C buffers (%u)",
              rev, rs.nxout);
        CHECK(once_lines == 0 && r->bad_count == 0 && r->refused == 0, "REV%d: nothing logged or counted (%s)", rev,
              last_once);
        close_renderer(r);
    }
}

/* 2. Updates whose framing does not match the renderer: refused whole. */
enum {
    R_BUFFERS, R_SMALL, R_TOTAL_BIG, R_TOTAL_SMALL, R_BEHAVIOR, R_POOLS, R_CHANNELS, R_VOICES, R_VOICES_AUTO, R_EFFECTS,
    R_SINKS, R_OVERRUN, R_UNCONSUMED, R_OUTPUT, R_SPL_HEADER, R_SPL_MAGIC, R_SPL_INFO_OVERRUN, R_SPL_INFO, R_SPL_WRAP,
    R_SPL_PADDING, R_DEST_OVERRUN, R_DEST_MAGIC, R_SPL_SIZE, R_MIX_DIRTY, R_MIX_SIZE, R_LAYOUT_NEGATIVE,
    R_LAYOUT_TOO_MANY, R_ROWS
};
static const struct { const char *name; int kind; } refuse_rows[R_ROWS] = {
    [R_BUFFERS] = {"an empty input buffer", AB_UPD_BUFFERS},
    [R_SMALL] = {"an input shorter than the header", AB_UPD_SMALL},
    [R_TOTAL_BIG] = {"a total past the input", AB_UPD_TOTAL},
    [R_TOTAL_SMALL] = {"a total under the header", AB_UPD_TOTAL},
    [R_BEHAVIOR] = {"a behavior section of 0x20 bytes", AB_UPD_BEHAVIOR},
    [R_POOLS] = {"a memory pool section one pool short", AB_UPD_POOLS},
    [R_CHANNELS] = {"a voice channel section of part of a record", AB_UPD_CHANNELS},
    [R_VOICES] = {"a voice section 0x10 bytes short", AB_UPD_VOICES},
    [R_VOICES_AUTO] = {"the same through RequestUpdateAuto", AB_UPD_VOICES},
    [R_EFFECTS] = {"an effect section one effect long", AB_UPD_EFFECTS},
    [R_SINKS] = {"an empty sink section", AB_UPD_SINKS},
    [R_OVERRUN] = {"a render info section past the total", AB_UPD_OVERRUN},
    [R_UNCONSUMED] = {"0x10 bytes after the last section", AB_UPD_UNCONSUMED},
    [R_OUTPUT] = {"an output buffer of 0x100 bytes", AB_UPD_OUTPUT},
    [R_SPL_HEADER] = {"a splitter section of 0x10 bytes", AB_SPL_HEADER},
    [R_SPL_MAGIC] = {"a splitter header that is not SNDH", AB_SPL_MAGIC},
    [R_SPL_INFO_OVERRUN] = {"a splitter info past the section", AB_SPL_INFO_OVERRUN},
    [R_SPL_INFO] = {"a splitter info that is not SNDI", AB_SPL_INFO},
    [R_SPL_WRAP] = {"a splitter info of 0x40000000 ids (4 * count wraps in 32 bits)", AB_SPL_INFO},
    [R_SPL_PADDING] = {"a splitter info's reserved bytes past the section", AB_SPL_PADDING},
    [R_DEST_OVERRUN] = {"a third destination record past the section", AB_DEST_OVERRUN},
    [R_DEST_MAGIC] = {"a destination record that is not SNDD", AB_DEST_MAGIC},
    [R_SPL_SIZE] = {"32 bytes after the splitter records", AB_SPL_SIZE},
    [R_MIX_DIRTY] = {"a changed-mix header of 2 mixes with 1 record", AB_MIX_DIRTY},
    [R_MIX_SIZE] = {"a mix section neither full nor changed-only", AB_MIX_SIZE},
    [R_LAYOUT_NEGATIVE] = {"a submix in use with -6 buffers", AB_MIX_LAYOUT},
    [R_LAYOUT_TOO_MANY] = {"mixes in use with 6 + 6 buffers of 8", AB_MIX_LAYOUT},
};
static IpcResponse refused_request(AudioRenderer *r, int row) {
    Update u;
    update_default(&u, 11);
    u.split_bytes = splitter_section(u.split, 11, 0, 2, both_ids, 2, both_dests);   /* 0x130 bytes, records to 0x124 */
    unsigned cmd = row == R_VOICES_AUTO ? 10 : 4;
    uint64_t out_size = OUT_BYTES;
    uint32_t word;
    switch (row) {
    case R_SPL_HEADER: u.split_bytes = 0x10; break;
    case R_SPL_MAGIC: u.split[0] = 'X'; break;
    case R_SPL_INFO_OVERRUN:   /* one info record announced, 8 bytes of it in the section */
        splitter_section(u.split, 11, 0, 0, NULL, 0, NULL);
        u.split_bytes = 0x28;
        break;
    case R_SPL_INFO: u.split[0x20] = 'X'; break;
    case R_SPL_WRAP: word = 0x40000000u, memcpy(u.split + 0x2c, &word, 4); break;
    case R_SPL_PADDING:   /* the id fits, 4 of its 12 reserved bytes do */
        splitter_section(u.split, 11, 0, 1, both_ids, 0, NULL);
        u.split_bytes = 0x38;
        break;
    case R_DEST_OVERRUN: u.split[8] = 3; break;
    case R_DEST_MAGIC: u.split[0x44] = 'X'; break;
    case R_SPL_SIZE: u.split_bytes += 32; break;
    case R_MIX_DIRTY:
        word = 0xcafebabeu, memcpy(u.mix_raw, &word, 4), word = 2, memcpy(u.mix_raw + 4, &word, 4);
        memcpy(u.mix_raw + 0x20, &u.mix[1], sizeof u.mix[1]);
        u.mix_raw_bytes = 0x20 + sizeof(AudrenMixIn);
        break;
    case R_MIX_SIZE:
        memcpy(u.mix_raw, u.mix, sizeof u.mix);
        u.mix_raw_bytes = sizeof u.mix + 0x10;
        break;
    case R_LAYOUT_NEGATIVE: u.mix[1].buffer_count = -6; break;
    case R_LAYOUT_TOO_MANY: u.mix[0].buffer_count = u.mix[1].buffer_count = 6; break;
    }
    uint64_t size = build(&u);
    AudrenHeader *h = header();
    switch (row) {
    case R_BUFFERS: size = 0; break;
    case R_SMALL: size = 0x20; break;
    case R_TOTAL_BIG: h->total += 0x10; break;
    case R_TOTAL_SMALL: h->total = 0x20; break;
    case R_BEHAVIOR: h->behavior = 0x20; break;
    case R_POOLS: h->memory_pools -= sizeof(AudrenPoolIn); break;
    case R_CHANNELS: h->voice_resources += 8; break;
    case R_VOICES: case R_VOICES_AUTO: h->voices -= 0x10, h->total -= 0x10, size -= 0x10; break;
    case R_EFFECTS: h->effects = sizeof(AudrenEffectIn); break;
    case R_SINKS: h->sinks = 0; break;
    case R_OVERRUN: h->render_info = 0x100; break;
    case R_UNCONSUMED: h->total += 0x10, size += 0x10; break;
    case R_OUTPUT: out_size = 0x100; break;
    }
    return request(r, cmd, size, out_size);
}
static void refused_updates(void) {
    for (int row = 0; row < R_ROWS; row++) {
        const char *name = refuse_rows[row].name;
        AudioRenderer *r = open_renderer();
        Update u;
        update_default(&u, 11);
        u.split_bytes = splitter_section(u.split, 11, 0, 2, both_ids, 2, both_dests);
        CHECK(apply(r, &u) == 0, "%s: the update before it is taken", name);
        pass(r);
        const int lines = once_lines;
        for (int n = 1; n <= 2; n++) {
            Snap before, after;
            snap(r, &before);
            memset(out, 0xa5, OUT_BYTES);
            IpcResponse rs = refused_request(r, row);
            snap(r, &after);
            CHECK(rs.result == 0x5299, "%s: answered %#x, not 0x5299", name, rs.result);
            CHECK(out_untouched(), "%s: the output buffer was written", name);
            CHECK(!memcmp(&before, &after, sizeof before), "%s: the renderer's state changed", name);
            CHECK(rs.nxout == 0, "%s: %u C buffers placed for a refused update", name, rs.nxout);
            CHECK(r->bad_seen[refuse_rows[row].kind] && kinds_seen(r) == 1, "%s: logged as kind %d (%d kinds)", name,
                  refuse_rows[row].kind, kinds_seen(r));
            CHECK(once_lines - lines == 1, "%s: %d log lines after send %d (one, on the first)", name,
                  once_lines - lines, n);
            CHECK(r->refused == (uint64_t)n && r->refused_run == (uint64_t)n && r->bad_count == (uint64_t)n,
                  "%s: refused %llu, in a row %llu, counted %llu after send %d", name, (unsigned long long)r->refused,
                  (unsigned long long)r->refused_run, (unsigned long long)r->bad_count, n);
        }
        pass(r);
        CHECK(flat(buf(r, 0), 0.5f) && flat(buf(r, 1), 0), "%s: the renderer goes on with the update before", name);
        update_default(&u, 11);
        CHECK(apply(r, &u) == 0 && r->refused_run == 0, "%s: the next valid update is taken (in a row %llu)", name,
              (unsigned long long)r->refused_run);
        close_renderer(r);
    }
}

/* 3. One record the renderer cannot use: left out, the rest of the update applied. */
static int left_out(AudioRenderer *r, int lines, int kind) {
    return once_lines - lines == 1 && r->bad_seen[kind] && kinds_seen(r) == 1 && r->refused == 0;
}
static void left_out_records(void) {
    Update u;
    int lines;

    AudioRenderer *r = open_renderer();   /* channel resource id 12 of 12 next to a valid record */
    update_default(&u, 11);
    apply(r, &u);
    lines = once_lines;
    u.chan[1] = u.chan[0], u.chan[1].mix[0] = 0.25f, u.chan[0].id = 12, u.chan[1].id = 1, u.nchan = 2;
    CHECK(apply(r, &u) == 0 && r->channels[1].mix[0] == 0.25f, "a valid channel resource next to id 12 is applied");
    CHECK(left_out(r, lines, AB_CHANNEL_ID), "channel resource id 12 is logged once (%s)", last_once);
    close_renderer(r);

    r = open_renderer();   /* an in-use voice with id 7 of 2 next to a valid voice */
    update_default(&u, 11);
    apply(r, &u);
    pass(r);
    lines = once_lines;
    ServerVoice was = r->voices[1];
    u.voice[0].volume = 0.75f;
    u.voice[1] = u.voice[0], u.voice[1].id = 7;
    memset(out, 0xa5, OUT_BYTES);
    CHECK(apply(r, &u) == 0 && r->voices[0].in.volume == 0.75f, "the valid voice next to voice id 7 is applied");
    CHECK(!memcmp(&was, &r->voices[1], sizeof was), "slot 1's server voice is untouched");
    const uint8_t *vo = out + sizeof(AudrenHeader) + (EFFECTS + 4 * VOICES) * sizeof(AudrenPoolOut);
    CHECK(vo[0] != 0xa5 && vo[sizeof(AudrenVoiceOut)] != 0xa5, "the voice status is written for both slots");
    CHECK(left_out(r, lines, AB_VOICE_ID), "voice id 7 is logged once (%s)", last_once);
    close_renderer(r);

    r = open_renderer();   /* a new voice whose current wave buffer is 4 of 4 */
    update_default(&u, 11);
    apply(r, &u);
    pass(r);
    lines = once_lines;
    was = r->voices[0];
    u.voice[0].is_new = 1, u.voice[0].wave_buffer_index = 4, u.voice[0].volume = 0.25f;
    CHECK(apply(r, &u) == 0 && !memcmp(&was, &r->voices[0], sizeof was), "the voice is left as it was (not reset)");
    CHECK(left_out(r, lines, AB_WAVE_INDEX), "wave buffer index 4 is logged once (%s)", last_once);
    close_renderer(r);

    r = open_renderer();   /* REV15: a voice filter with a NaN coefficient, next to a valid one */
    update_default(&u, 15);
    apply(r, &u);
    lines = once_lines;
    u.vfilter[0][0] = (AudrenBiquadFloat){.enable = 1, .numerator = {NAN, 0, 0}};
    u.vfilter[0][1] = (AudrenBiquadFloat){.enable = 1, .numerator = {1, 0, 0}};
    CHECK(apply(r, &u) == 0, "an update with a NaN voice filter is taken");
    const ServerVoice *v = &r->voices[0];
    CHECK(!v->in.biquads[0].enable && !v->float_biquads[0].enable, "the NaN filter is off");
    CHECK(v->in.biquads[1].enable && v->float_biquads[1].enable && v->use_float_biquads, "the other filter stays on");
    for (int i = 0; i < 10; i++) pass(r);
    CHECK(finite_buffers(r) && flat(buf(r, 0), 0.5f), "10 passes leave every mix buffer finite (%g)", buf(r, 0)[0]);
    CHECK(left_out(r, lines, AB_VOICE_FILTER), "the NaN filter is logged once (%s)", last_once);
    close_renderer(r);

    r = open_renderer();   /* splitter info naming splitter 3 of 1 */
    update_default(&u, 11);
    u.split_bytes = splitter_section(u.split, 11, 0, 2, both_ids, 2, both_dests);
    apply(r, &u);
    lines = once_lines;
    u.split_bytes = splitter_section(u.split, 11, 3, 2, both_ids, 0, NULL);
    CHECK(apply(r, &u) == 0 && split_is(r, 0, 1), "splitter info 3 is left out, splitter 0 keeps its destinations");
    CHECK(left_out(r, lines, AB_SPL_ID), "splitter 3 is logged once (%s)", last_once);
    close_renderer(r);

    /* a splitter listing destination 5 of 2: stored as -1, which both render loops skip. Destination 0 would put the
       sound into the submix's first buffer (the mixer's 2), destination 1 goes to final mix buffer 1. The destination
       table gets a guard entry in front of it, used and into the final mix at full volume, so a loop that reads id -1
       puts the sound into buffers 0 and 1 a second time. */
    static SplitDest guarded[1 + 2];
    for (int via_mix = 0; via_mix < 2; via_mix++) {
        r = open_renderer();
        Mixer *m = r->mixer;
        SplitDest *table = m->dest;
        memset(guarded, 0, sizeof guarded);
        guarded[0].used = 1, guarded[0].dest = 0;
        for (int b = 0; b < 24; b++) guarded[0].vol[b] = 1;
        m->dest = guarded + 1;
        update_default(&u, 11);
        apply(r, &u);
        lines = once_lines;
        const int32_t ids[2] = {5, 1};
        Dest d[2] = {both_dests[0], both_dests[1]};
        if (via_mix) {   /* voice 1 into the submix (buffers 2-3), the submix through the splitter */
            voice1_on(&u, 1, SPLITTER_UNUSED);
            u.mix[1].dest_mix_id = -1, u.mix[1].dest_splitter_id = 0;
        } else {         /* voice 1 through the splitter */
            voice1_on(&u, 0, 0);
            d[0].mix = 1;
        }
        u.split_bytes = splitter_section(u.split, 11, 0, 2, ids, 2, d);
        CHECK(apply(r, &u) == 0 && split_is(r, -1, 1), "%s: destination 5 is stored as -1, destination 1 kept",
              via_mix ? "a submix" : "a voice");
        for (int i = 0; i < 10; i++) pass(r);
        CHECK(flat(buf(r, 0), 0.5f) && flat(buf(r, 1), 0.5f), "%s: through the splitter into buffer 1 only (%g %g)",
              via_mix ? "a submix" : "a voice", buf(r, 0)[0], buf(r, 1)[0]);
        if (!via_mix) CHECK(flat(buf(r, 2), 0), "a voice: nothing through the destination left out (%g)", buf(r, 2)[0]);
        CHECK(left_out(r, lines, AB_SPL_DEST_REF), "destination 5 is logged once (%s)", last_once);
        m->dest = table;   /* the mixer frees its own table */
        close_renderer(r);
    }

    r = open_renderer();   /* a destination record naming destination 9 of 2, next to a valid one */
    update_default(&u, 11);
    u.split_bytes = splitter_section(u.split, 11, 0, 2, both_ids, 2, both_dests);
    apply(r, &u);
    lines = once_lines;
    SplitDest dest0 = ((Mixer *)r->mixer)->dest[0];
    Dest d9[2] = {both_dests[0], both_dests[1]};
    d9[0].id = 9, d9[0].volume = 0.25f, d9[1].volume = 0.5f;
    u.split_bytes = splitter_section(u.split, 11, 0, -1, NULL, 2, d9);
    CHECK(apply(r, &u) == 0 && ((Mixer *)r->mixer)->dest[1].vol[1] == 0.5f, "the valid destination record is applied");
    CHECK(!memcmp(&dest0, &((Mixer *)r->mixer)->dest[0], sizeof dest0), "destination 0 is untouched");
    CHECK(left_out(r, lines, AB_DEST_ID), "destination 9 is logged once (%s)", last_once);
    close_renderer(r);

    r = open_renderer();   /* REV15: a destination filter with an infinite denominator */
    update_default(&u, 15);
    apply(r, &u);
    lines = once_lines;
    Dest df[2] = {both_dests[0], both_dests[1]};
    df[0].filter[0] = (AudrenBiquadFloat){.enable = 1, .numerator = {1, 0, 0}, .denominator = {INFINITY, 0}};
    df[0].filter[1] = (AudrenBiquadFloat){.enable = 1, .numerator = {1, 0, 0}};
    u.split_bytes = splitter_section(u.split, 15, 0, 2, both_ids, 2, df);
    voice1_on(&u, 0, 0);
    CHECK(apply(r, &u) == 0, "an update with an infinite destination filter is taken");
    const SplitDest *sd = &((Mixer *)r->mixer)->dest[0];
    CHECK(!sd->biquads[0].enable && sd->biquads[1].enable, "that filter is off, the other stays on");
    for (int i = 0; i < 10; i++) pass(r);
    CHECK(finite_buffers(r), "10 passes leave every mix buffer finite");
    CHECK(left_out(r, lines, AB_DEST_FILTER), "the infinite filter is logged once (%s)", last_once);
    close_renderer(r);

    r = open_renderer();   /* a changed-mix record for mix 5 of 2 next to a valid one */
    update_default(&u, 11);
    apply(r, &u);
    lines = once_lines;
    uint32_t word = 0xcafebabeu;
    memcpy(u.mix_raw, &word, 4);
    word = 2;
    memcpy(u.mix_raw + 4, &word, 4);
    AudrenMixIn bad = u.mix[1], good = u.mix[1];
    bad.mix_id = 5, good.volume = 0.5f;
    memcpy(u.mix_raw + 0x20, &bad, sizeof bad);
    memcpy(u.mix_raw + 0x20 + sizeof bad, &good, sizeof good);
    u.mix_raw_bytes = 0x20 + 2 * sizeof(AudrenMixIn);
    CHECK(apply(r, &u) == 0 && r->mixes[1].volume == 0.5f, "the valid changed mix next to mix 5 is applied");
    CHECK(left_out(r, lines, AB_MIX_ID), "mix 5 is logged once (%s)", last_once);
    close_renderer(r);

    /* A splitter section whose framing breaks between audio_mixer_routing_check's walk and the apply walk (the game
       rewriting its input during its own RequestUpdate): audio_mixer_update_routing is called directly, as
       audren.c request_update calls it once the update was taken. The splitter info before the fault and the mix
       section after it are applied, the destination record at the fault is not, and the log says the rest of the
       splitter section is left out, not that the update was refused. */
    r = open_renderer();
    update_default(&u, 11);
    u.split_bytes = splitter_section(u.split, 11, 0, 2, both_ids, 2, both_dests);
    apply(r, &u);
    lines = once_lines;
    dest0 = ((Mixer *)r->mixer)->dest[0];
    const int32_t swapped[2] = {1, 0};
    Dest dq[2] = {both_dests[0], both_dests[1]};
    dq[0].volume = 0.25f;
    u.split_bytes = splitter_section(u.split, 11, 0, 2, swapped, 2, dq);
    u.split[0x44] = 'X';   /* the first destination record's magic, after the splitter info and its 2 ids */
    u.mix[1].volume = 0.5f;
    build(&u);
    audio_mixer_update_routing(r, in + L.splitters, u.split_bytes, in + L.mixes, 2 * sizeof(AudrenMixIn), 2);
    CHECK(split_is(r, 1, 0) && r->mixes[1].volume == 0.5f,
          "a fault in the apply walk: the splitter info before it and the mix section are applied");
    CHECK(!memcmp(&dest0, &((Mixer *)r->mixer)->dest[0], sizeof dest0),
          "a fault in the apply walk: the destination record at the fault is left out");
    CHECK(left_out(r, lines, AB_DEST_MAGIC) && strstr(last_once, "the rest of the splitter section is left out") &&
              !strstr(last_once, "refused"),
          "a fault in the apply walk is logged once as the rest of the section left out (%s)", last_once);
    close_renderer(r);
}

/* 4. Effects and sinks the renderer cannot use: passed through or skipped in each render pass. */
static void rings_fill(void) {
    memset(ring_send, 0, 128), memset(ring_ret, 0, 128);
    for (int i = 32; i < 32 + 4096; i++) ring_send[i] = ring_ret[i] = 0x1234;
    for (unsigned i = 0; i < SINK_SAMPLES; i++) sink_data[i] = 0x5a5a;
}
static int rings_untouched(void) {
    int32_t head[32] = {0};
    if (memcmp(ring_send, head, 128) || memcmp(ring_ret, head, 128)) return 0;
    for (int i = 32; i < 32 + 4096; i++)
        if (ring_send[i] != 0x1234 || ring_ret[i] != 0x1234) return 0;
    for (unsigned i = 0; i < SINK_SAMPLES; i++)
        if (sink_data[i] != 0x5a5a) return 0;
    return 1;
}

enum { PASS, SKIP };               /* what the render does with it: buffer 1 holds buffer 0's 0.5, or stays 0 */
enum { AT_UPDATE, EVERY_PASS };    /* where it is found: once per update, or in each render pass */
enum { E_NONE, E_BAD, E_NO_DSP, E_NO_DELAY, E_NO_REVERB };
typedef struct { const char *name; uint8_t type; int kind, render, found, state; } EffectRow;
static const EffectRow effect_rows[] = {
    {"limiter attack 2", 7, AB_LIMITER_PARAMS, PASS, EVERY_PASS, E_NONE},
    {"limiter release NaN", 7, AB_LIMITER_PARAMS, PASS, EVERY_PASS, E_NONE},
    {"limiter delay 0", 7, AB_LIMITER_PARAMS, PASS, EVERY_PASS, E_NONE},
    {"limiter delay above the longest", 7, AB_LIMITER_PARAMS, PASS, EVERY_PASS, E_NONE},
    {"limiter longest delay 0", 7, AB_LIMITER_SIZE, PASS, AT_UPDATE, E_NO_DELAY},
    {"limiter longest delay 200000", 7, AB_LIMITER_SIZE, PASS, AT_UPDATE, E_NO_DELAY},
    {"limiter 7 channels", 7, AB_LIMITER_CHANNELS, SKIP, EVERY_PASS, E_NO_DELAY},
    {"limiter input 9", 7, AB_LIMITER_INDEX, SKIP, EVERY_PASS, E_NONE},
    {"delay rate 0", 3, AB_DELAY_PARAMS, PASS, AT_UPDATE, E_NO_DSP},
    {"delay above the longest", 3, AB_DELAY_PARAMS, PASS, AT_UPDATE, E_NO_DSP},
    {"delay 3 channels", 3, AB_DELAY_PARAMS, PASS, AT_UPDATE, E_NO_DSP},
    {"reverb early mode 5", 4, AB_REVERB_PARAMS, PASS, AT_UPDATE, E_NO_DSP},
    {"reverb decay 0", 4, AB_REVERB_PARAMS, PASS, AT_UPDATE, E_NO_DSP},
    {"compressor ratio 0", 9, AB_COMPRESSOR_PARAMS, PASS, AT_UPDATE, E_NO_DSP},
    {"compressor attack 2", 9, AB_COMPRESSOR_PARAMS, PASS, AT_UPDATE, E_NO_DSP},
    {"reverb3d delays of 0.5 s", 5, AB_R3D_PARAMS, PASS, AT_UPDATE, E_NO_REVERB},
    {"reverb3d decay time 0", 5, AB_R3D_PARAMS, PASS, AT_UPDATE, E_NO_REVERB},
    {"reverb3d density 101", 5, AB_R3D_PARAMS, PASS, AT_UPDATE, E_NO_REVERB},
    {"reverb3d 3 channels", 5, AB_R3D_CHANNELS, SKIP, EVERY_PASS, E_NO_REVERB},
    {"reverb3d input 9", 5, AB_R3D_INDEX, SKIP, EVERY_PASS, E_NO_REVERB},
    {"biquad 0 channels", 6, AB_DSP_CHANNELS, SKIP, EVERY_PASS, E_NONE},
    {"biquad 7 channels", 6, AB_DSP_CHANNELS, SKIP, EVERY_PASS, E_NONE},
    {"delay input 9", 3, AB_DSP_INDEX, SKIP, EVERY_PASS, E_NONE},
    {"buffer-mix of 25 pairs", 1, AB_BUFMIX_COUNT, SKIP, EVERY_PASS, E_NONE},
    {"buffer-mix input 9", 1, AB_BUFMIX_INDEX, SKIP, EVERY_PASS, E_NONE},
    {"aux 25 channels", 2, AB_AUX_SIZE, SKIP, EVERY_PASS, E_NONE},
    {"aux ring of 16 Mi + 1", 2, AB_AUX_SIZE, SKIP, EVERY_PASS, E_NONE},
    {"aux ring at 1 << 40", 2, AB_AUX_ADDRESS, SKIP, EVERY_PASS, E_NONE},
    {"aux input 9", 2, AB_AUX_INDEX, SKIP, EVERY_PASS, E_NONE},
    {"capture 25 channels", 8, AB_CAPTURE_SIZE, SKIP, EVERY_PASS, E_NONE},
    {"capture ring of 16 Mi + 1", 8, AB_CAPTURE_SIZE, SKIP, EVERY_PASS, E_NONE},
    {"capture ring at 1 << 40", 8, AB_CAPTURE_ADDRESS, SKIP, EVERY_PASS, E_NONE},
    {"capture input 9", 8, AB_CAPTURE_INDEX, SKIP, EVERY_PASS, E_NONE},
    {"circular sink PCM8", 0, AB_SINK_PARAMS, SKIP, EVERY_PASS, E_NONE},
    {"circular sink of an odd size", 0, AB_SINK_PARAMS, SKIP, EVERY_PASS, E_NONE},
    {"circular sink of 0 channels", 0, AB_SINK_PARAMS, SKIP, EVERY_PASS, E_NONE},
    {"circular sink at 1 << 40", 0, AB_SINK_ADDRESS, SKIP, EVERY_PASS, E_NONE},
    {"circular sink input 200", 0, AB_SINK_INDEX, SKIP, EVERY_PASS, E_NONE},
};
#define EFFECT_ROWS ((int)(sizeof effect_rows / sizeof *effect_rows))

/* Effect slot 0 on the final mix from buffer 0 to buffer 1 (type 0: the sink instead), valid or as row `row` has it. */
static void effect_set(Update *u, int row, int bad) {
    const char *n = effect_rows[row].name;
    const uint8_t type = effect_rows[row].type;
#define IS(s) (bad && !strcmp(n, s))
    AudrenEffectIn *e = &u->effect[0];
    *e = (AudrenEffectIn){.type = type, .is_new = 1, .enabled = 1, .mix_id = 0, .order = 0};
    if (type == 7) {
        AudrenLimiterParam p = {.input = {0}, .output = {1}, .channel_max = 1, .channels = 1, .sample_rate = 48000,
                                .attack = 0.5f, .release = 0.5f, .threshold = 1, .input_gain = 1, .output_gain = 1,
                                .delay_min = 1, .delay_max = 48};
        if (IS("limiter attack 2")) p.attack = 2;
        if (IS("limiter release NaN")) p.release = NAN;
        if (IS("limiter delay 0")) p.delay_min = 0;
        if (IS("limiter delay above the longest")) p.delay_min = 49;
        if (IS("limiter longest delay 0")) p.delay_max = 0;
        if (IS("limiter longest delay 200000")) p.delay_max = 200000;
        if (IS("limiter 7 channels")) p.channels = p.channel_max = 7;
        if (IS("limiter input 9")) p.input[0] = 9;
        memcpy(e->specific, &p, sizeof p);
    } else if (type == 3) {
        AudrenDelayParam p = {.input = {0, 0, 0}, .output = {1, 1, 1}, .channel_max = 1, .channels = 1,
                              .delay_max = 100, .delay = 50, .sample_rate = 48000u << 14, .in_gain = 1 << 14,
                              .feedback = 1 << 13, .out_gain = 1 << 14, .dry_gain = 1 << 14, .lowpass = 1 << 13};
        if (IS("delay rate 0")) p.sample_rate = 0;
        if (IS("delay above the longest")) p.delay = 101;
        if (IS("delay 3 channels")) p.channels = p.channel_max = 3;
        if (IS("delay input 9")) p.input[0] = 9;
        memcpy(e->specific, &p, sizeof p);
    } else if (type == 4) {
        AudrenReverbParam p = {.input = {0}, .output = {1}, .channel_max = 1, .channels = 1, .sample_rate = 48 << 14,
                               .early_gain = 1 << 14, .late_gain = 1 << 14, .decay = 1 << 14, .hf_decay = 1 << 14,
                               .reverb_gain = 1 << 14, .out_gain = 1 << 14, .dry_gain = 1 << 14};
        if (IS("reverb early mode 5")) p.early_mode = 5;
        if (IS("reverb decay 0")) p.decay = 0;
        memcpy(e->specific, &p, sizeof p);
    } else if (type == 9) {
        AudrenCompressorParam p = {.input = {0}, .output = {1}, .channel_max = 1, .channels = 1, .sample_rate = 48000,
                                   .threshold = -10, .ratio = 2, .input_gain = 0.5f, .attack = 0.5f, .release = 0.5f};
        if (IS("compressor ratio 0")) p.ratio = 0;
        if (IS("compressor attack 2")) p.attack = 2;
        memcpy(e->specific, &p, sizeof p);
    } else if (type == 5) {
        Reverb3dParam p = {.input = {0}, .output = {1}, .channel_max = 1, .channels = 1, .sample_rate = 48000,
                           .hf_reference = 5000, .decay_time = 1, .hf_decay_ratio = 0.5f, .diffusion = 50,
                           .reflection_delay = 0.05f, .reverb_delay = 0.05f, .density = 50, .dry_gain = 1};
        if (IS("reverb3d delays of 0.5 s")) p.reflection_delay = 0.3f, p.reverb_delay = 0.2f;
        if (IS("reverb3d decay time 0")) p.decay_time = 0;
        if (IS("reverb3d density 101")) p.density = 101;
        if (IS("reverb3d 3 channels")) p.channels = p.channel_max = 3;
        if (IS("reverb3d input 9")) p.input[0] = 9;
        memcpy(e->specific, &p, sizeof p);
    } else if (type == 6) {
        AudrenBiquadEffectV1 p = {.input = {0}, .output = {1}, .numerator = {1 << 14}, .channels = 1, .status = 1};
        if (IS("biquad 0 channels")) p.channels = 0;
        if (IS("biquad 7 channels")) p.channels = 7;
        memcpy(e->specific, &p, sizeof p);
    } else if (type == 1) {
        AudrenBufferMixParam p = {.input = {0}, .output = {1}, .volume = {1}, .count = 1};
        if (IS("buffer-mix of 25 pairs")) p.count = 25;
        if (IS("buffer-mix input 9")) p.input[0] = 9;
        memcpy(e->specific, &p, sizeof p);
    } else if (type == 2 || type == 8) {
        AudrenAuxParam p = {.input = {0}, .output = {1}, .channels = 1, .sample_rate = 48000, .capacity = 4096,
                            .channel_max = 1, .send_info = (uintptr_t)ring_send, .return_info = (uintptr_t)ring_ret};
        if (IS("aux 25 channels") || IS("capture 25 channels")) p.channels = 25;
        if (IS("aux ring of 16 Mi + 1") || IS("capture ring of 16 Mi + 1")) p.capacity = 16 * 1024 * 1024 + 1;
        if (IS("aux ring at 1 << 40") || IS("capture ring at 1 << 40")) p.send_info = p.return_info = 1ull << 40;
        if (IS("aux input 9") || IS("capture input 9")) p.input[0] = 9;
        memcpy(e->specific, &p, sizeof p);
    } else {   /* the circular sink in sink slot 0, instead of the device sink */
        *e = (AudrenEffectIn){0};
        CircularSinkParam p = {.address = (uintptr_t)sink_data, .size = 2 * SINK_SAMPLES, .channels = 1,
                               .format = FMT_PCM16, .input = {0}};
        if (IS("circular sink PCM8")) p.format = FMT_PCM8;
        if (IS("circular sink of an odd size")) p.size = 2 * SINK_SAMPLES - 1;
        if (IS("circular sink of 0 channels")) p.channels = 0;
        if (IS("circular sink at 1 << 40")) p.address = 1ull << 40;
        if (IS("circular sink input 200")) p.input[0] = 200;
        u->sink = (AudrenSinkIn){.type = SINK_CIRCULAR, .in_use = 1};
        memcpy(u->sink.name, &p, sizeof p);
    }
#undef IS
}

/* What a valid effect of row `row`'s type leaves after a pass: its DSP, delay line or reverb, or its output. */
static int effect_runs(AudioRenderer *r, int row) {
    const EffectState *s = &((Mixer *)r->mixer)->effect[0];
    switch (effect_rows[row].type) {
    case 7: return s->delay != NULL;
    case 3: case 4: case 9: return s->dsp != NULL;
    case 5: return s->reverb != NULL;
    case 6: case 1: return flat(buf(r, 1), 0.5f);
    case 2: case 8: return ring_send[32] != 0x1234;
    default: return sink_data[0] != 0x5a5a;
    }
}

static void effects_left_out(void) {
    for (int row = 0; row < EFFECT_ROWS; row++) {
        const EffectRow *x = &effect_rows[row];
        AudioRenderer *r = open_renderer();
        Update u;
        update_default(&u, 11);
        apply(r, &u);
        rings_fill();
        const int lines = once_lines;
        effect_set(&u, row, 1);
        CHECK(apply(r, &u) == 0, "%s: the update is taken", x->name);
        for (int i = 0; i < 10; i++) pass(r);
        const EffectState *s = &((Mixer *)r->mixer)->effect[0];
        if (x->render == PASS)
            CHECK(flat(buf(r, 1), 0.5f) && flat(buf(r, 0), 0.5f), "%s: its sound passes through (%g)", x->name,
                  buf(r, 1)[0]);
        else
            CHECK(flat(buf(r, 1), 0) && rings_untouched(), "%s: skipped, nothing written (%g)", x->name, buf(r, 1)[0]);
        CHECK(x->state != E_NO_DSP || (s->bad && !s->dsp), "%s: marked bad, no DSP", x->name);
        CHECK(x->state != E_NO_DELAY || !s->delay, "%s: no delay line", x->name);
        CHECK(x->state != E_NO_REVERB || !s->reverb, "%s: no reverb", x->name);
        /* the values found at the update mark the effect bad; those found in a pass leave it as it is */
        CHECK(s->bad == (x->found == AT_UPDATE), "%s: bad is %d", x->name, s->bad);
        CHECK(once_lines - lines == 1 && r->bad_seen[x->kind] && kinds_seen(r) == 1,
              "%s: %d lines over the update and 10 passes, kind %d seen %d, %d kinds (%s)", x->name, once_lines - lines,
              x->kind, r->bad_seen[x->kind], kinds_seen(r), last_once);
        CHECK(r->bad_count == (x->found == AT_UPDATE ? 1u : 10u), "%s: %llu values counted", x->name,
              (unsigned long long)r->bad_count);
        CHECK(finite_buffers(r), "%s: every mix buffer finite", x->name);
        /* the same slot with usable values: the effect runs again */
        const uint64_t counted = r->bad_count;
        rings_fill();
        effect_set(&u, row, 0);
        u.effect[0].is_new = 0;
        CHECK(apply(r, &u) == 0, "%s: the valid update is taken", x->name);
        pass(r);
        CHECK(!s->bad && effect_runs(r, row) && r->bad_count == counted, "%s: with usable values it runs again",
              x->name);
        close_renderer(r);
    }
}

/* 5. Valid effects and sinks over 20 passes: nothing left out, every buffer finite. */
static void in_range_quiet(void) {
    uint8_t done[16] = {0};   /* by type: one valid block of each */
    for (int row = 0; row < EFFECT_ROWS; row++) {
        if (done[effect_rows[row].type]) continue;
        done[effect_rows[row].type] = 1;
        AudioRenderer *r = open_renderer();
        Update u;
        update_default(&u, 11);
        rings_fill();
        effect_set(&u, row, 0);
        CHECK(apply(r, &u) == 0, "type %u: taken", effect_rows[row].type);
        for (int i = 0; i < 20; i++) pass(r);
        CHECK(r->bad_count == 0 && finite_buffers(r) && effect_runs(r, row), "type %u: %llu values left out",
              effect_rows[row].type, (unsigned long long)r->bad_count);
        close_renderer(r);
    }
}

/* 6. NX2NAVI_AUDIO_STRICT=1: such a value ends the process, as most did before 0.11.0 (tests/audio_params_check.py
   runs these in their own processes and expects exit 2). */
static int strict(const char *what) {
    strict_setting = 1;
    AudioRenderer *r = open_renderer();
    Update u;
    update_default(&u, 11);
    if (!strcmp(what, "strict-effect")) {
        effect_set(&u, 0, 1);   /* limiter attack 2 */
        apply(r, &u);
        pass(r);
    } else {
        build(&u);
        header()->voices -= 0x10, header()->total -= 0x10;
        request(r, 4, L.total - 0x10, OUT_BYTES);
    }
    printf("audio_params_check %s: the process went on\n", what);
    return 1;
}

int main(int argc, char **argv) {
    uint8_t *low = low_alloc(IN_BYTES + OUT_BYTES + 2 * (32 + 4096) * 4 + 2 * SINK_SAMPLES);
    in = low, out = in + IN_BYTES;
    ring_send = (int32_t *)(out + OUT_BYTES), ring_ret = ring_send + 32 + 4096;
    sink_data = (int16_t *)(ring_ret + 32 + 4096);
    if (argc > 1 && !strcmp(argv[1], "-v")) verbose = 1;
    else if (argc > 1) return strict(argv[1]);
    valid_update();
    refused_updates();
    left_out_records();
    effects_left_out();
    in_range_quiet();
    printf("audio_params_check: %s (%d refused updates, %d left-out records, %d effects and sinks)\n",
           fails ? "FAIL" : "ok", R_ROWS, 11, EFFECT_ROWS);
    return fails ? 1 : 0;
}
