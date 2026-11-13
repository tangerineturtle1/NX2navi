// Contains code Copyright (c) 2026 MacNativeGames contributors (MIT; see THIRD_PARTY_NOTICES.md).
// Wire layouts follow the public audren:u wire format.
// audren:u, the audio renderer service: RequestUpdate wire formats, the server-side
// renderer state, and the mixer interface (audio_mixer.c: the real mixer with SDL3 output).
#pragma once
#include <stdint.h>

#include "ipc.h"
#include "platform/platform.h"

typedef struct {
    int32_t sample_rate, sample_count, mix_buffer_count, submix_count, voice_count, sink_count, effect_count,
        performance_frame_count;
    uint8_t voice_drop_enabled, unknown, rendering_device, execution_mode;
    int32_t splitter_count, splitter_destination_count;
    uint32_t external_context_size, revision;
} AudrenParameter;
_Static_assert(sizeof(AudrenParameter) == 0x34, "AudioRendererParameterInternal");

typedef struct {
    uint32_t revision, behavior, memory_pools, voices, voice_resources, effects, mixes, sinks, performance,
        splitters, render_info, reserved[4], total;
} AudrenHeader;
_Static_assert(sizeof(AudrenHeader) == 0x40, "UpdateDataHeader");

typedef struct { uint64_t address, size; uint32_t state, pad[3]; } AudrenPoolIn;
typedef struct { uint32_t state, pad[3]; } AudrenPoolOut;
enum { POOL_INVALID, POOL_NEW, POOL_REQUEST_DETACH, POOL_DETACHED, POOL_REQUEST_ATTACH, POOL_ATTACHED, POOL_RELEASED };

typedef struct { uint32_t id; float mix[24]; uint8_t in_use, pad[11]; } AudrenChannelIn;
_Static_assert(sizeof(AudrenChannelIn) == 0x70, "voice channel resource");

typedef struct { uint8_t enable, pad; int16_t numerator[3], denominator[2]; } AudrenBiquad;
typedef struct { uint8_t enable, pad[3]; float numerator[3], denominator[2]; } AudrenBiquadFloat;
_Static_assert(sizeof(AudrenBiquadFloat) == 0x18, "REV15 float biquad");

typedef struct {
    uint64_t address, size;
    int32_t start, end;
    uint8_t loop, end_of_stream, sent, pad;
    int32_t loop_count;
    uint64_t context_address, context_size;
    int32_t loop_start, loop_end;
} AudrenWaveBuffer;
_Static_assert(sizeof(AudrenWaveBuffer) == 0x38, "wave buffer");

typedef struct {
    uint32_t id, node_id;
    uint8_t is_new, in_use, play_state, sample_format;
    uint32_t sample_rate;
    int32_t priority, sort_order;
    uint32_t channel_count;
    float pitch, volume;
    AudrenBiquad biquads[2];
    uint32_t wave_buffer_count;
    uint16_t wave_buffer_index, pad0;
    uint32_t pad1;
    uint64_t extra_address, extra_size;
    int32_t mix_id;
    uint32_t splitter_id;
    AudrenWaveBuffer wave_buffers[4];
    int32_t channel_ids[6];
    uint8_t clear_voice_drop, flush_wave_buffer_count, pad2[2], flags, pad3, src_quality, pad4[17];
} AudrenVoiceIn;
_Static_assert(sizeof(AudrenVoiceIn) == 0x170, "voice in parameter");
// REV15 widens only the two filter records (Q14 -> float); the fields after them move by 24 bytes.
typedef struct { uint8_t prefix[0x24]; AudrenBiquadFloat biquads[2]; uint8_t tail[0x134]; } AudrenVoiceInV2;
_Static_assert(sizeof(AudrenVoiceInV2) == 0x188, "REV15 voice in parameter");
typedef struct { uint64_t played_samples; uint32_t wave_buffers_consumed, voice_drops; } AudrenVoiceOut;
enum { PLAY_STARTED, PLAY_STOPPED, PLAY_PAUSED };
enum { FMT_INVALID, FMT_PCM8, FMT_PCM16, FMT_PCM24, FMT_PCM32, FMT_PCMF32, FMT_ADPCM };

typedef struct {
    float volume;
    uint32_t sample_rate;
    int32_t buffer_count;
    uint8_t in_use, dirty, pad0[2];
    int32_t mix_id;
    uint32_t effect_count, node_id, pad1[2];
    float mix[24][24];
    int32_t dest_mix_id;
    uint32_t dest_splitter_id, pad2;
} AudrenMixIn;
_Static_assert(sizeof(AudrenMixIn) == 0x930, "mix in parameter");

typedef struct {
    uint8_t type, in_use, pad0[2];
    uint32_t node_id;
    uint64_t pad1[3];
    char name[0x100];
    uint32_t input_count;
    int8_t inputs[6];
    uint8_t pad2, downmix_enabled;
    float downmix[4];
} AudrenSinkIn;
_Static_assert(sizeof(AudrenSinkIn) == 0x140, "sink in parameter");
typedef struct { uint32_t last_written_offset, pad0; uint64_t pad1[3]; } AudrenSinkOut;
enum { SINK_INVALID, SINK_DEVICE, SINK_CIRCULAR };

typedef struct {
    uint8_t type, is_new, enabled, pad0;
    int32_t mix_id;
    uint64_t buffer_address, buffer_size;
    int32_t order;
    uint32_t pad1;
    uint8_t specific[0xa0];
} AudrenEffectIn;
_Static_assert(sizeof(AudrenEffectIn) == 0xc0, "effect in parameter");

typedef struct {
    uint8_t input[6], output[6]; uint16_t channel_max, channels;
    int32_t sample_rate, lookahead_max, attack_time, release_time, lookahead;
    float attack, release, threshold, input_gain, output_gain;
    int32_t delay_min, delay_max; uint8_t status, statistics_enabled, statistics_reset, reserved;
} AudrenLimiterParam;
_Static_assert(sizeof(AudrenLimiterParam) == 68, "limiter effect wire layout");

typedef struct {
    uint8_t input[24], output[24]; uint32_t channels, sample_rate, capacity, channel_max;
    uint64_t send_info, send_storage, return_info, return_storage;
    uint32_t sample_size, total_samples, frame_samples;
} AudrenAuxParam;
_Static_assert(__builtin_offsetof(AudrenAuxParam, send_info) == 64, "auxiliary effect wire layout");

#define AUDREN_REV(n) (0x30564552u + ((uint32_t)(n) << 24))   // "REVn"
#define AUDREN_WORK_BUFFER_BYTES 0x10000u                     // the host keeps its state in host memory
#define AUDREN_BEHAVIOR_OUT 0xb0u
#define AUDREN_RENDER_INFO_OUT 0x10u
// Catch-up after a stall (NX2NAVI_AUDIO_CATCHUP; audren.c render_thread, audio_mixer.c audio_mixer_owed): a gap longer
// than this is not caught up, the sound goes on from now.
#define AUDIO_CATCHUP_MAX_MS 3000

// Server-side voice: the DSP's view of one client voice.
typedef struct {
    AudrenVoiceIn in;
    AudrenBiquadFloat float_biquads[2];
    uint8_t use_float_biquads;
    AudrenWaveBuffer buffers[4];
    uint8_t valid[4];
    uint8_t pending[4];            // sent while the voice was not playing: valid once it plays
    uint8_t state;                 // SV_*: the client's Start / Stop / Pause, plus Stopping until the next frame
    uint32_t flush, flush_from;    // FlushWaveBuffers since the last frame, from the client's current buffer
    unsigned index;
    uint64_t offset;
    double fraction;
    int32_t loops_done;
    uint64_t played;
    uint32_t consumed, drops;
    struct { int16_t history[2]; int32_t buffer; uint64_t at; } adpcm;   // DSP-ADPCM decode cursor (buffer slot, next sample) and history
    float biquad_state[2][6][2];   // filter, channel: transposed direct-form II delays
    uint64_t dry_pass;             // NX2NAVI_AUDIO_STATS: 1 + the render pass in which the voice ran out of queued
                                   // buffers before its end of stream (audren.c advance_voice), 0 otherwise
    // Volume ramps (audio_mixer.c audio_mixer_render): what the voice was mixed with in its last render pass. The pass
    // after it starts every gain there and reaches the new volumes at its end; voice_reset (audren.c) forgets it.
    uint64_t ramp_pass;            // 1 + the render pass the voice was last mixed in, 0 when it has none to ramp from
    int32_t ramp_mix;              // the mix and
    uint32_t ramp_splitter;        // the splitter it went to then, and
    uint32_t ramp_channels;        // its channel count: a pass with another of these starts at the new volumes
    int32_t ramp_buffers;          // into a mix: how many of the mix's buffers ramp_gain holds
    float ramp_volume;             // into a splitter: the voice's volume then (the destinations keep their own)
    float ramp_gain[6][24];        // into a mix: the gain channel c went into buffer b with (NaN: nothing went in)
} ServerVoice;
enum { SV_STOPPED, SV_STARTED, SV_PAUSED, SV_STOPPING };

typedef struct AudioRenderer {
    AudrenParameter param;
    PlatMutex lock;
    PlatThread *thread;
    volatile int32_t running;
    uint32_t state, revision, time_limit, event;   // state 0 started, 1 stopped; event: writable handle
    int logged;
    uint64_t behavior_flags, elapsed_frames;
    uint64_t stop_released, empty_played, flushed, logged_released;
    struct AudrenStats *stats;   // NX2NAVI_AUDIO_STATS (audren.c), NULL when off
    ServerVoice *voices;
    AudrenPoolIn *pools;
    AudrenMixIn *mixes;
    AudrenSinkIn *sinks;
    AudrenEffectIn *effects;
    AudrenChannelIn *channels;
    void *mixer;
} AudioRenderer;

// IAudioRendererManager, served under "audren:u" (ipc.c's service table).
void audren_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs);
long audren_renderers_live(void);      // renderers whose render tick runs

// Mixer interface (audio_mixer.c). Every call but open/close/set_running runs with r->lock held.
void audio_mixer_open(AudioRenderer *r);
void audio_mixer_set_running(AudioRenderer *r, int running);
void audio_mixer_close(AudioRenderer *r);
void audio_mixer_update_routing(AudioRenderer *r, const uint8_t *splitters, uint32_t splitter_bytes,
                                const uint8_t *mixes, uint32_t mix_bytes, int mix_count);
void audio_mixer_update_effects(AudioRenderer *r);
void audio_mixer_effect_result(AudioRenderer *r, int id, void *output, unsigned size);
uint32_t audio_mixer_sink_written(AudioRenderer *r, int id);
// After audio_voices_prepare, before voices advance. `drop`: a catch-up frame (audren.c render_thread). It is mixed in
// full, so the voices' filters, the effects and the sinks the game reads back advance as for any frame, and nothing
// of it is queued for the device.
void audio_mixer_render(AudioRenderer *r, double seconds, int drop);
// Device pacing for the render thread at kern_now_ns `now`: 0 renders the next frame now, a positive value waits that
// many ns (the ring is at its target), -1 leaves the pace to the render thread's timer (no device, renderer stopped,
// device stalled).
int64_t audio_mixer_pace_ns(AudioRenderer *r, uint64_t now);
// Catch-up for the frames the device paces (render thread, lock not held). 1: the device has played at least one render
// frame of silence for want of rendered sound that is not made up for yet; that frame is taken off what is owed and
// the caller renders one frame with `drop` set. 0: nothing is owed. -1: more than AUDIO_CATCHUP_MAX_MS was owed and is
// forgotten.
int audio_mixer_owed(AudioRenderer *r);
// NX2NAVI_AUDIO_STATS: the output's counters since it opened, and the longest gap between two device requests since
// the last call (which starts that measure again). False without a device.
typedef struct { uint64_t underruns, missing_frames, skipped_frames, overruns, queued_frames, pull_gap_max_ns; } AudioOutStats;
int audio_mixer_out_stats(AudioRenderer *r, AudioOutStats *o);
// NX2NAVI_AUDIO_STATS: the render passes in which at least one volume was ramped (NX2NAVI_AUDIO_RAMP), and the ramps (one
// source channel into one mix buffer), since the mixer opened.
void audio_mixer_ramp_stats(const AudioRenderer *r, uint64_t *passes, uint64_t *ramps);
uint64_t audio_voice_consumed(const AudioRenderer *r, ServerVoice *v, int frames);   // source samples over one frame; advances v->fraction
int audio_buffer_empty(const AudrenWaveBuffer *b);   // no samples to decode: played as soon as it is reached
void audio_device_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs);   // IAudioDevice
void audio_channels_live(void);   // after a settings change: another NX2NAVI_AUDIO_CHANNELS reaches the game and the mixer
void audio_dmg_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs);      // auddmg:u (Switch 2)
void audio_sysmix_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs);   // audsmx:u (Switch 2)
