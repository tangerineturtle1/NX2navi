// Contains code Copyright (c) 2026 MacNativeGames contributors, Copyright (c) Ryujinx Team and Contributors (MIT; see
// THIRD_PARTY_NOTICES.md).
// audren:u. The SDK's nn::audio client builds RequestUpdate input unchanged; this
// service validates every section against the header, keeps server-side voice state, advances it in real time (one
// frame of sample_count / sample_rate seconds per render tick, signalling the renderer's event) and writes the output
// status the client expects. The mixer behind it is audio_mixer.c.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audren.h"
#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "ipc.h"
#include "platform/platform.h"

static volatile int32_t g_live;

extern GfxStats gfx_counters;   // frames: the game frame a stats line names, to match the frametime lines

// ---- NX2NAVI_AUDIO_STATS (default 0; 1 on): where a crackle comes from
// A gap in the sound has one of two sources here. The output device took a period and the ring held less (an underrun:
// silence until the prefill is queued again), because the render thread came late: a long gap between two of its
// passes, or a long wait for the renderer lock, which the game's audio thread holds while its RequestUpdate is read.
// Or the game's audio thread came late: on the console it sends RequestUpdate after each render pass and keeps every
// streamed voice's queue filled; a voice whose queue runs out before its end of stream ("dry") plays silence until its
// next buffer arrives ("late", counted when that comes within AS_LATE_PASSES passes, by how many passes it missed).
// One line per AS_WINDOW_NS in which something went wrong (and once a minute regardless), plus a line for each of the
// first AS_EVENTS underruns and late game updates, with the game frame (the frame numbers of the frametime lines).
#define AS_WINDOW_NS 10000000000ull
#define AS_EVENTS 48
#define AS_LATE_PASSES 40            // 200 ms: a buffer later than that is taken for a new sound, not a late one
#define AS_GAP_NS 15000000ull        // a render pass more than this after the one before (a 10 ms device period + 5)
#define AS_LOCK_NS 2000000ull        // a wait for the renderer lock longer than this
#define AS_UPDATE_NS 20000000ull     // a game update more than this after the one before (four passes)
#define AS_UPDATE_EVENT_NS 30000000ull
typedef struct AudrenStats {
    uint64_t window_ns, windows;
    // render thread (under the lock): passes, gaps between them, lock waits, the ring's level when a pass began
    uint64_t passes, last_pass_ns, gap_max_ns, gaps_over, lock_max_ns, locks_over, queue_min, pull_gap_max_ns;
    uint64_t out_underruns, out_missing, out_skipped, out_overruns;   // the output's counters at the window's start
    uint64_t seen_underruns;                                          // the last count an event line was written for
    // the game's RequestUpdate (under the lock)
    uint64_t upd_last_ns, upd_n, upd_gap_max_ns, upd_gap_frame, upd_gaps_over, upd_pass_last, upd_passes_max,
        upd_hold_max_ns;
    // voices (under the lock)
    uint64_t dry, late[3], late_max;   // next buffer late by 1-2 passes, 3-8, 9-AS_LATE_PASSES
    uint64_t ramp_passes, ramps;       // the mixer's volume ramp counts at the window's start (audio_mixer_ramp_stats)
    // catch-up (render_thread, under the lock): frames rendered and dropped; gaps over AUDIO_CATCHUP_MAX_MS, which
    // are left as they are
    uint64_t caught_up, gave_up;
    int events;
    uint64_t voices_ns;                // the game's update (under the lock): the last voice census line
    int voices_most;                   // the most voices in use a census found
} AudrenStats;

// NX2NAVI_AUDIO_GAME_PRIO (default 0): the host priority of the game's thread that sends RequestUpdate. 0 leaves it
// alone. 1 raises it to the highest while the update is read, so the render thread (highest priority) never waits for
// the renderer lock behind a game thread the host has preempted for busier threads. 2 also keeps that thread above
// normal from its first update on, so it is not queued behind the shader and pipeline workers when it wakes for the
// next render pass.
static int g_game_prio;

static uint64_t game_frame(void) { return __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED); }

long audren_renderers_live(void) { return plat_cas32(&g_live, 0, 0); }

static int revision_number(uint32_t rev) { return (int)(rev >> 24) - '0'; }

static void need(int ok, const char *what) {
    if (ok) return;
    ipc_backtrace();
    rt_fatal("audren: %s", what);
}

static AudioRenderer *renderer_new(const AudrenParameter *p) {
    need(p->sample_rate == 48000 || p->sample_rate == 32000, "unsupported output rate");
    need(p->sample_count > 0 && p->sample_count <= 4800, "sample count");
    need(p->voice_count > 0 && p->voice_count <= 1024 && p->sink_count > 0 && p->sink_count <= 256 &&
             p->effect_count >= 0 && p->effect_count <= 4096 && p->submix_count >= 0 && p->submix_count < 256,
         "renderer counts");
    AudioRenderer *r = calloc(1, sizeof *r);
    need(r != NULL, "renderer allocation");
    r->param = *p;
    r->revision = p->revision;
    r->voices = calloc((size_t)p->voice_count, sizeof *r->voices);
    r->pools = calloc((size_t)p->effect_count + 4u * (size_t)p->voice_count, sizeof *r->pools);
    r->mixes = calloc((size_t)p->submix_count + 1, sizeof *r->mixes);
    r->sinks = calloc((size_t)p->sink_count, sizeof *r->sinks);
    r->effects = calloc((size_t)(p->effect_count ? p->effect_count : 1), sizeof *r->effects);
    r->channels = calloc((size_t)p->voice_count * 6, sizeof *r->channels);
    need(r->voices && r->pools && r->mixes && r->sinks && r->effects && r->channels, "renderer tables");
    plat_mutex_init(&r->lock);
    r->event = kern_event_new();
    r->state = 1;   // stopped
    return r;
}

static void renderer_free(AudioRenderer *r) {
    free(r->stats);
    free(r->voices);
    free(r->pools);
    free(r->mixes);
    free(r->sinks);
    free(r->effects);
    free(r->channels);
    free(r);
}

// ---- RequestUpdate
typedef struct { const uint8_t *p, *end; } Reader;

static const uint8_t *take(Reader *in, uint32_t n, const char *what) {
    if ((size_t)(in->end - in->p) < n) {
        rt_log("audren: input section %s (%u bytes) overruns the request", what, n);
        need(0, "update input section size");
    }
    const uint8_t *p = in->p;
    in->p += n;
    return p;
}

static void voice_reset(ServerVoice *v) {
    memset(v->valid, 0, sizeof v->valid);
    memset(v->pending, 0, sizeof v->pending);
    v->state = SV_STOPPED;
    v->flush = v->flush_from = 0;
    v->index = 0;
    v->offset = 0;
    v->fraction = 0;
    v->played = 0;
    v->consumed = 0;
    v->loops_done = 0;
    memset(&v->adpcm, 0, sizeof v->adpcm);
    memset(v->biquad_state, 0, sizeof v->biquad_state);
    v->dry_pass = 0;
    v->ramp_pass = 0;   // a new voice starts at its volumes: no ramp from what the slot's last voice was mixed with
}

static void update_voice(AudioRenderer *r, const AudrenVoiceIn *in) {
    need(in->id < (uint32_t)r->param.voice_count, "voice id out of range");
    ServerVoice *v = &r->voices[in->id];
    if (in->is_new || !in->in_use) voice_reset(v);
    v->in = *in;
    v->use_float_biquads = 0;
    if (!in->in_use) return;
    need(in->wave_buffer_index < 4, "wave buffer index");
    // The play state as the DSP keeps it: Stop on a voice that is not stopped
    // becomes Stopping, which the next render frame turns into Stopped after giving back every queued buffer.
    if (in->play_state == PLAY_STARTED) v->state = SV_STARTED;
    else if (in->play_state == PLAY_PAUSED) v->state = SV_PAUSED;
    else if (v->state != SV_STOPPED) v->state = SV_STOPPING;
    for (unsigned i = 0; i < 4; i++) {   // a new buffer waits until a render frame finds the voice playing
        const AudrenWaveBuffer *w = &in->wave_buffers[i];
        if (!w->sent) {
            v->buffers[i] = *w;
            v->pending[i] = 1;
        }
    }
    v->flush += in->flush_wave_buffer_count;
    v->flush_from = in->wave_buffer_index;
    if (in->clear_voice_drop) v->drops = 0;
}

// No samples (no memory, no size or an empty range): the DSP counts the buffer as played when it reaches it, so a
// stream's empty last buffer does not hold up the buffers queued behind it.
int audio_buffer_empty(const AudrenWaveBuffer *b) { return !b->address || !b->size || b->end <= b->start; }

static void skip_empty(AudioRenderer *r, ServerVoice *v) {
    while (v->valid[v->index] && audio_buffer_empty(&v->buffers[v->index])) {
        const AudrenWaveBuffer *b = &v->buffers[v->index];
        if (b->loop && b->loop_count < 0) break;   // an endless empty loop starves the voice, as on the DSP
        v->valid[v->index] = 0;
        v->consumed++;
        v->offset = 0;
        v->loops_done = 0;
        v->index = (v->index + 1) & 3;
        r->empty_played++;
        if (b->end_of_stream) {
            v->played = 0;
            break;
        }
    }
}

// Each render frame, before mixing: flush, stop, start.
static void voices_prepare(AudioRenderer *r) {
    for (int k = 0; k < r->param.voice_count; k++) {
        ServerVoice *v = &r->voices[k];
        if (!v->in.in_use) continue;
        if (v->flush) {   // FlushWaveBuffers: from the client's current buffer, the queued ones count as played
            unsigned i = v->flush_from & 3;
            for (uint32_t n = v->flush; n; n--, i = (i + 1) & 3) {
                v->pending[i] = 0;
                if (v->valid[i]) {
                    v->valid[i] = 0;
                    v->consumed++;
                    v->index = (v->index + 1) & 3;
                    v->offset = 0;
                    v->loops_done = 0;
                    r->flushed++;
                }
            }
            v->flush = 0;
        }
        if (v->state == SV_STOPPING) {   // every queued buffer counts as played; unsent ones are dropped
            for (int i = 0; i < 4; i++) {
                if (v->valid[i]) {
                    v->consumed++;
                    v->index = (v->index + 1) & 3;
                    r->stop_released++;
                }
                v->valid[i] = 0;
                v->pending[i] = 0;
            }
            v->offset = 0;
            v->played = 0;
            v->fraction = 0;
            v->loops_done = 0;
            v->state = SV_STOPPED;
            v->dry_pass = 0;
            continue;
        }
        if (v->state == SV_STARTED) {
            for (int i = 0; i < 4; i++)
                if (v->pending[i]) {
                    v->pending[i] = 0;
                    v->valid[i] = 1;
                    if ((unsigned)i == v->index) {
                        v->offset = 0;
                        v->loops_done = 0;
                    }
                    if (v->dry_pass) {   // NX2NAVI_AUDIO_STATS: the buffer a dry voice waited for
                        uint64_t missed = r->elapsed_frames + 1 - v->dry_pass;
                        if (r->stats && missed <= AS_LATE_PASSES) {
                            AudrenStats *st = r->stats;
                            st->late[missed <= 2 ? 0 : missed <= 8 ? 1 : 2]++;
                            if (missed > st->late_max) st->late_max = missed;
                        }
                        v->dry_pass = 0;
                    }
                }
            skip_empty(r, v);
        }
    }
}

static uint32_t effect_out_bytes(const AudioRenderer *r) { return revision_number(r->revision) >= 9 ? 0x90u : 0x10u; }

static void request_update(AudioRenderer *r, const uint8_t *input, uint64_t in_size, uint8_t *output,
                           uint64_t out_size, uint64_t perf_size) {
    need(in_size >= sizeof(AudrenHeader), "update input too small");
    AudrenHeader h;
    memcpy(&h, input, sizeof h);
    need(h.total <= in_size, "update total size");
    Reader in = {input + sizeof h, input + h.total};
    plat_mutex_lock(&r->lock);
    const uint64_t locked_ns = r->stats ? kern_now_ns() : 0;
    r->revision = h.revision;
    const int vc = r->param.voice_count, ec = r->param.effect_count, sc = r->param.sink_count,
              mc = r->param.submix_count + 1;
    const uint32_t npools = (uint32_t)ec + 4u * (uint32_t)vc;
    if (!r->logged) {
        r->logged = 1;
        rt_log("audren: update REV%d sizes behavior=%u pools=%u voices=%u channels=%u effects=%u mixes=%u sinks=%u "
               "perf=%u splitters=%u render=%u total=%u out=%llu perf_out=%llu",
               revision_number(h.revision), h.behavior, h.memory_pools, h.voices, h.voice_resources, h.effects,
               h.mixes, h.sinks, h.performance, h.splitters, h.render_info, h.total, (unsigned long long)out_size,
               (unsigned long long)perf_size);
    }
    need(h.behavior == 0x10, "behavior section size");
    const uint8_t *behavior = take(&in, h.behavior, "behavior");
    memcpy(&r->behavior_flags, behavior + 8, 8);
    need(h.memory_pools == npools * sizeof(AudrenPoolIn), "memory pool section size");
    const AudrenPoolIn *pools = (const AudrenPoolIn *)take(&in, h.memory_pools, "memory pools");
    need(h.voice_resources % sizeof(AudrenChannelIn) == 0 &&
             h.voice_resources / sizeof(AudrenChannelIn) <= (uint32_t)vc * 6,
         "voice channel section size");
    const AudrenChannelIn *chans = (const AudrenChannelIn *)take(&in, h.voice_resources, "voice channels");
    const int float_voices = revision_number(h.revision) >= 15;
    const size_t voice_stride = float_voices ? sizeof(AudrenVoiceInV2) : sizeof(AudrenVoiceIn);
    need(h.voices == (uint32_t)vc * voice_stride, "voice section size");
    const uint8_t *voices = take(&in, h.voices, "voices");
    need(h.effects == (uint32_t)ec * sizeof(AudrenEffectIn), "effect section size");
    const AudrenEffectIn *effects = (const AudrenEffectIn *)take(&in, h.effects, "effects");
    const uint8_t *splitters = take(&in, h.splitters, "splitters");
    const uint8_t *mixes = take(&in, h.mixes, "mixes");
    need(h.sinks == (uint32_t)sc * sizeof(AudrenSinkIn), "sink section size");
    const AudrenSinkIn *sinks = (const AudrenSinkIn *)take(&in, h.sinks, "sinks");
    take(&in, h.performance, "performance");
    take(&in, h.render_info, "render info");
    need(in.p == in.end, "unconsumed update input");

    for (uint32_t i = 0; i < h.voice_resources / sizeof(AudrenChannelIn); i++) {
        need(chans[i].id < (uint32_t)vc * 6, "channel resource id");
        r->channels[chans[i].id] = chans[i];
    }
    for (int i = 0; i < vc; i++) {
        AudrenVoiceIn v;
        AudrenVoiceInV2 v2;
        if (float_voices) {
            memcpy(&v2, voices + i * voice_stride, sizeof v2);
            memset(&v, 0, sizeof v);
            memcpy(&v, v2.prefix, sizeof v2.prefix);
            memcpy((uint8_t *)&v + 0x3c, v2.tail, sizeof v2.tail);
            for (unsigned f = 0; f < 2; f++) {
                v.biquads[f].enable = v2.biquads[f].enable;
                if (!v.biquads[f].enable) continue;
                for (unsigned c = 0; c < 3; c++) need(isfinite(v2.biquads[f].numerator[c]), "voice biquad numerator");
                for (unsigned c = 0; c < 2; c++)
                    need(isfinite(v2.biquads[f].denominator[c]), "voice biquad denominator");
            }
        } else
            memcpy(&v, voices + i * voice_stride, sizeof v);
        // An unused slot can have a zero id. Release its own voice and leave voice zero playing.
        if (!v.in_use) v.id = (uint32_t)i;
        update_voice(r, &v);
        ServerVoice *server = &r->voices[v.id];
        server->use_float_biquads = (uint8_t)float_voices;
        if (float_voices) memcpy(server->float_biquads, v2.biquads, sizeof v2.biquads);
    }
    for (int i = 0; i < ec; i++) r->effects[i] = effects[i];
    audio_mixer_update_effects(r);
    audio_mixer_update_routing(r, splitters, h.splitters, mixes, h.mixes, mc);
    for (int i = 0; i < sc; i++) r->sinks[i] = sinks[i];

    // ---- output
    const uint32_t eo = effect_out_bytes(r);
    AudrenHeader o = {0};
    o.revision = AUDREN_REV(revision_number(h.revision) >= 1 ? revision_number(h.revision) : 1);
    o.memory_pools = npools * sizeof(AudrenPoolOut);
    o.voices = (uint32_t)vc * sizeof(AudrenVoiceOut);
    o.effects = (uint32_t)ec * eo;
    o.sinks = (uint32_t)sc * sizeof(AudrenSinkOut);
    o.performance = 0x10;
    o.behavior = AUDREN_BEHAVIOR_OUT;
    o.render_info = AUDREN_RENDER_INFO_OUT;
    o.total = (uint32_t)sizeof o + o.memory_pools + o.voices + o.effects + o.sinks + o.performance + o.behavior +
              o.render_info;
    if (o.total > out_size) {
        rt_log("audren: output needs %u bytes, the client gave %llu (effect out %u)", o.total,
               (unsigned long long)out_size, eo);
        need(0, "update output size");
    }
    memset(output, 0, out_size);
    uint8_t *w = output;
    memcpy(w, &o, sizeof o);
    w += sizeof o;
    for (uint32_t i = 0; i < npools; i++) {
        uint32_t s = pools[i].state;
        if (s == POOL_REQUEST_ATTACH) s = POOL_ATTACHED;
        else if (s == POOL_REQUEST_DETACH) s = POOL_DETACHED;
        r->pools[i] = pools[i];
        r->pools[i].state = s;
        AudrenPoolOut po = {s, {0}};
        memcpy(w, &po, sizeof po);
        w += sizeof po;
    }
    for (int i = 0; i < vc; i++) {
        AudrenVoiceOut vo = {r->voices[i].played, r->voices[i].consumed, r->voices[i].drops};
        memcpy(w, &vo, sizeof vo);
        w += sizeof vo;
    }
    for (int i = 0; i < ec; i++) {   // EffectStatus Enabled 3 / Disabled 4
        w[0] = effects[i].enabled ? 3 : 4;
        if (eo > 16) audio_mixer_effect_result(r, i, w + 16, eo - 16);
        w += eo;
    }
    for (int i = 0; i < sc; i++) {
        AudrenSinkOut so = {0};
        if (r->sinks[i].type == SINK_CIRCULAR) so.last_written_offset = audio_mixer_sink_written(r, i);
        memcpy(w, &so, sizeof so);
        w += sizeof so;
    }
    w += 0x10;                    // performance: nothing written
    w += AUDREN_BEHAVIOR_OUT;     // behavior: no errors
    memcpy(w, &r->elapsed_frames, 8);
    AudrenStats *st = r->stats;
    uint64_t late_gap = 0, late_passes = 0;
    int voices_line = 0, voices_used = 0, voices_playing = 0, voices_most = 0;
    if (st) {   // NX2NAVI_AUDIO_STATS: the game's cadence and how long its update held the lock
        uint64_t now = kern_now_ns(), gap = st->upd_last_ns ? now - st->upd_last_ns : 0,
                 passes = st->upd_last_ns ? r->elapsed_frames - st->upd_pass_last : 0;
        if (now - locked_ns > st->upd_hold_max_ns) st->upd_hold_max_ns = now - locked_ns;
        if (gap > st->upd_gap_max_ns) st->upd_gap_max_ns = gap, st->upd_gap_frame = game_frame();
        if (gap > AS_UPDATE_NS) st->upd_gaps_over++;
        if (passes > st->upd_passes_max) st->upd_passes_max = passes;
        if (gap > AS_UPDATE_EVENT_NS && r->state == 0 && st->events < AS_EVENTS)
            st->events++, late_gap = gap, late_passes = passes;
        st->upd_last_ns = now, st->upd_pass_last = r->elapsed_frames, st->upd_n++;
        // The voices the game holds: its sound engine takes one per sound from a budget of the renderer's voice count,
        // so sounds that are never let go show as a count that climbs from area to area until new sounds are refused.
        if (now - st->voices_ns >= AS_WINDOW_NS) {
            int used = 0, playing = 0;
            for (int k = 0; k < r->param.voice_count; k++)
                used += r->voices[k].in.in_use != 0, playing += r->voices[k].in.in_use && r->voices[k].state == SV_STARTED;
            if (used > st->voices_most) st->voices_most = used;
            st->voices_ns = now;
            voices_line = 1, voices_used = used, voices_playing = playing, voices_most = st->voices_most;
        }
    }
    plat_mutex_unlock(&r->lock);
    if (voices_line)
        rt_log("audio: voices in use %d, playing %d, of %d (the most so far %d), game frame %llu", voices_used,
               voices_playing, r->param.voice_count, voices_most, (unsigned long long)game_frame());
    if (late_gap)
        rt_log("audio: the game's audio update came %.1f ms after its last one (%llu render passes of 5 ms), game frame "
               "%llu", late_gap / 1e6, (unsigned long long)late_passes, (unsigned long long)game_frame());
}

// ---- render tick: one frame each time the output device has taken the ring below its target (audio_mixer_pace_ns),
// or every sample_count / sample_rate seconds of the host clock without a device, while stopped or while it stalls;
// after a stall the frames that are owed are rendered and dropped (render_thread, "Catch-up")
static void advance_voice(AudioRenderer *r, ServerVoice *v) {
    if (!v->in.in_use || v->state != SV_STARTED || !v->in.sample_rate) return;
    uint64_t samples = audio_voice_consumed(r, v, (int)r->param.sample_count);
    const int had = v->valid[v->index];
    while (samples && v->valid[v->index]) {
        AudrenWaveBuffer *b = &v->buffers[v->index];
        if (audio_buffer_empty(b)) break;   // an endless empty loop skip_empty kept: it starves the voice, and its
                                            // zero length would make the loop below go round for good
        uint64_t length = (uint64_t)(b->end - b->start), left = length - v->offset;
        uint64_t step = samples < left ? samples : left;
        v->offset += step;
        v->played += step;
        samples -= step;
        if (v->offset < length) break;
        if (b->loop && (b->loop_count < 0 || v->loops_done < b->loop_count)) {
            v->loops_done++;
            v->offset = b->loop_start > b->start && b->loop_start < b->end ? (uint64_t)(b->loop_start - b->start) : 0;
            continue;
        }
        v->valid[v->index] = 0;
        v->consumed++;
        v->offset = 0;
        v->loops_done = 0;
        v->index = (v->index + 1) & 3;
        if (b->end_of_stream) {
            v->played = 0;
            break;
        }
        skip_empty(r, v);
    }
    // NX2NAVI_AUDIO_STATS: the queue ran out within this pass, not at an end of stream (which zeroes played)
    if (r->stats && samples && had && !v->valid[v->index] && v->played && !v->dry_pass) {
        v->dry_pass = r->elapsed_frames + 1;
        r->stats->dry++;
    }
}

// NX2NAVI_AUDIO_STATS, render thread: the pass that begins at `now` (lock held after waiting `lock_ns` for it).
// `paced`: the device paces this pass (the timer's passes, without a device or in a stall, keep their own cadence).
static void stats_pass(AudioRenderer *r, uint64_t now, uint64_t lock_ns, int paced) {
    AudrenStats *st = r->stats;
    AudioOutStats o;
    if (paced && st->last_pass_ns) {
        uint64_t gap = now - st->last_pass_ns;
        if (gap > st->gap_max_ns) st->gap_max_ns = gap;
        if (gap > AS_GAP_NS) st->gaps_over++;
    }
    st->last_pass_ns = now;
    st->passes++;
    if (lock_ns > st->lock_max_ns) st->lock_max_ns = lock_ns;
    if (lock_ns > AS_LOCK_NS) st->locks_over++;
    if (audio_mixer_out_stats(r, &o)) {
        if (o.queued_frames < st->queue_min) st->queue_min = o.queued_frames;
        if (o.pull_gap_max_ns > st->pull_gap_max_ns) st->pull_gap_max_ns = o.pull_gap_max_ns;
    }
}

// NX2NAVI_AUDIO_STATS, render thread after a pass, lock not held: the event lines and the window line.
static void stats_report(AudioRenderer *r, uint64_t now, uint64_t lock_ns, uint64_t gap_ns) {
    AudrenStats *st = r->stats, snap;
    AudioOutStats o = {0};
    int dev = audio_mixer_out_stats(r, &o);
    char event[320] = "";
    int window = 0;
    uint64_t ramp_passes, ramps;
    plat_mutex_lock(&r->lock);
    audio_mixer_ramp_stats(r, &ramp_passes, &ramps);
    if (dev && o.pull_gap_max_ns > st->pull_gap_max_ns) st->pull_gap_max_ns = o.pull_gap_max_ns;
    if (dev && o.underruns > st->seen_underruns) {
        if (st->events < AS_EVENTS) {
            st->events++;
            snprintf(event, sizeof event,
                     "audio: underrun at game frame %llu (%llu so far, %.1f ms silent so far): this render pass came "
                     "%.1f ms after the one before and waited %.2f ms for the lock; the game's last update was %.1f ms "
                     "ago",
                     (unsigned long long)game_frame(), (unsigned long long)o.underruns,
                     o.missing_frames * 1000.0 / r->param.sample_rate, gap_ns / 1e6, lock_ns / 1e6,
                     st->upd_last_ns ? (now - st->upd_last_ns) / 1e6 : 0.0);
        }
        st->seen_underruns = o.underruns;
    }
    if (!st->window_ns) {
        st->window_ns = now;
        st->out_underruns = o.underruns, st->out_missing = o.missing_frames, st->out_skipped = o.skipped_frames;
        st->out_overruns = o.overruns;
        st->ramp_passes = ramp_passes, st->ramps = ramps;
    }
    if (now - st->window_ns >= AS_WINDOW_NS) {
        snap = *st;
        window = 1;
        st->windows++;
        st->window_ns = now;
        st->passes = st->gap_max_ns = st->gaps_over = st->lock_max_ns = st->locks_over = st->pull_gap_max_ns = 0;
        st->queue_min = UINT64_MAX;
        st->out_underruns = o.underruns, st->out_missing = o.missing_frames, st->out_skipped = o.skipped_frames;
        st->out_overruns = o.overruns;
        st->ramp_passes = ramp_passes, st->ramps = ramps;
        st->upd_n = 0;   // upd_last_ns and upd_pass_last stay: the next gap is measured from the last update
        st->upd_gap_max_ns = st->upd_gap_frame = st->upd_gaps_over = st->upd_passes_max = st->upd_hold_max_ns = 0;
        st->dry = st->late[0] = st->late[1] = st->late[2] = st->late_max = 0;
        st->caught_up = st->gave_up = 0;
    }
    plat_mutex_unlock(&r->lock);
    if (*event) rt_log("%s", event);
    if (!window) return;
    const double rate = r->param.sample_rate;
    const uint64_t under = o.underruns - snap.out_underruns;
    const int bad = under || o.overruns != snap.out_overruns || o.skipped_frames != snap.out_skipped ||
                    snap.gaps_over || snap.locks_over || snap.upd_gaps_over || snap.dry || snap.caught_up ||
                    snap.gave_up;
    if (!bad && snap.windows % 6 != 5) return;
    rt_log("audio: stats %.0f s to game frame %llu: device underruns %llu (%.1f ms silent), skipped %.1f ms, dropped "
           "frames %llu, ring at least %.1f ms, longest device request gap %.1f ms; render passes %llu, gaps over %llu "
           "ms %llu (longest %.1f ms), lock waits over %llu ms %llu (longest %.2f ms); game updates %llu, gaps over "
           "%llu ms %llu (longest %.1f ms at game frame %llu, %llu passes), longest held the lock %.2f ms; voices ran "
           "dry %llu, next buffer late by 1-2 passes %llu, 3-8 %llu, 9-%d %llu (most %llu); render passes that ramped "
           "a volume %llu (%llu ramps); catch-up frames rendered and dropped %llu, gaps over %d ms not caught up %llu",
           (now - snap.window_ns) / 1e9, (unsigned long long)game_frame(), (unsigned long long)under,
           (o.missing_frames - snap.out_missing) * 1000.0 / rate, (o.skipped_frames - snap.out_skipped) * 1000.0 / rate,
           (unsigned long long)(o.overruns - snap.out_overruns),
           snap.queue_min == UINT64_MAX ? 0.0 : snap.queue_min * 1000.0 / rate, snap.pull_gap_max_ns / 1e6,
           (unsigned long long)snap.passes, (unsigned long long)(AS_GAP_NS / 1000000), (unsigned long long)snap.gaps_over,
           snap.gap_max_ns / 1e6, (unsigned long long)(AS_LOCK_NS / 1000000), (unsigned long long)snap.locks_over,
           snap.lock_max_ns / 1e6, (unsigned long long)snap.upd_n, (unsigned long long)(AS_UPDATE_NS / 1000000),
           (unsigned long long)snap.upd_gaps_over, snap.upd_gap_max_ns / 1e6, (unsigned long long)snap.upd_gap_frame,
           (unsigned long long)snap.upd_passes_max, snap.upd_hold_max_ns / 1e6, (unsigned long long)snap.dry,
           (unsigned long long)snap.late[0], (unsigned long long)snap.late[1], AS_LATE_PASSES,
           (unsigned long long)snap.late[2], (unsigned long long)snap.late_max,
           (unsigned long long)(ramp_passes - snap.ramp_passes), (unsigned long long)(ramps - snap.ramps),
           (unsigned long long)snap.caught_up, AUDIO_CATCHUP_MAX_MS, (unsigned long long)snap.gave_up);
}

static void render_thread(void *p) {
    AudioRenderer *r = p;
    plat_thread_priority(PLAT_PRIO_HIGHEST);
    const uint64_t frame_ns = (uint64_t)r->param.sample_count * 1000000000ull / (uint64_t)r->param.sample_rate;
    const double frame = (double)r->param.sample_count / r->param.sample_rate;
    // When the device pulls a period (1024 samples on PipeWire, 4-5 frames) the ring drops below its target
    // and the frames would otherwise render back to back. On hardware the DSP renders one frame every 5 ms and
    // the game's audio thread works in between; AUX effects need that time to fill the return ring, or mix_aux
    // reads stale samples and the frame clicks at its seams. Device-paced frames stay at least 4/5 of a frame
    // apart, which still refills a period well before the device drains it. Linux only: on Windows SDL pulls
    // 480-sample periods, and a sink dump (NX2NAVI_AUDIO_DUMP) of the bursts there showed no more seam spikes than
    // chance, so a wait on this thread buys nothing.
#ifdef __linux__
    const uint64_t gap_ns = frame_ns * 4 / 5;
#else
    const uint64_t gap_ns = 0;
#endif
    // Catch-up (NX2NAVI_AUDIO_CATCHUP, default 1). The console's audio hardware takes a frame every 5 ms whatever the
    // rest of the system does, so the game's sound keeps its place against the picture. Here a frame that is not
    // rendered when it is due is time the sound loses for good: the device plays silence meanwhile, rendering goes on
    // where it stopped, and everything after the gap is heard that much later than the picture it belongs to. So after
    // a stall the frames that are owed are rendered and dropped: mixed in full, which moves the voices, the effects
    // and what the game reads back on as if they had been heard, and not queued for the device, whose queue is then
    // filled as before. While the device paces, it says what is owed: the silence it played for want of frames
    // (audio_mixer_owed), asked before anything is waited for or queued, so the dropped frames come right after the
    // gap. On the timer a frame is owed when its deadline passed a whole frame ago. A gap of more than
    // AUDIO_CATCHUP_MAX_MS (a debugger, the PC asleep) is not caught up: the sound goes on from now. With the setting
    // off nothing is dropped, and the timer queues frames up to 100 ms late back to back and skips a longer gap.
    const int catchup = cfg_bool("NX2NAVI_AUDIO_CATCHUP");
    const uint64_t late_max_ns = catchup ? AUDIO_CATCHUP_MAX_MS * 1000000ull : 100000000ull;
    uint64_t next = kern_now_ns(), last = 0, gave_up = 0;
    while (plat_cas32(&r->running, 0, 0)) {
        uint64_t now = kern_now_ns();
        int64_t pace = audio_mixer_pace_ns(r, now);
        int drop = 0;
        if (catchup && pace >= 0) {
            int owed = audio_mixer_owed(r);
            drop = owed > 0;
            gave_up += owed < 0;
        }
        if (pace > 0 && !drop) {
            plat_wait_ns((uint64_t)pace);
            continue;
        }
        if (pace == 0 && !drop && last && now - last < gap_ns) {
            plat_wait_ns(gap_ns - (now - last));
            continue;
        }
        if (pace >= 0) {
            next = now + frame_ns;   // where the timer goes on if the device stops pacing
        } else {
            if (next > now) plat_wait_ns(next - now);
            else if (now - next > late_max_ns) next = now, gave_up += catchup;   // fell far behind: resynchronise
            else if (catchup && now - next >= frame_ns) drop = 1;
            next += frame_ns;
        }
        const uint64_t prev = last;
        last = kern_now_ns();
        plat_mutex_lock(&r->lock);
        const uint64_t lock_ns = kern_now_ns() - last;
        if (r->stats && r->state == 0) {
            stats_pass(r, last, lock_ns, pace >= 0);
            r->stats->caught_up += drop;
            r->stats->gave_up += gave_up;
            gave_up = 0;
        }
        if (r->state == 0) {
            voices_prepare(r);
            audio_mixer_render(r, frame, drop);
            for (int i = 0; i < r->param.voice_count; i++) advance_voice(r, &r->voices[i]);
            r->elapsed_frames++;
            uint64_t given = r->stop_released + r->empty_played + r->flushed;
            if ((rt_trace & RT_TRACE_IPC) && given != r->logged_released && r->elapsed_frames % 1000 == 0) {
                r->logged_released = given;
                rt_log("audren: wave buffers given back: %llu by stopped voices, %llu without samples, %llu flushed",
                       (unsigned long long)r->stop_released, (unsigned long long)r->empty_played,
                       (unsigned long long)r->flushed);
            }
        }
        plat_mutex_unlock(&r->lock);
        kern_event_signal(r->event);
        if (r->stats && r->state == 0) stats_report(r, kern_now_ns(), lock_ns, prev ? last - prev : 0);
    }
}

// ---- IAudioRenderer
static IpcBuffer pick(const IpcBuffer *a, unsigned na, const IpcBuffer *b, unsigned nb, unsigned i) {
    IpcBuffer x = i < na ? a[i] : (IpcBuffer){0}, y = i < nb ? b[i] : (IpcBuffer){0};
    return x.size ? x : y;
}

static uint32_t voice_drop_bits = 0x3f800000u;   // SetVoiceDropParameter's float (1.0 until set)

static void renderer_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    AudioRenderer *r = s->state;
    switch (rq->cmd) {
    case 0: { int32_t v = r->param.sample_rate; IPC_OUT(rs, v); return; }       // GetSampleRate
    case 1: { int32_t v = r->param.sample_count; IPC_OUT(rs, v); return; }      // GetSampleCount
    case 2: { int32_t v = r->param.mix_buffer_count; IPC_OUT(rs, v); return; }  // GetMixBufferCount
    case 3: { uint32_t v = r->state; IPC_OUT(rs, v); return; }                  // GetState
    case 4: case 10: {   // RequestUpdate(Auto): out(output), out(performance), in(input)
        IpcBuffer in = pick(rq->a, rq->na, rq->x, rq->nx, 0);
        IpcBuffer out = pick(rq->b, rq->nb, rq->c, rq->nc, 0), perf = pick(rq->b, rq->nb, rq->c, rq->nc, 1);
        if (!r->logged)
            rt_log("audren: RequestUpdate cmd %u in=%#llx+%#llx out=%#llx+%#llx perf=%#llx+%#llx (a%u b%u x%u c%u)",
                   rq->cmd, (unsigned long long)in.addr, (unsigned long long)in.size, (unsigned long long)out.addr,
                   (unsigned long long)out.size, (unsigned long long)perf.addr, (unsigned long long)perf.size, rq->na,
                   rq->nb, rq->nx, rq->nc);
        need(in.size && out.size, "RequestUpdate buffers");
        int prio = 0;
        if (g_game_prio) prio = plat_thread_priority_get(), plat_thread_priority(PLAT_PRIO_HIGHEST);   // AUDIO_GAME_PRIO
        request_update(r, ipc_guest(in.addr, in.size), in.size, ipc_guest(out.addr, out.size), out.size, perf.size);
        if (g_game_prio)
            plat_thread_priority(g_game_prio >= 2 && prio < PLAT_PRIO_ABOVE_NORMAL ? PLAT_PRIO_ABOVE_NORMAL : prio);
        for (unsigned i = 0; i < rq->nc && rq->cmd == 10; i++)
            if (rq->c[i].size && (i >= rq->nb || !rq->b[i].size))
                rs->xout[rs->nxout++] = (IpcBuffer){rq->c[i].addr, rq->c[i].size, i};
        return;
    }
    case 5:   // Start
        plat_mutex_lock(&r->lock);
        r->state = 0;
        audio_mixer_set_running(r, 1);
        plat_mutex_unlock(&r->lock);
        return;
    case 6:   // Stop
        plat_mutex_lock(&r->lock);
        r->state = 1;
        audio_mixer_set_running(r, 0);
        plat_mutex_unlock(&r->lock);
        return;
    case 7: rs->copy[rs->ncopy++] = kern_event_readable(r->event); return;   // QuerySystemEvent
    case 8: r->time_limit = IPC_ARG(rq, uint32_t, 0); return;                // SetRenderingTimeLimit
    case 9: { uint32_t v = r->time_limit ? r->time_limit : 100; IPC_OUT(rs, v); return; }   // GetRenderingTimeLimit
    // 15.0.0+ Set/GetVoiceDropParameter (the system's audio renderer service): the factor the DSP uses to drop
    // voices when a frame overruns its time. The host mixer renders every voice, so the value is only kept.
    case 12: {
        float v = IPC_ARG(rq, float, 0);
        __atomic_store_n(&voice_drop_bits, isfinite(v) ? *(uint32_t *)&v : 0x3f800000u, __ATOMIC_RELAXED);
        return;
    }
    case 13: {
        uint32_t bits = __atomic_load_n(&voice_drop_bits, __ATOMIC_RELAXED);
        float v;
        memcpy(&v, &bits, sizeof v);
        IPC_OUT(rs, v);
        return;
    }
    default: ipc_unimplemented(s, rq);
    }
}

static void renderer_close(void *p) {
    AudioRenderer *r = p;
    plat_xchg32(&r->running, 0);
    int away = kern_core_away();
    plat_thread_join(r->thread);
    kern_core_back(&away);
    plat_dec32(&g_live);
    audio_mixer_close(r);
    kern_handle_close(r->event);
    rt_log("audren: renderer closed after %llu frames", (unsigned long long)r->elapsed_frames);
    renderer_free(r);
}

// ---- IAudioRendererManager
void audren_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: {   // OpenAudioRenderer(param, pad, work size, aruid; copy tmem, process; pid)
        AudrenParameter p = IPC_ARG(rq, AudrenParameter, 0);
        uint64_t work = IPC_ARG(rq, uint64_t, 0x38);
        need(rq->ncopy >= 1, "OpenAudioRenderer without a transfer memory handle");
        IpcTransferMemory *tm = kern_handle_get(rq->copy[0], KERN_TMEM);
        need(tm && tm->size >= work, "OpenAudioRenderer work buffer is not a transfer memory of the stated size");
        AudioRenderer *r = renderer_new(&p);
        if (rt_knob("AUDIO_STATS", "0")) {
            r->stats = calloc(1, sizeof *r->stats);
            if (r->stats) r->stats->queue_min = UINT64_MAX;
        }
        const char *gp = cfg_get("NX2NAVI_AUDIO_GAME_PRIO");
        g_game_prio = gp && *gp ? atoi(gp) : 0;
        if (g_game_prio < 0 || g_game_prio > 2) g_game_prio = 0;
        rt_log("audio: stats %s (NX2NAVI_AUDIO_STATS); the game's audio thread %s (NX2NAVI_AUDIO_GAME_PRIO=%d); frames "
               "owed after a stall are %s (NX2NAVI_AUDIO_CATCHUP)",
               r->stats ? "on" : "off",
               g_game_prio == 2   ? "runs above normal priority, at the highest while its update is read"
               : g_game_prio == 1 ? "runs at the highest priority while its update is read"
                                  : "keeps its own priority",
               g_game_prio, cfg_bool("NX2NAVI_AUDIO_CATCHUP") ? "rendered and dropped" : "not caught up");
        r->running = 1;
        audio_mixer_open(r);
        r->thread = plat_thread_new(render_thread, r, "audren");
        if (!r->thread) rt_fatal("audren: cannot start the render thread");
        plat_inc32(&g_live);
        rt_log("audren: renderer open %d Hz x%d, mix buffers %d, voices %d, effects %d, submixes %d, sinks %d, "
               "splitters %d/%d, REV%d, work buffer %#llx+%#llx",
               p.sample_rate, p.sample_count, p.mix_buffer_count, p.voice_count, p.effect_count, p.submix_count,
               p.sink_count, p.splitter_count, p.splitter_destination_count, revision_number(p.revision),
               (unsigned long long)tm->addr, (unsigned long long)work);
        rs->move[rs->nmove++] = ipc_session_new("IAudioRenderer", renderer_handler, r, renderer_close);
        return;
    }
    case 1: {   // GetWorkBufferSize(param) -> u64
        AudrenParameter p = IPC_ARG(rq, AudrenParameter, 0);
        need(p.sample_rate == 48000 || p.sample_rate == 32000, "GetWorkBufferSize: unsupported output rate");
        uint64_t v = AUDREN_WORK_BUFFER_BYTES;
        IPC_OUT(rs, v);
        return;
    }
    case 2: case 4:   // GetAudioDeviceService(aruid), GetAudioDeviceServiceWithRevisionInfo(revision, aruid)
        rs->move[rs->nmove++] = ipc_session_new("IAudioDevice", audio_device_handler, NULL, NULL);
        return;
    default: ipc_unimplemented(s, rq);
    }
}
