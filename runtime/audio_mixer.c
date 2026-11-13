// Contains code Copyright (c) 2026 MacNativeGames contributors, Copyright (c) Ryujinx Team and Contributors (MIT; see
// THIRD_PARTY_NOTICES.md).
// The mixing stage (voice decode and resampling, splitters, mixes, effects, sinks) and the IAudioDevice service, with
// SDL3 audio output.
//
// Per 5 ms frame the render thread (audren.c, renderer lock held) decodes every started voice (PCM8/16/24/32, float,
// DSP-ADPCM) along the sample path the voice clock consumes, resamples it linearly, runs the voice biquads, scales it
// into its destination mix (a volume the game changed is reached in equal steps across the frame's samples,
// accumulate_ramp), folds the submixes, runs the mix effects and writes the device sink's mix (stereo, or 5.1
// with NX2NAVI_AUDIO_CHANNELS, output_channels) into a single-producer single-consumer ring. The SDL audio thread drains
// that ring without taking any lock: an empty ring
// plays silence and never blocks the render thread or the guest. NX2NAVI_AUDIO=none (or NX2NAVI_AUDIO_OFF) mixes without a
// device; NX2NAVI_AUDIO_DEVICE picks the playback device (resolve_audio_device); NX2NAVI_AUDIO_PREFILL_MS (0..500, default 20, 60 on Linux) is the queued audio the output waits for after start or underrun.
// The device's clock paces the render thread, as the audio hardware paces the Switch's DSP (audio_mixer_pace_ns): a
// frame is rendered when the ring holds less than the output target, max(prefill, two device periods), so the queue and
// the delay from the game to the speakers stay there and the host and device clocks cannot drift apart.
// When the render thread is held up the ring runs out and the device plays silence; the frames that silence stands for
// are then rendered and dropped (audio_mixer_owed, NX2NAVI_AUDIO_CATCHUP), so the sound keeps its place against the
// picture.
// Without a device, while the renderer is
// stopped and while the device takes nothing (a stall), the render thread keeps real time on its own timer; what piled
// up meanwhile is skipped on the SDL thread once the device resumes (more than QUEUE_SLACK_MS above the target plus the
// device's request), which trims the ring after a device stall.
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "menu_sound.h"
#include "cfg.h"
#include "audio_effects.h"
#include "audio_reverb.h"
#include "audren.h"
#include "host.h"
#include "ipc.h"

#define MAX_FRAME 1024
#define RING_FRAMES 16384            // output frames, about 340 ms at 48 kHz
#define OUT_MAX 6                    // output channels at most: 5.1
#define QUEUE_SLACK_MS 40            // queued audio above the target plus the device's request that ring_pull skips
#define STALL_MS 50                  // the device took nothing this long (at least 4 periods): the timer paces
#define SPLITTER_UNUSED 0xffffffffu
#define dm_guest_write_prepare(p, n) ((void)(p), (void)(n))   // guest memory is plain host memory on Windows

typedef struct {
    float vol[24]; int32_t dest; uint8_t used; AudrenBiquadFloat biquads[2]; float state[2][2];
    // volume ramps (audio_mixer_render): the volumes it had in the render pass before, the mix it went to and how many
    // of that mix's buffers; prev_ok 0 when that pass did not use it
    float prev[24]; int32_t prev_dest, prev_buffers; uint8_t prev_ok;
} SplitDest;
typedef struct { int32_t ndest; int32_t *ids; } SplitInfo;
// volume ramps: a mix in the render pass before: its volume, the splitter it went to, its buffer count, whether in use
typedef struct { float volume; uint32_t splitter; int32_t buffers; uint8_t ok; } MixPrev;
typedef struct {
    Reverb3d *reverb; Reverb3dParam param; int pending;
    float *delay, detector[6], compression[6], peak[6], minimum[6]; int position[6], delay_max, channels;
    AudioEffectDsp *dsp; unsigned type; uint8_t wire[160];
    float biquad_z[6][2], numerator[3], denominator[2]; uint8_t biquad_channels;
} EffectState;
typedef struct Mixer {
    float *buf; int nbuf, frame;
    int *offset;                                   // first mix buffer of each mix id
    SplitInfo *split; int nsplit; SplitDest *dest; int ndest;
    SDL_AudioStream *stream; int device_ok, rate;
    SDL_AudioDeviceID device;                      // the explicitly chosen device, 0 while the system default is open
    char device_name[128];                         // its name, for the log when it goes away
    uint64_t watch_ns;                             // render thread: when device_check last looked
    int channels;                                  // output channels, 2 or 6 (output_channels)
    float ring[RING_FRAMES * OUT_MAX];
    _Atomic uint64_t wr, rd, underruns, overruns, missing_frames, callback_frames, callback_nonzero, skipped_frames;
    _Atomic int playing; int ready;                // ready: the callback's own state
    int gap;                                       // the callback's own: an underrun's silence has not ended yet
    _Atomic uint64_t silent_frames;                // silence played in such gaps so far (audio_mixer_owed)
    uint64_t made_up; unsigned owed_epoch;         // render thread: how much of it is caught up or forgotten; the
                                                   // stop_epoch that count belongs to
    int target, period;                            // output target and device period, frames (output_open)
    uint64_t stall_ns;
    _Atomic uint64_t pulled_ns;                    // kern_now_ns of the device's last request, 0 before the first
    _Atomic uint64_t pull_gap_max;                 // NX2NAVI_AUDIO_STATS: longest gap between two requests (ns)
    _Atomic uint64_t flush_until; _Atomic unsigned stop_epoch; unsigned callback_epoch;
    EffectState *effect; int *effect_order; unsigned effect_reported;
    uint64_t *sink_address; uint32_t *sink_size, *sink_offset, *sink_old, *sink_written;
    unsigned reported;                             // one-time reports
    float voice_scratch[6][MAX_FRAME], filter_scratch[MAX_FRAME], output_scratch[MAX_FRAME * OUT_MAX];
    double level_sum, level_peak; uint64_t level_count, level_frames; int first_sound_logged;
    double input_sum[6];                           // NX2NAVI_TRACE=audio: the device sink's inputs, squared and summed
    int prefill;                                   // render blocks
    int ramp;                                      // NX2NAVI_AUDIO_RAMP: volumes ramp across a frame (accumulate_ramp)
    int snap;                                      // the next pass has no pass before it to ramp from (open, start)
    MixPrev *mix_prev;                             // each mix in the pass before
    uint64_t ramps, ramp_passes;                   // ramps made; render passes with at least one (audio_mixer_ramp_stats)
    uint64_t trace_pass, trace_seen[1024];         // NX2NAVI_TRACE=audio: render passes; the pass each voice was last mixed in
    uint64_t trace_buf[1024];                      // and the wave buffer address it read in that pass
    uint64_t trace_pair[1024];                     // the last shared-buffer pair logged for each voice (other voice, address)
    unsigned trace_shared;                         // "voices share a buffer" lines written (capped)
} Mixer;

static _Atomic float device_volume = 1.0f;

// NX2NAVI_VOLUME: the player's volume in percent (0..100) on top of the game's own; a live setting (the
// menu), parsed again when cfg_version changes. Render thread only.
static float host_volume(void) {
    static float v = 1.0f;
    static unsigned seen = ~0u;
    unsigned ver = cfg_version();
    if (ver == seen) return v;
    seen = ver;
    double pct = cfg_double("NX2NAVI_VOLUME");
    v = (float)((pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100);
    return v;
}
static unsigned audio_revision(const AudioRenderer *r) { unsigned byte = r->revision >> 24; return byte >= '0' ? byte - '0' : 0; }
static void report_once(Mixer *m, unsigned bit, const char *msg, int value) {
    if (m->reported & (1u << bit)) return;
    m->reported |= 1u << bit;
    rt_log("audio: %s (%d)", msg, value);
}

// ---- output: SDL3 audio stream fed from the ring
// How much audio the output waits for before it starts, and again after an underrun: NX2NAVI_AUDIO_PREFILL_MS rounded up
// to whole 5 ms render blocks. Returns -1 for a value outside 0..500 ms.
// The Linux default is higher: at 20 ms PipeWire's queue (max(prefill, two 1024-sample periods)) underran
// thousands of times within minutes in busy scenes, and 60 ms removed the underruns.
#ifdef __linux__
#define PREFILL_DEFAULT_MS 60
#else
#define PREFILL_DEFAULT_MS 20
#endif
static int prefill_blocks_for(const char *e) {
    if (!e || !*e) return PREFILL_DEFAULT_MS / 5;
    char *end;
    double ms = strtod(e, &end);
    if (end == e || *end || !(ms >= 0) || ms > 500) return -1;
    return (int)ceil(ms / 5.0);
}

// ---- playback device choice (NX2NAVI_AUDIO_DEVICE)
// SDL's playback device ids sorted by name (ties by id), the order --list-audio-devices prints them and the numbering of
// the index form. SDL's own order is not stable: it differed between two enumerations in one process, so an index into it
// would pick another device from one start to the next. Free the result with SDL_free. SDL audio must be initialised.
// *count is 0 when SDL reports none.
static int device_order(const void *x, const void *y) {
    SDL_AudioDeviceID a = *(const SDL_AudioDeviceID *)x, b = *(const SDL_AudioDeviceID *)y;
    const char *na = SDL_GetAudioDeviceName(a), *nb = SDL_GetAudioDeviceName(b);
    int c = SDL_strcasecmp(na ? na : "", nb ? nb : "");
    return c ? c : a < b ? -1 : a > b;
}
static SDL_AudioDeviceID *playback_devices(int *count) {
    SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(count);
    if (!ids) *count = 0;
    else qsort(ids, (size_t)*count, sizeof *ids, device_order);
    return ids;
}

// One line describing device `id` ("name, 2 ch, 48000 Hz"), without a leading index.
static void describe_device(char *buf, size_t n, SDL_AudioDeviceID id) {
    SDL_AudioSpec spec = {0};
    const char *name = SDL_GetAudioDeviceName(id);
    if (SDL_GetAudioDeviceFormat(id, &spec, NULL)) snprintf(buf, n, "%s, %d ch, %d Hz", name ? name : "?", spec.channels, spec.freq);
    else snprintf(buf, n, "%s", name ? name : "?");
}

static void log_playback_devices(void) {
    int count;
    SDL_AudioDeviceID *ids = playback_devices(&count);
    if (!count) rt_log("audio: SDL reports no playback devices");
    for (int i = 0; i < count; i++) {
        char line[256];
        describe_device(line, sizeof line, ids[i]);
        rt_log("audio:   %d: %s", i + 1, line);
    }
    SDL_free(ids);
}

// NX2NAVI_AUDIO_DEVICE `want` to the SDL device to open, with `how` saying which rule decided. default (or empty) gives
// SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, which SDL keeps pointed at the system default when Windows changes it. An all-digit
// value is a 1-based index into playback_devices; any other text is a case-insensitive substring of SDL_GetAudioDeviceName,
// and the first device in list order that contains it wins. A value that matches nothing also gives the default device
// (the game is never silent because of this setting); `say` then logs the playback devices. SDL audio must be initialised.
static SDL_AudioDeviceID resolve_audio_device(const char *want, char *how, size_t how_n, int say) {
    if (!want || !*want || !SDL_strcasecmp(want, "default")) {
        snprintf(how, how_n, "NX2NAVI_AUDIO_DEVICE=default: the system default playback device");
        return SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    }
    int count, matches = 0;
    SDL_AudioDeviceID *ids = playback_devices(&count), pick = 0;
    if (want[strspn(want, "0123456789")] == 0) {
        long index = strtol(want, NULL, 10);   // more digits than a long holds: out of range either way
        if (index >= 1 && index <= count) {
            pick = ids[index - 1];
            snprintf(how, how_n, "NX2NAVI_AUDIO_DEVICE=%s: index %ld in the device list", want, index);
        } else {
            snprintf(how, how_n, "NX2NAVI_AUDIO_DEVICE=%s: no device has that index (%d playback devices), using the system "
                                 "default", want, count);
        }
    } else {
        for (int i = 0; i < count; i++) {
            const char *name = SDL_GetAudioDeviceName(ids[i]);
            if (!name || !SDL_strcasestr(name, want)) continue;
            if (!pick) pick = ids[i];
            matches++;
        }
        if (pick)
            snprintf(how, how_n, "NX2NAVI_AUDIO_DEVICE=%s: the first of %d device name%s containing it", want, matches,
                     matches == 1 ? "" : "s");
        else
            snprintf(how, how_n, "NX2NAVI_AUDIO_DEVICE=%s: no device name contains it, using the system default", want);
    }
    SDL_free(ids);
    if (!pick && say) {
        rt_log("audio: %s; playback devices:", how);
        log_playback_devices();
    }
    return pick ? pick : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
}

// --list-audio-devices (main.c): each playback device with its index, name, channels and frequency, the one the system
// default points at, then what NX2NAVI_AUDIO_DEVICE would pick. Needs no game files. Returns the exit code.
int list_audio_devices(void) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL audio init failed: %s\n", SDL_GetError());
        return 1;
    }
    int count;
    SDL_AudioDeviceID *ids = playback_devices(&count);
    const char *def = SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
    char defname[128] = "";
    if (def) snprintf(defname, sizeof defname, "%s", def);
    if (!count) printf("no playback devices\n");
    for (int i = 0; i < count; i++) {
        char line[256];
        const char *name = SDL_GetAudioDeviceName(ids[i]);
        describe_device(line, sizeof line, ids[i]);
        printf("%d: %s%s\n", i + 1, line, *defname && name && !strcmp(name, defname) ? "  (default)" : "");
    }
    SDL_free(ids);
    char how[256];
    SDL_AudioDeviceID id = resolve_audio_device(cfg_value("NX2NAVI_AUDIO_DEVICE"), how, sizeof how, 0);
    const char *name = SDL_GetAudioDeviceName(id);
    printf("%s -> '%s'\n", how, name ? name : "?");
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return 0;
}

// NX2NAVI_AUDIO_CHANNELS: the output channels, decided at the first call and again when the setting changes
// (audio_channels_live). stereo gives 2; 5.1 gives 6 (FL FR C LFE RL RR,
// the device sink's order and SDL's); auto gives 6 when the chosen playback device (NX2NAVI_AUDIO_DEVICE; the system default unless set) has at least six channels (a 5.1
// or 7.1 speaker setup in Windows; SDL places 5.1 on 7.1) and 2 otherwise. IAudioDevice reports the same count to the
// game (GetActiveChannelCount), which picks its sink layout from it. NX2NAVI_AUDIO=none: 2, without touching SDL.
static _Atomic int decided;   // 0: output_channels decides at its next call
static int output_channels(void) {
    int n = atomic_load(&decided);
    if (n) return n;
    const char *want = cfg_value("NX2NAVI_AUDIO_CHANNELS"), *mode = cfg_get("NX2NAVI_AUDIO");
    if ((mode && !strcmp(mode, "none")) || cfg_get("NX2NAVI_AUDIO_OFF")) n = 2;
    else if (!strcmp(want, "5.1")) n = 6;
    else if (!strcmp(want, "stereo")) n = 2;
    else {
        SDL_AudioSpec dev = {0};
        char how[256];
        n = 2;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            // The choice output_open makes again (only that one logs the playback devices when nothing matches).
            SDL_AudioDeviceID id = resolve_audio_device(cfg_value("NX2NAVI_AUDIO_DEVICE"), how, sizeof how, 0);
            if (SDL_GetAudioDeviceFormat(id, &dev, NULL) && dev.channels >= 6) n = 6;
        }
        rt_log("audio: NX2NAVI_AUDIO_CHANNELS=auto: the chosen device has %d channels, output %s", dev.channels,
               n == 6 ? "5.1" : "stereo");
    }
    int expected = 0;
    return atomic_compare_exchange_strong(&decided, &expected, n) ? n : expected;
}

// Fills `frames` output frames from the ring (zeros for what is missing). Lock-free; runs on SDL's audio thread.
static void ring_pull(Mixer *m, float *out, uint32_t frames) {
    const int oc = m->channels;
    atomic_fetch_add(&m->callback_frames, frames);
    unsigned epoch = atomic_load_explicit(&m->stop_epoch, memory_order_acquire);
    uint64_t rd = atomic_load(&m->rd), wr = atomic_load_explicit(&m->wr, memory_order_acquire), have = wr - rd;
    if (epoch != m->callback_epoch) {   // the renderer stopped: drop what was queued before the stop
        uint64_t until = atomic_load_explicit(&m->flush_until, memory_order_acquire);
        if (until > rd) {
            rd = until < wr ? until : wr;
            atomic_store_explicit(&m->rd, rd, memory_order_release);
        }
        have = wr - rd;
        m->callback_epoch = epoch;
        m->ready = 0;
        m->gap = 0;
    }
    if (!atomic_load_explicit(&m->playing, memory_order_acquire)) {
        memset(out, 0, (size_t)frames * oc * sizeof(float));
        m->ready = 0;
        m->gap = 0;
        atomic_store_explicit(&m->rd, wr, memory_order_release);
        return;
    }
    uint64_t prefill = (uint64_t)m->frame * (uint64_t)m->prefill;
    if (prefill < frames) prefill = frames;
    // no more than the target the renderer fills the ring to (output_open caps it at half the ring): a longer
    // prefill (audio_prefill_ms of 180 and up) was never reached, and the output stayed silent for good
    if (prefill > RING_FRAMES / 2) prefill = RING_FRAMES / 2;
    if (!m->ready) {
        if (have < prefill) {
            memset(out, 0, (size_t)frames * oc * sizeof(float));
            // an underrun's silence goes on until the prefill is queued again (audio_mixer_owed); the wait for the
            // first prefill after a start is not such a gap
            if (m->gap) atomic_fetch_add_explicit(&m->silent_frames, frames, memory_order_release);
            return;
        }
        m->ready = 1;
        m->gap = 0;
    }
    uint64_t keep = (prefill > (uint64_t)m->target ? prefill : (uint64_t)m->target) + frames;
    uint64_t slack = (uint64_t)m->rate * QUEUE_SLACK_MS / 1000;
    if (have > keep + slack) {   // piled up while the device stalled: skip the oldest, keep target + request
        atomic_fetch_add(&m->skipped_frames, have - keep);
        rd = wr - keep;
        have = keep;
    }
    uint32_t n = frames < have ? frames : (uint32_t)have;
    uint64_t nonzero = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t k = (rd + i) % RING_FRAMES;
        int sound = 0;
        for (int c = 0; c < oc; c++) {
            out[oc * i + c] = m->ring[oc * k + c];
            sound |= out[oc * i + c] != 0;
        }
        nonzero += sound;
    }
    atomic_fetch_add(&m->callback_nonzero, nonzero);
    if (n < frames) {
        memset(out + (size_t)oc * n, 0, (size_t)(frames - n) * oc * sizeof(float));
        atomic_fetch_add(&m->underruns, 1);
        atomic_fetch_add(&m->missing_frames, frames - n);
        atomic_fetch_add_explicit(&m->silent_frames, frames - n, memory_order_release);
        m->ready = 0;
        m->gap = 1;
    }
    atomic_store_explicit(&m->rd, rd + n, memory_order_release);
}

static void SDLCALL sdl_pull(void *user, SDL_AudioStream *stream, int additional, int total) {
    (void)total;
    Mixer *m = user;
    float tmp[OUT_MAX * 1024];
    const uint32_t frame_bytes = (uint32_t)m->channels * sizeof(float);
    uint64_t now = kern_now_ns(), prev = atomic_exchange_explicit(&m->pulled_ns, now, memory_order_acq_rel);
    if (prev && now > prev && now - prev > atomic_load_explicit(&m->pull_gap_max, memory_order_relaxed))
        atomic_store_explicit(&m->pull_gap_max, now - prev, memory_order_relaxed);   // a lost race only loses a sample
    while (additional > 0) {
        uint32_t frames = (uint32_t)additional / frame_bytes;
        if (!frames) break;
        if (frames > 1024) frames = 1024;
        ring_pull(m, tmp, frames);
        menu_sound_mix(tmp, frames, (int)m->channels, (int)m->rate);   // the settings menu's sounds (menu_sound.c)
        if (!SDL_PutAudioStreamData(stream, tmp, (int)(frames * frame_bytes))) break;
        additional -= (int)(frames * frame_bytes);
    }
}

static void output_timing(Mixer *m, int rate);

// Opens an SDL stream on `dev` and starts it. Returns 0 with the reason in `err` otherwise (no stream is left behind).
static int stream_open(Mixer *m, int rate, SDL_AudioDeviceID dev, char *err, size_t err_n) {
    SDL_AudioSpec spec = {SDL_AUDIO_F32, m->channels, rate};
    m->stream = SDL_OpenAudioDeviceStream(dev, &spec, sdl_pull, m);
    if (!m->stream) {
        snprintf(err, err_n, "opening the device failed: %s", SDL_GetError());
        return 0;
    }
    if (!SDL_ResumeAudioStreamDevice(m->stream)) {
        snprintf(err, err_n, "starting the output failed: %s", SDL_GetError());
        SDL_DestroyAudioStream(m->stream);
        m->stream = NULL;
        return 0;
    }
    return 1;
}

static void output_open(Mixer *m, int rate) {
    const char *mode = cfg_get("NX2NAVI_AUDIO");
    if ((mode && !strcmp(mode, "none")) || cfg_get("NX2NAVI_AUDIO_OFF")) {
        rt_log("audio: NX2NAVI_AUDIO=none: mixing without a device");
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        rt_log("audio: SDL audio init failed (%s): mixing without sound", SDL_GetError());
        return;
    }
    char how[256], err[256];
    SDL_AudioDeviceID dev = resolve_audio_device(cfg_value("NX2NAVI_AUDIO_DEVICE"), how, sizeof how, 1);
    const char *chosen = dev == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ? NULL : SDL_GetAudioDeviceName(dev);
    if (chosen) snprintf(m->device_name, sizeof m->device_name, "%s", chosen);
    int ok = stream_open(m, rate, dev, err, sizeof err);
    if (!ok && chosen) {   // the chosen device would not open: the default device, so the game still has sound
        rt_log("audio: %s: '%s' %s; using the system default, playback devices:", how, m->device_name, err);
        log_playback_devices();
        dev = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
        m->device_name[0] = 0;
        ok = stream_open(m, rate, dev, err, sizeof err);
    } else {
        rt_log("audio: %s", how);
    }
    if (!ok) {
        rt_log("audio: no output device (%s): mixing without sound", err);
        return;
    }
    m->device_ok = 1;
    m->device = dev == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ? 0 : dev;
    output_timing(m, rate);
}

// After stream_open: the device's period, the output target that follows from it, and the stall limit. Logs the device.
static void output_timing(Mixer *m, int rate) {
    SDL_AudioSpec dev = {0};
    int period = 0;
    if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(m->stream), &dev, &period) || period <= 0) period = m->frame;
    m->period = period;
    m->target = m->frame * m->prefill > 2 * period ? m->frame * m->prefill : 2 * period;
    if (m->target > RING_FRAMES / 2) m->target = RING_FRAMES / 2;
    uint64_t period_ns = (uint64_t)period * 1000000000ull / (uint64_t)rate;
    m->stall_ns = 4 * period_ns > STALL_MS * 1000000ull ? 4 * period_ns : STALL_MS * 1000000ull;
    const char *name = SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(m->stream));
    rt_log("audio: SDL device '%s' opened, %d Hz %s float (device %d channels), prefill %d ms, device period %d frames; "
           "the device paces rendering at %.1f ms queued", name ? name : "?", rate, m->channels == 6 ? "5.1" : "stereo",
           dev.channels, m->prefill * 5, period, m->target * 1000.0 / rate);
}

// Render thread (audio_mixer_render): the output takes another channel count after NX2NAVI_AUDIO_CHANNELS changed
// (audio_channels_live). The stream is made again with the new count on the device it was on: destroying it waits
// for the SDL callback, the only other reader of the ring and of m->channels, and what was queued in the old layout
// is dropped.
static void channels_switch(Mixer *m, int channels) {
    if (m->stream) SDL_DestroyAudioStream(m->stream);
    m->stream = NULL;
    atomic_store_explicit(&m->rd, atomic_load(&m->wr), memory_order_release);
    m->channels = channels;
    m->ready = 0;
    m->gap = 0;
    m->reported &= ~(1u << 6);   // the device sink's line again, with the new output
    atomic_store_explicit(&m->pulled_ns, 0, memory_order_release);
    rt_log("audio: the output is now %s", channels == 6 ? "5.1" : "stereo");
    if (!m->device_ok) return;   // mixing without a device
    char err[256];
    if (stream_open(m, m->rate, m->device ? m->device : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, err, sizeof err)) {
        output_timing(m, m->rate);
    } else {
        m->device_ok = 0;
        rt_log("audio: no output device (%s): mixing without sound", err);
    }
}

// Render thread, from audio_mixer_pace_ns while the device takes nothing. An explicitly chosen device (NX2NAVI_AUDIO_DEVICE)
// can disappear (a virtual sink removed, a USB headset unplugged); SDL keeps only the system default device alive across
// that, so the stream of a chosen one just stops being pulled, and nothing in this file polls SDL's event queue for the
// removal. This looks once a second whether SDL still knows the device and, if not, moves the output to the system
// default; if that fails too, the mixer goes on without a device, as with NX2NAVI_AUDIO=none. Destroying the stream waits
// for the SDL callback (sdl_pull), which takes no lock the render thread holds.
static void device_check(Mixer *m, uint64_t now) {
    if (now - m->watch_ns < 1000000000ull) return;
    m->watch_ns = now;
    if (SDL_GetAudioDeviceName(m->device)) return;
    rt_log("audio: device '%s' is gone; moving the output to the system default device", m->device_name);
    SDL_DestroyAudioStream(m->stream);
    m->stream = NULL;
    m->device = 0;
    m->device_name[0] = 0;
    m->ready = 0;
    m->gap = 0;
    atomic_store_explicit(&m->pulled_ns, 0, memory_order_release);
    char err[256];
    if (stream_open(m, m->rate, SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, err, sizeof err)) {
        output_timing(m, m->rate);
    } else {
        m->device_ok = 0;
        rt_log("audio: no output device (%s): mixing without sound", err);
    }
}

int64_t audio_mixer_pace_ns(AudioRenderer *r, uint64_t now) {
    Mixer *m = r->mixer;
    if (!m || !m->device_ok || !atomic_load_explicit(&m->playing, memory_order_acquire)) return -1;
    uint64_t pulled = atomic_load_explicit(&m->pulled_ns, memory_order_acquire);
    if (!pulled || (now > pulled && now - pulled > m->stall_ns)) {
        if (m->device) device_check(m, now);
        return -1;
    }
    uint64_t have = atomic_load(&m->wr) - atomic_load_explicit(&m->rd, memory_order_acquire);
    if (have < (uint64_t)m->target) return 0;
    int64_t ns = (int64_t)((have - (uint64_t)m->target + 1) * 1000000000ull / (uint64_t)m->rate);
    return ns < 500000 ? 500000 : ns;   // until the device has taken it below the target
}

// Catch-up after a stall (NX2NAVI_AUDIO_CATCHUP, audren.c render_thread), for the frames the device paces. While the
// render thread is held up the device goes on taking periods: the ring runs out and ring_pull plays silence, first for
// the rest of that request (the underrun) and then until the prefill is queued again. The device's clock went on by
// that silence and the game's sound did not, and audio_mixer_pace_ns only fills the ring to its target again, so
// everything after the gap would be heard that much later than the picture it belongs to, and every further gap would
// add to it. This says whether a whole render frame of such silence is still owed and takes it off the debt; the
// caller renders that frame with `drop` set (audio_mixer_render), which moves the game's sound on and queues nothing,
// so the ring is filled to the same target afterwards. The debt is counted in the device's own samples, not in host
// time: the two clocks cannot drift into it, and a gap of less than a frame waits for the next one. Whatever was owed
// when the renderer stopped is forgotten, and so is a debt of more than AUDIO_CATCHUP_MAX_MS, with -1 returned: after
// that long the sound goes on from now.
int audio_mixer_owed(AudioRenderer *r) {
    Mixer *m = r->mixer;
    if (!m || !m->device_ok) return 0;
    unsigned epoch = atomic_load_explicit(&m->stop_epoch, memory_order_acquire);
    uint64_t silent = atomic_load_explicit(&m->silent_frames, memory_order_acquire);
    if (epoch != m->owed_epoch) {
        m->owed_epoch = epoch;
        m->made_up = silent;
    }
    uint64_t owed = silent - m->made_up;
    if (owed < (uint64_t)m->frame) return 0;
    if (owed > (uint64_t)m->rate * AUDIO_CATCHUP_MAX_MS / 1000) {
        m->made_up = silent;
        return -1;
    }
    m->made_up += (uint64_t)m->frame;
    return 1;
}

int audio_mixer_out_stats(AudioRenderer *r, AudioOutStats *o) {
    Mixer *m = r->mixer;
    if (!m || !m->device_ok) return 0;
    o->underruns = atomic_load(&m->underruns);
    o->missing_frames = atomic_load(&m->missing_frames);
    o->skipped_frames = atomic_load(&m->skipped_frames);
    o->overruns = atomic_load(&m->overruns);
    o->queued_frames = atomic_load(&m->wr) - atomic_load_explicit(&m->rd, memory_order_acquire);
    o->pull_gap_max_ns = atomic_exchange(&m->pull_gap_max, 0);
    return 1;
}

// Render thread only.
static void output_push(Mixer *m, const float *out, int frames) {
    const int oc = m->channels;
    uint64_t wr = atomic_load(&m->wr), rd = atomic_load_explicit(&m->rd, memory_order_acquire);
    if (m->device_ok && wr + (uint64_t)frames - rd > RING_FRAMES) {   // device stalled: drop this frame
        atomic_fetch_add(&m->overruns, 1);
        return;
    }
    for (int i = 0; i < frames; i++) {
        uint64_t k = (wr + (uint64_t)i) % RING_FRAMES;
        memcpy(&m->ring[oc * k], &out[oc * i], (size_t)oc * sizeof(float));
    }
    atomic_store_explicit(&m->wr, wr + (uint64_t)frames, memory_order_release);
    if (!m->device_ok) atomic_store(&m->rd, wr + (uint64_t)frames);   // no device: the ring only keeps the latest frames
}

// NX2NAVI_AUDIO_DUMP=<path>: the sink mix before the clamp as raw interleaved float32 stereo, and every 5 s one log line with
// how many frames were rendered less than 1 ms after the one before (a burst refill). File writes on the render thread:
// for diagnosis only.
static void sink_dump(const float *raw, int frames) {
    static FILE *f;
    static int opened;
    static uint64_t last, window_start, total, bursts;
    if (!opened) {
        opened = 1;
        const char *path = cfg_get("NX2NAVI_AUDIO_DUMP");
        if (path && *path) f = fopen(path, "wb");
    }
    if (!f) return;
    fwrite(raw, sizeof(float), (size_t)frames * 2, f);
    uint64_t now = kern_now_ns();
    total++;
    if (last && now - last < 1000000ull) bursts++;
    last = now;
    if (!window_start) window_start = now;
    if (now - window_start >= 5000000000ull) {
        rt_log("audio: dump: %llu frames in %.1f s, %llu rendered under 1 ms after the previous", (unsigned long long)total,
               (now - window_start) / 1e9, (unsigned long long)bursts);
        total = bursts = 0;
        window_start = now;
        fflush(f);
    }
}

// Level meter (NX2NAVI_TRACE=audio): one line per 5 s of output, and the first nonzero frame.
static void level_note(AudioRenderer *r, Mixer *m, const float *out, int frames) {
    if (!(rt_trace & RT_TRACE_AUDIO)) return;
    int sound = 0;
    for (int k = 0; k < m->channels * frames; k++) {
        double x = out[k];
        m->level_sum += x * x;
        if (fabs(x) > m->level_peak) m->level_peak = fabs(x);
        if (x != 0) sound = 1;
    }
    m->level_count += (uint64_t)m->channels * (uint64_t)frames;
    m->level_frames += (uint64_t)frames;
    if (sound && !m->first_sound_logged) {
        m->first_sound_logged = 1;
        rt_log("audio: first nonzero output after %llu frames", (unsigned long long)r->elapsed_frames);
    }
    if (m->level_frames >= (uint64_t)r->param.sample_rate * 5) {
        uint64_t wr = atomic_load(&m->wr), queued = wr - atomic_load(&m->rd);
        rt_log("audio: level rms %.4f peak %.4f over %.1f s; written %llu frames, underruns %llu, overruns %llu, "
               "missing frames %llu, callback frames %llu (nonzero %llu), queued %.1f ms, skipped %llu frames",
               sqrt(m->level_sum / (double)m->level_count), m->level_peak, m->level_frames / (double)r->param.sample_rate,
               (unsigned long long)wr, (unsigned long long)atomic_load(&m->underruns),
               (unsigned long long)atomic_load(&m->overruns), (unsigned long long)atomic_load(&m->missing_frames),
               (unsigned long long)atomic_load(&m->callback_frames), (unsigned long long)atomic_load(&m->callback_nonzero),
               queued * 1000.0 / r->param.sample_rate, (unsigned long long)atomic_load(&m->skipped_frames));
        double f = (double)m->level_frames;
        rt_log("audio: device sink inputs rms FL %.4f FR %.4f C %.4f LFE %.4f RL %.4f RR %.4f, output %s",
               sqrt(m->input_sum[0] / f), sqrt(m->input_sum[1] / f), sqrt(m->input_sum[2] / f), sqrt(m->input_sum[3] / f),
               sqrt(m->input_sum[4] / f), sqrt(m->input_sum[5] / f), m->channels == 6 ? "5.1" : "stereo");
        memset(m->input_sum, 0, sizeof m->input_sum);
        m->level_sum = m->level_peak = 0;
        m->level_count = m->level_frames = 0;
    }
}

void audio_mixer_open(AudioRenderer *r) {
    Mixer *m = calloc(1, sizeof *m);
    if (!m) rt_fatal("audio mixer allocation");
    m->frame = r->param.sample_count;
    m->nbuf = r->param.mix_buffer_count;
    m->rate = r->param.sample_rate;
    if (m->frame <= 0 || m->frame > MAX_FRAME || m->nbuf <= 0 || m->nbuf > 512) rt_fatal("audio mixer frame / buffer count");
    m->buf = calloc((size_t)m->nbuf * (size_t)m->frame, sizeof(float));
    m->offset = calloc((size_t)r->param.submix_count + 1, sizeof(int));
    m->nsplit = r->param.splitter_count;
    m->ndest = r->param.splitter_destination_count;
    m->split = calloc((size_t)(m->nsplit ? m->nsplit : 1), sizeof *m->split);
    m->dest = calloc((size_t)(m->ndest ? m->ndest : 1), sizeof *m->dest);
    m->mix_prev = calloc((size_t)r->param.submix_count + 1, sizeof *m->mix_prev);
    if (!m->buf || !m->offset || !m->split || !m->dest || !m->mix_prev) rt_fatal("audio mixer tables");
    m->effect = calloc((size_t)(r->param.effect_count ? r->param.effect_count : 1), sizeof *m->effect);
    m->effect_order = calloc((size_t)(r->param.effect_count ? r->param.effect_count : 1), sizeof *m->effect_order);
    if (!m->effect || !m->effect_order) rt_fatal("audio effect tables");
    size_t sinks = (size_t)r->param.sink_count;
    m->sink_address = calloc(sinks, sizeof(uint64_t));
    m->sink_size = calloc(sinks, sizeof(uint32_t));
    m->sink_offset = calloc(sinks, sizeof(uint32_t));
    m->sink_old = calloc(sinks, sizeof(uint32_t));
    m->sink_written = calloc(sinks, sizeof(uint32_t));
    if (!m->sink_address || !m->sink_size || !m->sink_offset || !m->sink_old || !m->sink_written)
        rt_fatal("audio sink state allocation");
    const char *pe = cfg_get("NX2NAVI_AUDIO_PREFILL_MS");
    m->prefill = prefill_blocks_for(pe);
    if (m->prefill < 0) {
        rt_log("audio: NX2NAVI_AUDIO_PREFILL_MS=%s is not 0..500 ms: using %d", pe, PREFILL_DEFAULT_MS);
        m->prefill = PREFILL_DEFAULT_MS / 5;
    }
    m->channels = output_channels();
    m->ramp = cfg_bool("NX2NAVI_AUDIO_RAMP");
    m->snap = 1;
    rt_log("audio: NX2NAVI_AUDIO_RAMP=%d: a changed volume %s", m->ramp,
           m->ramp ? "is reached in equal steps across the frame" : "applies to the whole frame at once");
    r->mixer = m;
    atomic_store(&m->playing, r->state == 0);
    output_open(m, r->param.sample_rate);
}

void audio_mixer_set_running(AudioRenderer *r, int running) {
    Mixer *m = r->mixer;
    if (!m) return;
    if (!running) {
        atomic_store_explicit(&m->flush_until, atomic_load_explicit(&m->wr, memory_order_acquire), memory_order_release);
        atomic_fetch_add_explicit(&m->stop_epoch, 1, memory_order_release);
    } else {
        m->snap = 1;   // what was mixed before the stop is not the sample before the next pass's first
    }
    atomic_store_explicit(&m->playing, running, memory_order_release);
}

void audio_mixer_close(AudioRenderer *r) {
    Mixer *m = r->mixer;
    if (!m) return;
    if (m->stream) {   // destroying the stream closes the device and waits for the callback to finish
        SDL_DestroyAudioStream(m->stream);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    for (int i = 0; i < m->nsplit; i++) free(m->split[i].ids);
    for (int i = 0; i < r->param.effect_count; i++) {
        audio_reverb_free(m->effect[i].reverb);
        audio_effect_free(m->effect[i].dsp);
        free(m->effect[i].delay);
    }
    free(m->sink_address);
    free(m->sink_size);
    free(m->sink_offset);
    free(m->sink_old);
    free(m->sink_written);
    free(m->effect);
    free(m->effect_order);
    free(m->buf);
    free(m->offset);
    free(m->mix_prev);
    free(m->split);
    free(m->dest);
    free(m);
    r->mixer = NULL;
}

/* ---------- routing input: splitters and mixes ---------- */
static void splitters_update(AudioRenderer *r, const uint8_t *p, uint32_t n) {
    Mixer *m = r->mixer;
    if (!n) return;
    uint32_t hdr[3];
    if (n < 0x20) rt_fatal("audio splitter section header");
    memcpy(hdr, p, 12);
    if (hdr[0] != 0x48444e53u) rt_fatal("audio splitter header magic (SNDH)");
    uint32_t ni = hdr[1], nd = hdr[2], at = 0x20;
    for (uint32_t i = 0; i < ni; i++) {
        uint32_t h[4]; if (at + 16 > n) rt_fatal("audio splitter info overruns section");
        memcpy(h, p + at, 16); at += 16;
        if (h[0] != 0x49444e53u || h[1] >= (uint32_t)m->nsplit || h[3] > (uint32_t)m->ndest || at + 4u * h[3] > n) {
            rt_log("audio: splitter info %u at %#x: %08x %08x %08x %08x (section %u bytes, infos %u, destinations %u)", i, at - 16, h[0], h[1], h[2], h[3], n, ni, nd);
            rt_fatal("audio splitter info (SNDI)");
        }
        SplitInfo *s = &m->split[h[1]]; free(s->ids);
        s->ndest = (int32_t)h[3]; s->ids = calloc(h[3] ? h[3] : 1, sizeof(int32_t));
        if (!s->ids) rt_fatal("audio splitter destinations");
        memcpy(s->ids, p + at, 4u * h[3]); at += 4u * h[3] + 12;   /* 12 reserved bytes follow the ids (measured, NX2NAVI REV11) */
        if (at > n) rt_fatal("audio splitter info padding overruns section");
        for (uint32_t k = 0; k < h[3]; k++) if (s->ids[k] < 0 || s->ids[k] >= m->ndest) rt_fatal("audio splitter destination id");
    }
    for (uint32_t i = 0; i < nd; i++) {
        unsigned rev=(r->revision>>24)-'0',stride=rev>=15?0xa8:rev>=12?0x90:0x70;
        uint8_t e[0xa8]={0}; if (at>n||stride>n-at) rt_fatal("audio splitter destination overruns section");
        memcpy(e, p + at, stride); at += stride;
        uint32_t magic, id; int32_t dst; memcpy(&magic, e, 4); memcpy(&id, e + 4, 4); memcpy(&dst, e + 0x68, 4);
        if (magic != 0x44444e53u || id >= (uint32_t)m->ndest) rt_fatal("audio splitter destination (SNDD)");
        SplitDest *d = &m->dest[id]; memcpy(d->vol, e + 8, sizeof d->vol); d->dest = dst;
        d->used=e[rev>=15?0x9c:rev>=12?0x84:0x6c];
        for(unsigned f=0;f<2;f++){
            AudrenBiquadFloat b={0};
            if(rev>=15)memcpy(&b,e+0x6c+f*sizeof b,sizeof b);
            else if(rev>=12){AudrenBiquad q;memcpy(&q,e+0x6c+f*sizeof q,sizeof q);b.enable=q.enable;for(unsigned c=0;c<3;c++)b.numerator[c]=q.numerator[c]/16384.0f;for(unsigned c=0;c<2;c++)b.denominator[c]=q.denominator[c]/16384.0f;}
            if(b.enable){for(unsigned c=0;c<3;c++)if(!isfinite(b.numerator[c]))rt_fatal("audio splitter nonfinite numerator");for(unsigned c=0;c<2;c++)if(!isfinite(b.denominator[c]))rt_fatal("audio splitter nonfinite denominator");}
            if(!b.enable||!d->biquads[f].enable||!d->used)memset(d->state[f],0,sizeof d->state[f]);
            d->biquads[f]=b;
        }
    }
    if (at <= n && n - at < 16) at = n;   /* the section is padded to 16 bytes; the padding is not cleared by the game (measured) */
    if (at != n) { rt_log("audio: splitter section %u bytes, parsed %u (infos %u, destinations %u)", n, at, ni, nd); rt_fatal("audio splitter section size"); }
}
void audio_mixer_update_routing(AudioRenderer *r, const uint8_t *splitters, uint32_t splitter_bytes, const uint8_t *mixes, uint32_t mix_bytes, int mix_count) {
    splitters_update(r, splitters, splitter_bytes);
    if (mix_bytes == (uint32_t)mix_count * sizeof(AudrenMixIn)) { memcpy(r->mixes, mixes, mix_bytes); return; }
    /* MixInParameterDirtyOnly: 0x20-byte header {u32 magic 0xCAFEBABE, u32 count, pad} followed by the dirty mixes */
    if (mix_bytes >= 0x20 && (mix_bytes - 0x20) % sizeof(AudrenMixIn) == 0) {
        uint32_t magic, n; memcpy(&magic, mixes, 4); memcpy(&n, mixes + 4, 4);
        if (magic != 0xcafebabeu || n * sizeof(AudrenMixIn) != mix_bytes - 0x20) rt_fatal("audio dirty mix header");
        for (uint32_t i = 0; i < n; i++) {
            AudrenMixIn m; memcpy(&m, mixes + 0x20 + i * sizeof m, sizeof m);
            if (m.mix_id < 0 || m.mix_id >= mix_count) rt_fatal("audio mix id");
            r->mixes[m.mix_id] = m;
        }
        return;
    }
    rt_log("audio: mix section %u bytes for %d mixes", mix_bytes, mix_count);
    rt_fatal("audio mix section size");
}

/* Effect parameters arrive once per IPC update, while the DSP may process multiple frames. */
void audio_mixer_update_effects(AudioRenderer *r) {
    Mixer *m = r->mixer;
    for (int i=0;i<r->param.effect_count;i++) {
        const AudrenEffectIn *e=&r->effects[i]; EffectState *s=&m->effect[i];
        unsigned revision=audio_revision(r);
        int reset=e->is_new || s->type!=e->type;
        if(reset){audio_effect_free(s->dsp);s->dsp=NULL;memset(s->biquad_z,0,sizeof s->biquad_z);}
        if(e->type==3 || e->type==4 || e->type==9) {
            unsigned status,channels;int dimensions_changed=0;
            if(e->type==3){AudrenDelayParam p,old;memcpy(&p,e->specific,sizeof p);memcpy(&old,s->wire,sizeof old);status=p.status;channels=p.channels;dimensions_changed=p.channels!=old.channels || p.sample_rate!=old.sample_rate || p.delay_max!=old.delay_max;}
            else if(e->type==4){AudrenReverbParam p,old;memcpy(&p,e->specific,sizeof p);memcpy(&old,s->wire,sizeof old);status=p.status;channels=p.channels;dimensions_changed=p.channels!=old.channels || p.sample_rate!=old.sample_rate;}
            else {AudrenCompressorParam p,old;memcpy(&p,e->specific,sizeof p);memcpy(&old,s->wire,sizeof old);status=p.status;channels=p.channels;dimensions_changed=p.channels!=old.channels;}
            if(status==0 || dimensions_changed){audio_effect_free(s->dsp);s->dsp=NULL;}
            if(e->enabled && channels){if(!s->dsp)s->dsp=audio_effect_create(e->type,e->specific,revision);else audio_effect_update(s->dsp,e->specific);}
        } else if(s->dsp){audio_effect_free(s->dsp);s->dsp=NULL;}
        if(e->type==6){
            unsigned status;
            if(revision>=15){AudrenBiquadEffectV2 p;memcpy(&p,e->specific,sizeof p);s->biquad_channels=p.channels;status=p.status;memcpy(s->numerator,p.numerator,sizeof p.numerator);memcpy(s->denominator,p.denominator,sizeof p.denominator);}
            else {AudrenBiquadEffectV1 p;memcpy(&p,e->specific,sizeof p);s->biquad_channels=p.channels;status=p.status;for(int k=0;k<3;k++)s->numerator[k]=p.numerator[k]/16384.f;for(int k=0;k<2;k++)s->denominator[k]=p.denominator[k]/16384.f;}
            if(status==0 || (status==1 && revision<6))memset(s->biquad_z,0,sizeof s->biquad_z);
            if(e->enabled && (s->biquad_channels<1 || s->biquad_channels>6))rt_fatal("biquad effect channel count");
        }
        s->type=e->type;memcpy(s->wire,e->specific,sizeof s->wire);
        if(e->type!=5 || e->is_new) { audio_reverb_free(s->reverb); s->reverb=NULL; s->pending=1; }
        if(e->type==7) {
            AudrenLimiterParam p; memcpy(&p,e->specific,sizeof p);
            if(e->is_new || !s->delay || p.status==0 || p.channels!=s->channels || p.delay_max!=s->delay_max) {
                free(s->delay);s->delay=NULL;s->delay_max=p.delay_max;s->channels=p.channels;
                memset(s->detector,0,sizeof s->detector);memset(s->position,0,sizeof s->position);
                for(int c=0;c<6;c++)s->compression[c]=s->minimum[c]=1;
                memset(s->peak,0,sizeof s->peak);
                if(e->enabled) {
                    if(p.channels<1 || p.channels>6 || p.delay_max<1 || p.delay_max>192000)rt_fatal("limiter delay dimensions");
                    s->delay=calloc((size_t)p.channels*p.delay_max,sizeof(float));if(!s->delay)rt_fatal("limiter delay allocation");
                }
            }
            if(p.statistics_reset) { memset(s->peak,0,sizeof s->peak); for(int c=0;c<6;c++)s->minimum[c]=1; }
        } else if(s->delay) { free(s->delay);s->delay=NULL; }
        if(e->type!=5) continue;
        Reverb3dParam p; memcpy(&p,e->specific,sizeof p);
        if(s->reverb && p.sample_rate!=s->param.sample_rate) { audio_reverb_free(s->reverb); s->reverb=NULL; }
        if(memcmp(&p,&s->param,72) || p.status<2) s->pending=1;
        s->param=p;
    }
}

/* ---------- voice decoding along the voice clock's sample path ---------- */
typedef struct { ServerVoice *v; unsigned index; uint64_t offset; int32_t loops; uint8_t valid[4]; int ended; int16_t h1, h2; int32_t abuf; uint64_t aat; } Walk;
static int fmt_bytes(int f) { return f == FMT_PCM8 ? 1 : f == FMT_PCM16 ? 2 : f==FMT_PCM24?3:f == FMT_PCM32 || f == FMT_PCMF32 ? 4 : 0; }
/* DSP-ADPCM: 8-byte frames of a predictor/scale byte and 14 4-bit samples; the history runs across samples. */
static int16_t adpcm_next(Walk *w, const AudrenWaveBuffer *b, const int16_t *coef, uint64_t i) {
    const uint8_t *d = (const uint8_t *)(uintptr_t)b->address;
    uint64_t frame = i / 14, nib = i % 14;
    if ((frame + 1) * 8 > b->size) return 0;
    uint8_t ps = d[frame * 8]; int scale = 1 << (ps & 15), pred = (ps >> 4) & 7;
    int n = (d[frame * 8 + 1 + nib / 2] >> ((nib & 1) ? 0 : 4)) & 15; if (n >= 8) n -= 16;
    int64_t s = (int64_t)n * scale * 2048 + 1024 + (int64_t)coef[pred * 2] * w->h1 + (int64_t)coef[pred * 2 + 1] * w->h2;
    s >>= 11; if (s > 32767) s = 32767; if (s < -32768) s = -32768;
    w->h2 = w->h1; w->h1 = (int16_t)s; return (int16_t)s;
}
static float sample_at(Walk *w, int ch) {
    if (w->ended || !w->valid[w->index]) return 0;
    ServerVoice *v = w->v; const AudrenWaveBuffer *b = &v->buffers[w->index];
    uint64_t i = (uint64_t)b->start + w->offset; int f = v->in.sample_format, cc = (int)v->in.channel_count;
    if (f == FMT_ADPCM) {
        if (cc != 1 || !v->in.extra_address || v->in.extra_size < 32) return 0;
        const int16_t *coef = (const int16_t *)(uintptr_t)v->in.extra_address;
        if (w->abuf != (int32_t)w->index || w->aat > i + 1) {   /* (re)start this buffer: from its context, else from the frame boundary with carried history */
            w->abuf = (int32_t)w->index;
            if (b->context_address && b->context_size >= 6) { const int16_t *c = (const int16_t *)(uintptr_t)b->context_address; w->h1 = c[1]; w->h2 = c[2]; w->aat = (uint64_t)b->start; }
            else w->aat = (uint64_t)b->start - (uint64_t)b->start % 14;
            if (i < w->aat) w->aat = i;
        }
        while (w->aat <= i) adpcm_next(w, b, coef, w->aat++);
        return w->h1 / 32768.0f;
    }
    int bytes = fmt_bytes(f); if (!bytes || ch >= cc) return 0;
    uint64_t at = (i * (uint64_t)cc + (uint64_t)ch) * (uint64_t)bytes;
    if (at + (uint64_t)bytes > b->size) return 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)b->address + at;
    switch (f) {
    case FMT_PCM8: return (int8_t)p[0] / 128.0f;
    case FMT_PCM16: { int16_t s; memcpy(&s, p, 2); return s / 32768.0f; }
    case FMT_PCM24: {uint32_t word=(uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16;return ((int32_t)(word<<8)>>8)/8388608.f;}
    case FMT_PCM32: { int32_t s; memcpy(&s, p, 4); return (float)(s / 2147483648.0); }
    default: { float s; memcpy(&s, p, 4); return s; }
    }
}
static void walk_skip_empty(Walk *w) {   /* the same as skip_empty in audio_renderer.c: no samples, played on arrival */
    while (!w->ended && w->valid[w->index] && audio_buffer_empty(&w->v->buffers[w->index])) {
        const AudrenWaveBuffer *b = &w->v->buffers[w->index];
        if (b->loop && b->loop_count < 0) break;
        w->valid[w->index] = 0; w->offset = 0; w->loops = 0; w->index = (w->index + 1) & 3;
        if (b->end_of_stream) w->ended = 1;
    }
}
static void walk_step(Walk *w) {   /* the same transitions as advance_voice in audio_renderer.c */
    if (w->ended || !w->valid[w->index]) { w->ended = 1; return; }
    const AudrenWaveBuffer *b = &w->v->buffers[w->index];
    uint64_t length = (uint64_t)(b->end - b->start);
    if (++w->offset < length) return;
    if (b->loop && (b->loop_count < 0 || w->loops < b->loop_count)) { w->loops++; w->offset = b->loop_start > b->start && b->loop_start < b->end ? (uint64_t)(b->loop_start - b->start) : 0; return; }
    w->valid[w->index] = 0; w->offset = 0; w->loops = 0; w->index = (w->index + 1) & 3;
    if (b->end_of_stream) w->ended = 1;
    walk_skip_empty(w);
}
/* Decodes `frames` output samples per channel into out[c][k]; the renderer advances the voice afterwards by the same amount. */
// Source samples per output sample. The guest's sample rate and pitch are not checked: an infinite or huge step
// would keep the per-sample loops below (audio_voice_consumed, voice_render) running for good under the renderer's
// lock, so it is held to 64 source samples per output sample, far above any pitch the game asks for.
static double voice_step(const AudioRenderer *r, const ServerVoice *v) {
    double step = v->in.sample_rate * (v->in.pitch > 0 ? v->in.pitch : 1.0) / r->param.sample_rate;
    return step >= 0 && step <= 64 ? step : step > 64 ? 64 : 0;   // NaN: no progress
}
/* Source samples the voice consumes over `frames` output samples, counted with the exact accumulation voice_render steps by
   (float rounding makes `fraction + seconds * rate * pitch` differ from it by one sample on some frames, a seam click);
   leaves the fractional position in v->fraction. */
uint64_t audio_voice_consumed(const AudioRenderer *r, ServerVoice *v, int frames) {
    const double step = voice_step(r, v);
    double frac = v->fraction;
    uint64_t consumed = 0;
    for (int k = 0; k < frames; k++) {
        frac += step;
        while (frac >= 1.0) { frac -= 1.0; consumed++; }
    }
    v->fraction = frac;
    return consumed;
}
static void voice_render(AudioRenderer *r, ServerVoice *v, float out[6][MAX_FRAME], int frames) {
    const double step = voice_step(r, v);
    Walk w = {v, v->index, v->offset, v->loops_done, {0}, 0, v->adpcm.history[0], v->adpcm.history[1], v->adpcm.buffer, v->adpcm.at};
    memcpy(w.valid, v->valid, sizeof w.valid);
    walk_skip_empty(&w);
    int cc = (int)v->in.channel_count; double frac = v->fraction;
    for (int k = 0; k < frames; k++) {
        Walk next = w; walk_step(&next);
        for (int c = 0; c < cc; c++) { float s0 = sample_at(&w, c), s1 = sample_at(&next, c); out[c][k] = (float)(s0 + (s1 - s0) * frac); }
        frac += step;
        while (frac >= 1.0) { frac -= 1.0; walk_step(&w); }
    }
    v->adpcm.history[0] = w.h1; v->adpcm.history[1] = w.h2; v->adpcm.buffer = w.abuf; v->adpcm.at = w.aat;
}

/* ---------- mixing ---------- */
static float *mixbuf(Mixer *m, int i) { return m->buf + (size_t)i * (size_t)m->frame; }
static void accumulate(Mixer *m, int buffer, const float *src, float vol, int frames) {
    if (vol == 0 || buffer < 0 || buffer >= m->nbuf) return;
    float *d = mixbuf(m, buffer); for (int k = 0; k < frames; k++) d[k] += src[k] * vol;
}
// Volume ramps (NX2NAVI_AUDIO_RAMP, default 1). The game sets volumes once per audio update, and a volume applied to a
// whole frame as one constant makes the level jump at the frame's first sample whenever the game moves it (a fade, a
// pan, a sound going past the camera): a step in the waveform, heard as a click. The console's renderer goes from the
// volume of the frame before to the new one across the frame's samples, and so does this: `from` is the gain the same
// source went into this buffer with in the render pass before, sample k gets from + k (to - from) / frames, and the
// next pass, which starts at `to`, continues the line. Equal gains take accumulate's constant path, and so does a gain
// that is not a finite number (NaN also marks "no gain in the pass before").
static void accumulate_ramp(Mixer *m, int buffer, const float *src, float from, float to, int frames) {
    if (from == to || !isfinite(from) || !isfinite(to)) { accumulate(m, buffer, src, to, frames); return; }
    if (buffer < 0 || buffer >= m->nbuf) return;
    const float step = (to - from) / (float)frames;
    float *d = mixbuf(m, buffer), gain = from;
    for (int k = 0; k < frames; k++) { d[k] += src[k] * gain; gain += step; }
    m->ramps++;
}
// A splitter destination ramps when the pass before used it and sent it to the same mix: one that is new or was moved
// starts at its new volumes.
static int dest_ramps(const SplitDest *d) { return d->prev_ok && d->prev_dest == d->dest; }
void audio_mixer_ramp_stats(const AudioRenderer *r, uint64_t *passes, uint64_t *ramps) {
    const Mixer *m = r->mixer;
    *passes = m ? m->ramp_passes : 0;
    *ramps = m ? m->ramps : 0;
}
static int mix_valid(AudioRenderer *r, int32_t id) { return id >= 0 && id <= r->param.submix_count && r->mixes[id].in_use; }
static int mix_depth(AudioRenderer *r, int id) {
    int d = 0;
    for (int guard = 0; id > 0 && guard < 64; guard++, d++) {
        if (!mix_valid(r, r->mixes[id].dest_mix_id)) return d + 1;
        id = r->mixes[id].dest_mix_id;
    }
    return d;
}

/* Guest custom effects exchange int32 samples in two rings. Each ring has CPU and DSP
   0x40-byte headers followed by samples; only the DSP header's owned cursor is advanced. */
static void mix_aux(AudioRenderer *r, int mix, const AudrenEffectIn *e) {
    Mixer *m=r->mixer; AudrenAuxParam p; memcpy(&p,e->specific,sizeof p);
    if(!p.send_info || !p.return_info || !p.capacity || !p.channels) return;
    if(p.channels>24 || p.capacity>16*1024*1024)rt_fatal("auxiliary effect dimensions");
    uint8_t *send=ipc_guest(p.send_info,128+(uint64_t)p.capacity*4), *ret=ipc_guest(p.return_info,128+(uint64_t)p.capacity*4);
    uint32_t *sh=(uint32_t *)(send+64),*rh=(uint32_t *)(ret+64); int32_t *sd=(int32_t *)(send+128),*rd=(int32_t *)(ret+128);
    uint32_t wr=sh[1],read=rh[0],total=p.channels*(uint32_t)m->frame;
    if(e->enabled) report_once(m,6,"auxiliary effect in use, channels",(int)p.channels);
    for(unsigned c=0;c<p.channels;c++) {
        if(p.input[c]>=r->mixes[mix].buffer_count || p.output[c]>=r->mixes[mix].buffer_count)rt_fatal("auxiliary effect mix buffer");
        float *input=mixbuf(m,m->offset[mix]+p.input[c]),*output=mixbuf(m,m->offset[mix]+p.output[c]);
        if(!e->enabled) { if(input!=output)memmove(output,input,m->frame*sizeof(float)); continue; }
        uint64_t w=(uint64_t)wr+c*m->frame,at=(uint64_t)read+c*m->frame;
        for(int k=0;k<m->frame;k++) {
            double scaled=trunc((double)input[k]*32768); int32_t value=scaled>INT32_MAX?INT32_MAX:scaled<INT32_MIN?INT32_MIN:!isfinite(scaled)?0:(int32_t)scaled;
            input[k]=value/32768.f;
            if(w<=p.capacity)sd[(w+(unsigned)k)%p.capacity]=value;
        }
        for(int k=0;k<m->frame;k++)output[k]=at<=p.capacity?rd[(at+(unsigned)k)%p.capacity]/32768.f:0;
        if(c==p.channels-1) { if(w<=p.capacity)sh[1]=(wr+total)%p.capacity; if(at<=p.capacity)rh[0]=(read+total)%p.capacity; }
    }
    if(!e->enabled) { sh[0]=sh[1]=sh[3]=0; rh[0]=rh[1]=rh[3]=0; }
}

void audio_mixer_effect_result(AudioRenderer *r, int id, void *output, unsigned size) {
    Mixer *m=r->mixer;
    if(size>=48 && r->effects[id].type==7) { memcpy(output,m->effect[id].peak,24);memcpy((uint8_t *)output+24,m->effect[id].minimum,24); }
    if(r->effects[id].type==9)audio_effect_statistics(m->effect[id].dsp,output,size);
}
static void mix_limiter(AudioRenderer *r, int mix, int id) {
    Mixer *m=r->mixer; EffectState *s=&m->effect[id]; const AudrenEffectIn *e=&r->effects[id];
    AudrenLimiterParam p;memcpy(&p,e->specific,sizeof p);
    if(p.channels<1 || p.channels>6) { if(e->enabled)rt_fatal("limiter channel count");return; }
    if(e->enabled && (!s->delay || p.delay_min<1 || p.delay_min>p.delay_max || p.attack<0 || p.attack>1 || p.release<0 || p.release>1))rt_fatal("limiter parameters");
    for(int c=0;c<p.channels;c++) {
        if(p.input[c]>=r->mixes[mix].buffer_count || p.output[c]>=r->mixes[mix].buffer_count)rt_fatal("limiter mix buffer");
        float *input=mixbuf(m,m->offset[mix]+p.input[c]),*output=mixbuf(m,m->offset[mix]+p.output[c]);
        if(!e->enabled) { if(input!=output)memmove(output,input,m->frame*sizeof(float));continue; }
        for(int k=0;k<m->frame;k++) {
            float x=(input[k]*(32768.f/32767.f))*p.input_gain,level=fabsf(x);
            float coeff=level>s->detector[c]?p.attack:p.release;s->detector[c]+=coeff*(level-s->detector[c]);
            float attenuation=s->detector[c]>p.threshold?p.threshold/s->detector[c]:1;
            coeff=s->compression[c]>attenuation?p.attack:p.release;s->compression[c]+=coeff*(attenuation-s->compression[c]);
            int slot=c*p.delay_max+s->position[c];output[k]=s->delay[slot]*s->compression[c]*p.output_gain*(32767.f/32768.f);
            s->delay[slot]=x;s->position[c]=(s->position[c]+1)%p.delay_min;
            if(p.statistics_enabled) { s->peak[c]=fmaxf(s->peak[c],level);s->minimum[c]=fminf(s->minimum[c],s->compression[c]); }
        }
    }
}

static int32_t effect_integer(float value) {
    double x=trunc((double)value*32768);return !isfinite(x)?0:x>INT32_MAX?INT32_MAX:x<INT32_MIN?INT32_MIN:(int32_t)x;
}
static void prepare_ring_write(uint8_t *base,uint32_t size,uint32_t at,uint64_t bytes) {
    if(bytes>=size){dm_guest_write_prepare(base,size);return;}
    uint32_t first=(uint32_t)bytes;if(first>size-at)first=size-at;
    dm_guest_write_prepare(base+at,first);if(bytes>first)dm_guest_write_prepare(base,(size_t)bytes-first);
}
/* SDK SampleCounter::Add advances the producer counter itself, not the
   producer-consumer difference or the ring cursor. Saturate on diff overflow. */
static void effect_counter_add(uint32_t cpu,uint32_t *dsp,uint32_t count) {
    uint32_t previous=*dsp-cpu,next=*dsp+count;
    *dsp=next-cpu<previous?cpu-1:next;
}
static void mix_capture(AudioRenderer *r,int mix,const AudrenEffectIn *e) {
    Mixer *m=r->mixer;AudrenAuxParam p;memcpy(&p,e->specific,sizeof p);
    if(!p.send_info || !p.capacity || !p.channels)return;
    if(p.channels>24 || p.capacity>16*1024*1024)rt_fatal("capture effect dimensions");
    uint8_t *base=ipc_guest(p.send_info,128+(uint64_t)p.capacity*4);uint32_t *cpu=(uint32_t *)base,*dsp=(uint32_t *)(base+64);int32_t *data=(int32_t *)(base+128);
    dm_guest_write_prepare(base+64,64);
    if(!e->enabled){dsp[0]=dsp[1]=dsp[3]=0;return;}
    uint32_t wr=dsp[1],total=p.channels*(uint32_t)m->frame;
    for(unsigned c=0;c<p.channels;c++){
        if(p.input[c]>=r->mixes[mix].buffer_count)rt_fatal("capture effect input buffer");
        float *in=mixbuf(m,m->offset[mix]+p.input[c]);uint64_t at=(uint64_t)wr+c*m->frame;
        if(at<=p.capacity)prepare_ring_write((uint8_t *)data,p.capacity*4,(uint32_t)(at%p.capacity)*4,(uint64_t)m->frame*4);
        for(int k=0;k<m->frame;k++){int32_t value=effect_integer(in[k]);in[k]=value/32768.f;if(at<=p.capacity)data[(at+(unsigned)k)%p.capacity]=value;}
    }
    if((uint64_t)wr+(p.channels-1)*(uint32_t)m->frame<=p.capacity){
        uint32_t diff=dsp[3]-cpu[3];
        if(diff>=p.capacity)effect_counter_add(cpu[2],&dsp[2],total);
        dsp[1]=(wr+total)%p.capacity;effect_counter_add(cpu[3],&dsp[3],total);
    }
}
static void mix_simple_effect(AudioRenderer *r,int mix,int id) {
    Mixer *m=r->mixer;const AudrenEffectIn *e=&r->effects[id];EffectState *s=&m->effect[id];
    if(e->type==1){
        AudrenBufferMixParam p;memcpy(&p,e->specific,sizeof p);if(!e->enabled)return;
        if(p.count>24)rt_fatal("buffer-mix effect count");
        for(unsigned c=0;c<p.count;c++){
            if(p.input[c]>=r->mixes[mix].buffer_count || p.output[c]>=r->mixes[mix].buffer_count)rt_fatal("buffer-mix effect index");
            float *in=mixbuf(m,m->offset[mix]+p.input[c]),*out=mixbuf(m,m->offset[mix]+p.output[c]);
            for(int k=0;k<m->frame;k++)out[k]+=ceilf(in[k]*32768*p.volume[c])/32768;
        }return;
    }
    unsigned channels;if(e->type==6)channels=s->biquad_channels;else {uint16_t n;memcpy(&n,e->specific+14,2);channels=n;}
    if(channels<1 || channels>6){if(e->enabled)rt_fatal("audio DSP effect channels");return;}
    float *in[6]={0},*out[6]={0};unsigned revision=audio_revision(r);
    for(unsigned c=0;c<channels;c++){
        unsigned idx=c;
        if(channels==6 && ((e->type==3 && revision<11)||(e->type==4 && revision>=11))){static const unsigned map[6]={0,1,4,5,2,3};idx=map[c];}
        if(e->specific[idx]>=r->mixes[mix].buffer_count || e->specific[6+idx]>=r->mixes[mix].buffer_count)rt_fatal("audio DSP effect buffer index");
        in[c]=mixbuf(m,m->offset[mix]+e->specific[idx]);out[c]=mixbuf(m,m->offset[mix]+e->specific[6+idx]);
    }
    if(!e->enabled){for(unsigned c=0;c<channels;c++)if(in[c]!=out[c])memmove(out[c],in[c],(size_t)m->frame*sizeof(float));return;}
    if(e->type==6){
        for(unsigned c=0;c<channels;c++)for(int k=0;k<m->frame;k++){
            float x=in[c][k],y=s->numerator[0]*x+s->biquad_z[c][0];
            s->biquad_z[c][0]=s->numerator[1]*x+s->denominator[0]*y+s->biquad_z[c][1];s->biquad_z[c][1]=s->numerator[2]*x+s->denominator[1]*y;out[c][k]=y;
        }
    } else {if(!s->dsp)rt_fatal("audio DSP effect state missing");audio_effect_process(s->dsp,in,out,m->frame);}
}
static void mix_effects(AudioRenderer *r, int mix) {
    Mixer *m=r->mixer; int count=0;
    for(int i=0;i<r->param.effect_count;i++) if(r->effects[i].type && r->effects[i].mix_id==mix) {
        int j=count++; while(j>0 && r->effects[m->effect_order[j-1]].order>r->effects[i].order) { m->effect_order[j]=m->effect_order[j-1]; j--; } m->effect_order[j]=i;
    }
    for(int n=0;n<count;n++) {
        int id=m->effect_order[n]; const AudrenEffectIn *e=&r->effects[id]; EffectState *s=&m->effect[id];
        if(e->type==1 || e->type==3 || e->type==4 || e->type==6 || e->type==9){mix_simple_effect(r,mix,id);continue;}
        if(e->type==2) { mix_aux(r,mix,e); continue; }
        if(e->type==8) { mix_capture(r,mix,e);continue; }
        if(e->type==7) { mix_limiter(r,mix,id); continue; }
        if(e->type!=5) {
            if(e->enabled && e->type<32 && !(m->effect_reported&(1u<<e->type))) { m->effect_reported|=1u<<e->type; rt_log("audio: effect type %u not implemented",e->type); }
            continue;
        }
        Reverb3dParam *p=&s->param; float *input[6]={0},*output[6]={0};
        if(p->channels!=1 && p->channels!=2 && p->channels!=4 && p->channels!=6) { if(e->enabled)rt_fatal("audio reverb channel count"); continue; }
        for(int c=0;c<p->channels;c++) {
            int idx=c; /* REV11+ channel resources use L,R,C,LFE,BL,BR; the reference DSP uses legacy order. */
            if(p->channels==6 && (r->revision>>24)>='0'+11) { static const int map[6]={0,1,4,5,2,3}; idx=map[c]; }
            if(p->input[idx]>=r->mixes[mix].buffer_count || p->output[idx]>=r->mixes[mix].buffer_count)rt_fatal("audio reverb buffer index");
            input[c]=mixbuf(m,m->offset[mix]+p->input[idx]); output[c]=mixbuf(m,m->offset[mix]+p->output[idx]);
        }
        if(!e->enabled) { for(int c=0;c<p->channels;c++)if(input[c]!=output[c])memmove(output[c],input[c],m->frame*sizeof(float)); continue; }
        if(!s->reverb)s->reverb=audio_reverb_create(p); else if(s->pending)audio_reverb_update(s->reverb,p);
        s->pending=0; audio_reverb_process(s->reverb,input,output,m->frame);
    }
}

static const float *splitter_filter(SplitDest *d,const float *input,float *output,int frames){
    if(!d->biquads[0].enable&&!d->biquads[1].enable)return input;
    memcpy(output,input,(size_t)frames*sizeof *output);
    for(unsigned f=0;f<2;f++)if(d->biquads[f].enable){
        const AudrenBiquadFloat *b=&d->biquads[f];float *z=d->state[f];
        for(int k=0;k<frames;k++){
            float x=output[k],y=b->numerator[0]*x+z[0];
            z[0]=b->numerator[1]*x+b->denominator[0]*y+z[1];
            z[1]=b->numerator[2]*x+b->denominator[1]*y;output[k]=y;
        }
    }
    return output;
}
typedef struct {
    uint64_t address;uint32_t size,channels,samples,last_read;
    uint8_t format,reserved[3],input[6],padding[2];
} CircularSinkParam;
_Static_assert(__builtin_offsetof(CircularSinkParam,input)==28,"circular sink wire layout");
static void mix_circular_sink(AudioRenderer *r,int id) {
    Mixer *m=r->mixer;CircularSinkParam p;memcpy(&p,r->sinks[id].name,sizeof p);
    if(!p.address || !p.size)return;
    if(p.channels<1 || p.channels>6 || p.format!=FMT_PCM16 || p.size>256*1024*1024 || p.size%2)rt_fatal("circular audio sink parameters");
    if(m->sink_address[id]!=p.address || m->sink_size[id]!=p.size){m->sink_address[id]=p.address;m->sink_size[id]=p.size;m->sink_offset[id]=m->sink_old[id]=m->sink_written[id]=0;}
    uint8_t *data=ipc_guest(p.address,p.size);uint32_t at=m->sink_offset[id];
    prepare_ring_write(data,p.size,at,(uint64_t)p.channels*m->frame*2);
    float gain=r->mixes[0].in_use?r->mixes[0].volume:1;
    for(unsigned c=0;c<p.channels;c++){
        if(p.input[c]>=m->nbuf)rt_fatal("circular audio sink input index");
        float *in=mixbuf(m,p.input[c]);
        for(int k=0;k<m->frame;k++){
            float scaled=in[k]*gain*32768;int16_t value=!isfinite(scaled)?0:scaled>=32767?32767:scaled<=-32768?-32768:(int16_t)scaled;
            memcpy(data+at,&value,2);at=(at+2)%p.size;
        }
    }
    m->sink_written[id]=m->sink_old[id];m->sink_old[id]=m->sink_offset[id];m->sink_offset[id]=at;
}
uint32_t audio_mixer_sink_written(AudioRenderer *r,int id) {Mixer *m=r->mixer;return m?m->sink_written[id]:0;}

void audio_mixer_render(AudioRenderer *r, double seconds, int drop) {
    (void)seconds;
    Mixer *m = r->mixer; if (!m) return;
    const int frames = m->frame, mc = r->param.submix_count + 1;
    memset(m->buf, 0, (size_t)m->nbuf * (size_t)frames * sizeof(float));
    int at = 0;
    for (int i = 0; i < mc; i++) { m->offset[i] = at; if (r->mixes[i].in_use) at += r->mixes[i].buffer_count; }
    if (at > m->nbuf) { rt_log("audio: mixes use %d buffers, renderer has %d", at, m->nbuf); rt_fatal("audio mix buffer layout"); }

    float (*vs)[MAX_FRAME]=m->voice_scratch,*filtered=m->filter_scratch;
    m->trace_pass++;
    // A pass ramps from the pass before it. The first one after the mixer opened or the renderer started has none.
    const int ramp = m->ramp && !m->snap;
    const uint64_t ramps_before = m->ramps;
    m->snap = 0;
    for (int i = 0; i < r->param.voice_count; i++) {
        ServerVoice *v = &r->voices[i];
        if (!v->in.in_use || v->state != SV_STARTED || !v->in.sample_rate || !v->valid[v->index]) continue;
        int cc = (int)v->in.channel_count; if (cc < 1 || cc > 6) { report_once(m, 2, "voice channel count unsupported", cc); continue; }
        {   /* each sample format a started voice uses, logged once: one the mixer cannot decode plays as silence */
            static volatile uint32_t seen;
            unsigned f = v->in.sample_format & 31;
            if (!(__atomic_fetch_or(&seen, 1u << f, __ATOMIC_RELAXED) & (1u << f)))
                rt_log("audio: voice %d uses sample format %u (%s), %d channel(s) at %u Hz", i, v->in.sample_format,
                       v->in.sample_format == FMT_ADPCM || fmt_bytes(v->in.sample_format) ? "decoded" : "NOT decoded: silent",
                       cc, (unsigned)v->in.sample_rate);
        }
        if ((rt_trace & RT_TRACE_AUDIO) && i < 1024) {   // mixed now and not in the last pass: what it plays and where to
            const AudrenWaveBuffer *wb = &v->buffers[v->index & 3];
            if (m->trace_seen[i] + 1 != m->trace_pass)   // pass: 5 ms each at 48 kHz (frame samples per pass)
                rt_log("audio: voice %d plays at pass %llu: format %u, %d channel(s) at %u Hz, buffer %u at %#llx+%#llx "
                       "(%s%s), volume %.3f, pitch %.3f, mix %d, splitter %u, priority %d", i,
                       (unsigned long long)m->trace_pass, (unsigned)v->in.sample_format, cc, (unsigned)v->in.sample_rate,
                       v->index, (unsigned long long)wb->address, (unsigned long long)wb->size,
                       wb->loop ? "loop" : "once", wb->end_of_stream ? ", end of stream" : "", v->in.volume, v->in.pitch,
                       (int)v->in.mix_id, (unsigned)v->in.splitter_id, (int)v->in.priority);
            m->trace_seen[i] = m->trace_pass;
            m->trace_buf[i] = wb->address;
        }
        voice_render(r, v, vs, frames);
        for (int f = 0; f < 2; f++) if (v->in.biquads[f].enable) {
            const AudrenBiquad *b = &v->in.biquads[f];
            const AudrenBiquadFloat *bf=&v->float_biquads[f];
            const float a0 = v->use_float_biquads?bf->numerator[0]:b->numerator[0]/16384.0f;
            const float a1 = v->use_float_biquads?bf->numerator[1]:b->numerator[1]/16384.0f;
            const float a2 = v->use_float_biquads?bf->numerator[2]:b->numerator[2]/16384.0f;
            const float b1 = v->use_float_biquads?bf->denominator[0]:b->denominator[0]/16384.0f;
            const float b2 = v->use_float_biquads?bf->denominator[1]:b->denominator[1]/16384.0f;
            for (int c = 0; c < cc; c++) {
                float *z = v->biquad_state[f][c];
                for (int k = 0; k < frames; k++) {
                    float x = vs[c][k], y = a0 * x + z[0];
                    z[0] = a1 * x + b1 * y + z[1];
                    z[1] = a2 * x + b2 * y;
                    vs[c][k] = y;
                }
            }
        }
        // The voice ramps when the pass before mixed it, with as many channels and into the same mix or splitter. One
        // that has just started (voice_reset in audren.c; or stopped, paused or out of wave buffers in that pass) and
        // one the game moved to another destination start at their new volumes.
        const int vramp = ramp && v->ramp_pass == r->elapsed_frames && v->ramp_channels == (uint32_t)cc &&
                          v->ramp_mix == v->in.mix_id && v->ramp_splitter == v->in.splitter_id;
        int routed = 0;
        if (v->in.splitter_id != SPLITTER_UNUSED && v->in.splitter_id < (uint32_t)m->nsplit) {
            const SplitInfo *s = &m->split[v->in.splitter_id];
            for (int c = 0; c < cc; c++) for (int k = c; k < s->ndest; k += cc) {
                SplitDest *d = &m->dest[s->ids[k]];
                if (!d->used || !mix_valid(r, d->dest)) continue;
                const float *source=splitter_filter(d,vs[c],filtered,frames);
                // from the destination's volumes and the voice's volume of the pass before, whose product is the gain
                // that pass mixed with (the destination's are kept once per pass: other voices may feed it too)
                const int from_prev = vramp && dest_ramps(d);
                for (int b = 0; b < r->mixes[d->dest].buffer_count && b < 24; b++) {
                    const float to = d->vol[b] * v->in.volume;
                    accumulate_ramp(m, m->offset[d->dest] + b, source, from_prev && b < d->prev_buffers ? d->prev[b] * v->ramp_volume : to, to, frames);
                }
            }
            routed = 1;
        } else if (mix_valid(r, v->in.mix_id)) {
            const int nb = r->mixes[v->in.mix_id].buffer_count < 24 ? r->mixes[v->in.mix_id].buffer_count : 24;
            for (int c = 0; c < cc; c++) {
                int32_t id = v->in.channel_ids[c];
                if (id < 0 || id >= r->param.voice_count * 6) {   // nothing goes in: no gain for the next pass to ramp from
                    for (int b = 0; b < nb; b++) v->ramp_gain[c][b] = NAN;
                    continue;
                }
                const AudrenChannelIn *ch = &r->channels[id];
                for (int b = 0; b < nb; b++) {
                    const float to = ch->mix[b] * v->in.volume;
                    accumulate_ramp(m, m->offset[v->in.mix_id] + b, vs[c], vramp && b < v->ramp_buffers ? v->ramp_gain[c][b] : to, to, frames);
                    v->ramp_gain[c][b] = to;
                }
            }
            v->ramp_buffers = nb;
            routed = 1;
        }
        v->ramp_pass = routed ? r->elapsed_frames + 1 : 0;   // audren.c render_thread counts the pass after this render
        v->ramp_channels = (uint32_t)cc;
        v->ramp_mix = v->in.mix_id;
        v->ramp_splitter = v->in.splitter_id;
        v->ramp_volume = v->in.volume;
    }
    if (rt_trace & RT_TRACE_AUDIO) {
        // Voices mixed in the last pass and not in this one, and why; then voices that read the same wave buffer memory
        // in this pass (one stream heard twice, offset by how far apart the two read).
        for (int i = 0; i < r->param.voice_count && i < 1024; i++) {
            if (!m->trace_seen[i] || m->trace_seen[i] + 1 != m->trace_pass) continue;   // 0: never mixed
            const ServerVoice *v = &r->voices[i];
            rt_log("audio: voice %d stops at pass %llu: %s; %llu samples played, %u buffers consumed", i,
                   (unsigned long long)m->trace_pass,
                   !v->in.in_use ? "released by the game" : v->state == SV_PAUSED ? "paused"
                   : v->state != SV_STARTED ? "stopped by the game" : "out of wave buffers",
                   (unsigned long long)v->played, (unsigned)v->consumed);
        }
        for (int i = 0; i < r->param.voice_count && i < 1024; i++) {
            if (m->trace_seen[i] != m->trace_pass || !m->trace_buf[i]) continue;
            for (int j = i + 1; j < r->param.voice_count && j < 1024 && m->trace_shared < 400; j++)
                if (m->trace_seen[j] == m->trace_pass && m->trace_buf[j] == m->trace_buf[i] &&
                    m->trace_pair[i] != ((uint64_t)j << 48 ^ m->trace_buf[i])) {   // once per pair and buffer
                    m->trace_pair[i] = (uint64_t)j << 48 ^ m->trace_buf[i];
                    m->trace_shared++;
                    rt_log("audio: voices %d and %d both play the wave buffer at %#llx in pass %llu (offsets %llu and %llu)",
                           i, j, (unsigned long long)m->trace_buf[i], (unsigned long long)m->trace_pass,
                           (unsigned long long)r->voices[i].offset, (unsigned long long)r->voices[j].offset);
                }
        }
    }
    /* submixes: deepest first, so every mix is complete before it is folded into its destination */
    int order[256], n = 0, depth[256];
    for (int i = 1; i < mc && n < 256; i++) if (r->mixes[i].in_use) { depth[n] = mix_depth(r, i); order[n++] = i; }
    for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++) if (depth[b] > depth[a]) { int t = order[a]; order[a] = order[b]; order[b] = t; t = depth[a]; depth[a] = depth[b]; depth[b] = t; }
    for (int a = 0; a < n; a++) {
        mix_effects(r, order[a]);
        const AudrenMixIn *x = &r->mixes[order[a]]; int src = m->offset[order[a]];
        if (x->dest_splitter_id != SPLITTER_UNUSED && x->dest_splitter_id < (uint32_t)m->nsplit) {
            const SplitInfo *s = &m->split[x->dest_splitter_id];
            // a mix into a splitter ramps like a voice into one: from the destination's volumes and the mix's volume
            // of the pass before, when that pass had the mix with as many buffers on the same splitter
            const MixPrev *xp = &m->mix_prev[order[a]];
            const int xramp = ramp && xp->ok && xp->splitter == x->dest_splitter_id && xp->buffers == x->buffer_count;
            for (int c = 0; c < x->buffer_count && c < 24; c++) for (int k = c; k < s->ndest; k += x->buffer_count) {
                SplitDest *d = &m->dest[s->ids[k]];
                if (!d->used || !mix_valid(r, d->dest)) continue;
                const float *source=splitter_filter(d,mixbuf(m,src+c),filtered,frames);
                const int from_prev = xramp && dest_ramps(d);
                for (int b = 0; b < r->mixes[d->dest].buffer_count && b < 24; b++) {
                    const float to = d->vol[b] * x->volume;
                    accumulate_ramp(m, m->offset[d->dest] + b, source, from_prev && b < d->prev_buffers ? d->prev[b] * xp->volume : to, to, frames);
                }
            }
        } else if (mix_valid(r, x->dest_mix_id)) {
            for (int c = 0; c < x->buffer_count && c < 24; c++)
                for (int b = 0; b < r->mixes[x->dest_mix_id].buffer_count && b < 24; b++) accumulate(m, m->offset[x->dest_mix_id] + b, mixbuf(m, src + c), x->mix[c][b] * x->volume, frames);
        }
    }
    // What every splitter destination and every mix went in with, for the next pass's ramps: kept here, once per pass,
    // after every voice and mix that feeds a destination has read the values of the pass before.
    for (int i = 0; i < m->ndest; i++) {
        SplitDest *d = &m->dest[i];
        d->prev_ok = d->used && mix_valid(r, d->dest);
        if (!d->prev_ok) continue;
        memcpy(d->prev, d->vol, sizeof d->prev);
        d->prev_dest = d->dest;
        d->prev_buffers = r->mixes[d->dest].buffer_count;
    }
    for (int i = 1; i < mc; i++)
        m->mix_prev[i] = (MixPrev){r->mixes[i].volume, r->mixes[i].dest_splitter_id, r->mixes[i].buffer_count, r->mixes[i].in_use != 0};
    if (m->ramps != ramps_before) m->ramp_passes++;
    if (r->mixes[0].in_use) mix_effects(r, 0);
    /* device sink */
    const AudrenSinkIn *sink = NULL;int circular=0;
    for (int i = 0; i < r->param.sink_count; i++) if (r->sinks[i].in_use) {
        if(r->sinks[i].type!=SINK_CIRCULAR){m->sink_address[i]=0;m->sink_size[i]=m->sink_offset[i]=m->sink_old[i]=m->sink_written[i]=0;}
        if (r->sinks[i].type == SINK_DEVICE && !sink) sink = &r->sinks[i];
        else if (r->sinks[i].type == SINK_CIRCULAR){mix_circular_sink(r,i);circular=1;}
    } else {m->sink_address[i]=0;m->sink_size[i]=m->sink_offset[i]=m->sink_old[i]=m->sink_written[i]=0;}
    if (m->channels != output_channels()) channels_switch(m, output_channels());
    // A catch-up frame (audren.c render_thread) ends here. Everything above ran, so the voices' filters and ramps, the
    // effects and the sinks the game reads back are where they would be had the frame been heard; the device gets
    // nothing of it.
    if (drop) return;
    const int oc = m->channels;
    float *out=m->output_scratch;
    memset(out, 0, sizeof(float) * (size_t)oc * (size_t)frames);
    if (sink && sink->input_count >= 1 && sink->input_count <= 6) {
        if (!(m->reported & (1u << 6))) {
            m->reported |= 1u << 6;
            rt_log("audio: device sink has %u inputs, output %s", sink->input_count, oc == 6 ? "5.1" : "stereo");
            rt_log("audio: device sink inputs %d %d %d %d %d %d, downmix %s %.3f %.3f %.3f %.3f", sink->inputs[0], sink->inputs[1],
                   sink->inputs[2], sink->inputs[3], sink->inputs[4], sink->inputs[5], sink->downmix_enabled ? "on" : "off",
                   sink->downmix[0], sink->downmix[1], sink->downmix[2], sink->downmix[3]);
        }
        const float gain = host_volume() * atomic_load(&device_volume) * (r->mixes[0].in_use ? r->mixes[0].volume : 1.0f);
        const float *ch[6] = {0};
        float raw[2 * MAX_FRAME];
        for (uint32_t c = 0; c < sink->input_count; c++) if (sink->inputs[c] >= 0 && sink->inputs[c] < m->nbuf) ch[c] = mixbuf(m, sink->inputs[c]);
        if (rt_trace & RT_TRACE_AUDIO)   // what the game puts on each input: shows whether it mixes for 5.1 (level_note)
            for (int c = 0; c < 6; c++)
                for (int k = 0; ch[c] && k < frames; k++) m->input_sum[c] += (double)ch[c][k] * ch[c][k];
        for (int k = 0; k < frames; k++) {
            float s[OUT_MAX] = {0};   /* FL FR C LFE RL RR */
            if (sink->input_count >= 6 && oc == 6) {
                for (int c = 0; c < 6; c++) s[c] = ch[c] ? ch[c][k] : 0;
            } else if (sink->input_count >= 6) {   /* 5.1 -> stereo; the LFE is left out, as the Switch's downmix does */
                float ff = sink->downmix_enabled ? sink->downmix[0] : 1.0f, cf = sink->downmix_enabled ? sink->downmix[1] : 0.7071f, sf = sink->downmix_enabled ? sink->downmix[2] : 0.7071f;
                float C = ch[2] ? ch[2][k] : 0;
                s[0] = ff * (ch[0] ? ch[0][k] : 0) + cf * C + sf * (ch[4] ? ch[4][k] : 0);
                s[1] = ff * (ch[1] ? ch[1][k] : 0) + cf * C + sf * (ch[5] ? ch[5][k] : 0);
            } else if (sink->input_count >= 2) { s[0] = ch[0] ? ch[0][k] : 0; s[1] = ch[1] ? ch[1][k] : 0; }
            else s[0] = s[1] = ch[0] ? ch[0][k] : 0;
            raw[2 * k] = s[0] * gain; raw[2 * k + 1] = s[1] * gain;   /* NX2NAVI_AUDIO_DUMP: the front pair before clipping */
            for (int c = 0; c < oc; c++) {
                float x = s[c] * gain;
                // NaN (a NaN sample, an effect's state gone NaN) reaches the device as silence, not full scale
                out[oc * k + c] = x > 1 ? 1 : x < -1 ? -1 : x == x ? x : 0;
            }
        }
        sink_dump(raw, frames);
    } else if (!sink && !circular) report_once(m, 5, "no device sink in use", r->param.sink_count);
    level_note(r, m, out, frames);
    output_push(m, out, frames);
}

// ---- IAudioDevice: one output device, stereo or 5.1 (output_channels)
static const char device_name[] = "AudioTvOutput";

// The events the game took to hear of a change of the output device (IAudioDevice's, its notifications' and the
// virtual device manager's). audio_channels_live signals them all: the game then asks for the channel count again, as
// it does on a console whose output moves from the TV to headphones.
static uint32_t change_events[16];
static _Atomic int n_change_events;

static uint32_t change_event_new(void) {
    uint32_t ev = kern_event_new();
    int i = atomic_fetch_add(&n_change_events, 1);
    if (i < 16) change_events[i] = ev;
    return ev;
}

// Present path, after a settings change (gfx_present.c window_settings_live): a changed NX2NAVI_AUDIO_CHANNELS is
// decided again, and when it gives another channel count the game is told its output device changed. The game asks
// GetActiveChannelCount again and mixes for the new count; the mixer moves its device to that count at its next
// render (channels_switch). Measured on Ocarina 1.1.1: one event held, and the game asked within the same second.
void audio_channels_live(void) {
    static char seen[16];
    const char *want = cfg_value("NX2NAVI_AUDIO_CHANNELS");
    if (!strncmp(want, seen, sizeof seen - 1)) return;
    int first = !seen[0];
    snprintf(seen, sizeof seen, "%s", want);
    int was = atomic_load(&decided);
    if (first || !was) return;   // nothing has asked yet: the first call decides
    atomic_store(&decided, 0);
    int now = output_channels();
    if (now == was) return;
    int n = atomic_load(&n_change_events);
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) kern_event_signal(change_events[i]);
    rt_log("audio: NX2NAVI_AUDIO_CHANNELS=%s: the output goes from %s to %s; the game is told the device changed "
           "(%d events)", want, was == 6 ? "5.1" : "stereo", now == 6 ? "5.1" : "stereo", n);
}

static void put_name(const IpcBuffer *b, IpcResponse *rs, int pointer) {
    if (!b->size) return;
    char *dst = ipc_guest(b->addr, b->size);
    size_t n = b->size < sizeof device_name ? b->size : sizeof device_name;
    memset(dst, 0, b->size);
    memcpy(dst, device_name, n);
    if (pointer) rs->xout[rs->nxout++] = (IpcBuffer){b->addr, b->size, b->index};
}

static IpcBuffer out_buffer(const IpcRequest *rq, int *pointer) {
    *pointer = !(rq->nb && rq->b[0].size) && rq->nc && rq->c[0].size;
    return *pointer ? rq->c[0] : rq->nb ? rq->b[0] : (IpcBuffer){0};
}

void audio_device_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    static uint32_t change_event;   // the device change event: signalled when the output's channels change (audio_channels_live)
    int pointer;
    switch (rq->cmd) {
    case 0: case 6: case 14: {   // ListAudioDeviceName(Auto), ListAudioOutputDeviceName [13.0.0+]
        IpcBuffer b = out_buffer(rq, &pointer);
        int32_t n = b.size >= 0x100 ? 1 : 0;
        if (n) put_name(&b, rs, pointer);
        IPC_OUT(rs, n);
        return;
    }
    case 1: case 7: {   // SetAudioDeviceOutputVolume(Auto)
        float v = IPC_ARG(rq, float, 0);
        atomic_store(&device_volume, isfinite(v) ? v : 0.0f);
        return;
    }
    case 2: case 8: { float v = atomic_load(&device_volume); IPC_OUT(rs, v); return; }   // GetAudioDeviceOutputVolume(Auto)
    case 3: case 10: case 13: {   // GetActiveAudioDeviceName(Auto), GetActiveAudioOutputDeviceName [13.0.0+]
        IpcBuffer b = out_buffer(rq, &pointer);
        put_name(&b, rs, pointer);
        return;
    }
    case 5: { int32_t n = output_channels(); IPC_OUT(rs, n); return; }   // GetActiveChannelCount
    case 4: case 11: case 12: {   // QueryAudioDeviceSystemEvent, ...InputEvent, ...OutputEvent
        uint32_t ev = __atomic_load_n(&change_event, __ATOMIC_ACQUIRE);
        if (!ev) {
            uint32_t mine = change_event_new(), expected = 0;
            if (__atomic_compare_exchange_n(&change_event, &expected, mine, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                ev = mine;
            else
                ev = expected;   // the loser's event stays unused (no host-side close yet)
        }
        rs->copy[rs->ncopy++] = kern_event_readable(ev);
        return;
    }
    // 17.0.0+: Acquire/Release Audio{Input,Output}DeviceNotification (15-18), 18.0.0+: SetAudioDeviceOutputVolume-
    // AutoTuneEnabled (19) / IsAudioDeviceOutputVolumeAutoTuneEnabled (20), 21.0.0+:
    // IsActiveOutputDeviceEstimatedLowLatency (21). Command ids as the system answers them. The
    // host applies no auto-tuned volume, so the answers are "off" / "no".
    case 15: case 17: {   // Acquire...Notification -> a readable event, signalled with the others
        uint32_t ev = change_event_new();
        rs->copy[rs->ncopy++] = kern_event_readable(ev);
        return;
    }
    case 16: case 18: case 19: return;
    case 20: case 21: { uint8_t no = 0; IPC_OUT(rs, no); return; }
    default: ipc_unimplemented(s, rq);
    }
}

// ---- auddmg:u: the Switch 2 audio device manager. Command 0 opens the virtual device manager. Its output-side
// commands and the 0x228-byte property block follow the system service's layout
// (INx2AudioVirtualDeviceManager); ids there are partly guessed. One virtual output device feeds the host mixer;
// there are no input devices, so a command not listed here (the input side the SDK's ListVirtualInputDevices
// reaches) is logged once and answered with success and a zero count.
static const char virtual_output[] = "AudioVirtualOutput0";
#define AUD_R_NOT_SUPPORTED 0x40299u      // audio module 153, description 513
#define AUD_R_INVALID_ARGUMENT 0x70899u   // audio module 153, description 900

static void put_block(const IpcRequest *rq, IpcResponse *rs, const void *src, size_t n) {
    int pointer;
    IpcBuffer b = out_buffer(rq, &pointer);
    if (!b.size) return;
    uint8_t *dst = ipc_guest(b.addr, b.size);
    memset(dst, 0, b.size);
    memcpy(dst, src, n < b.size ? n : b.size);
    if (pointer) rs->xout[rs->nxout++] = (IpcBuffer){b.addr, b.size, b.index};
}

static void virtual_device_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    switch (rq->cmd) {
    case 0: {   // ListVirtualOutputDevices -> count, names (0x100 each)
        char name[0x100] = {0};
        memcpy(name, virtual_output, sizeof virtual_output);
        int pointer;
        IpcBuffer b = out_buffer(rq, &pointer);
        uint32_t n = b.size >= sizeof name ? 1 : 0;
        if (n) put_block(rq, rs, name, sizeof name);
        IPC_OUT(rs, n);
        return;
    }
    case 100: {   // GetDefaultVirtualOutputDevice -> name
        char name[0x100] = {0};
        memcpy(name, virtual_output, sizeof virtual_output);
        put_block(rq, rs, name, sizeof name);
        return;
    }
    case 300: {   // IsVirtualOutputDeviceStreamFormatSupported(sample type, channels, rate, functionality)
        uint32_t channels = IPC_ARG(rq, uint32_t, 4), rate = IPC_ARG(rq, uint32_t, 8), fn = IPC_ARG(rq, uint32_t, 12);
        rs->result = fn == 1 && (channels == 1 || channels == 2 || channels == 6) && rate == 48000 ? 0 : AUD_R_NOT_SUPPORTED;
        return;
    }
    case 401: {   // GetVirtualOutputDeviceProperties(name) -> block
        uint8_t block[0x228] = {0};
        memcpy(block, virtual_output, sizeof virtual_output);
        // type, (2), channels, sample rate. The channels are this run's output (output_channels), as IAudioDevice's
        // GetActiveChannelCount reports: a Switch 2 game (Ocarina) sizes its mix from this block, so a constant 2
        // here kept 5.1 at stereo
        uint32_t v[4] = {3, 2, (uint32_t)output_channels(), 48000};
        memcpy(block + 0x100, v, sizeof v);
        memcpy(block + 0x110, device_name, sizeof device_name);
        uint64_t latency_ns = 10000000;
        memcpy(block + 0x218, &latency_ns, sizeof latency_ns);
        uint32_t id = 1;   // nonzero: the SDK's output streams pass it on (a zero breaks stream creation)
        memcpy(block + 0x220, &id, sizeof id);
        put_block(rq, rs, block, sizeof block);
        return;
    }
    case 500: rs->copy[rs->ncopy++] = kern_event_readable(change_event_new()); return;   // state change event
    case 700: case 800: return;   // AttachStream / DetachStream
    default: {
        static volatile int32_t seen[64];
        if (rq->cmd >= 64 || !__atomic_exchange_n(&seen[rq->cmd], 1, __ATOMIC_RELAXED))
            rt_log("auddmg:u manager: command %u answered with success and a zero count (no input devices)", rq->cmd);
        uint32_t zero = 0;
        IPC_OUT(rs, zero);
        return;
    }
    }
}

void audio_dmg_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    if (rq->cmd == 0) {   // OpenVirtualDeviceManager
        rs->move[rs->nmove++] = ipc_session_new("auddmg:u manager", virtual_device_manager_handler, NULL, NULL);
        return;
    }
    ipc_unimplemented(s, rq);
}

// ---- audsmx:u: the Switch 2 sound mixer (sysmix). Command 0 opens a user session; the session's CreateStream (1)
// makes a stream of a virtual device (the sysmix user session: device 1 is the virtual output). This host
// renders through audren, has no sysmix output streams yet and no input devices: CreateStream of a device other
// than 1 fails with InvalidArgument (as the system does), and device 1 stops with its name so the first title that needs
// a sysmix output stream says so. Engage/DisengageStreams (2/3) succeed.
static void sysmix_session_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    switch (rq->cmd) {
    case 1: {
        uint32_t device = IPC_ARG(rq, uint32_t, 0), flags = IPC_ARG(rq, uint32_t, 4);
        if (device == 1) rt_fatal("audsmx:u: CreateStream on the virtual output (flags %#x) is not implemented", flags);
        rt_log("audsmx:u: CreateStream device %u flags %#x refused: no audio input devices", device, flags);
        rs->result = AUD_R_INVALID_ARGUMENT;
        return;
    }
    case 2: case 3: return;
    default: ipc_unimplemented(s, rq);
    }
}

void audio_sysmix_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    if (rq->cmd == 0) {   // OpenUserSession(process handle)
        rs->move[rs->nmove++] = ipc_session_new("audsmx:u session", sysmix_session_handler, NULL, NULL);
        return;
    }
    ipc_unimplemented(s, rq);
}
