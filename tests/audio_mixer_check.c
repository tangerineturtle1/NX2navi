/* The mixer's volume ramps (runtime/audio_mixer.c accumulate_ramp, audio_mixer_render) and what it owes the device
   after a stall (ring_pull, audio_mixer_owed), with the rest of the runtime stubbed. No game data and no audio device:
   NX2NAVI_AUDIO=none mixes without opening one, and the device's requests are made by hand. Each voice plays a wave
   buffer of one constant sample, so a mix buffer holds the gain the voice went in with, sample by sample. */
#include "../runtime/audio_mixer.c"

#include <stdarg.h>

static int fails;
#define CHECK(ok, ...) do { if (!(ok)) { fails++; printf("FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the runtime around the mixer ---- */
static int ramp_setting = 1;
unsigned rt_trace;
void rt_log(const char *fmt, ...) { (void)fmt; }
void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    exit(2);
}
uint64_t kern_now_ns(void) { return 1; }
uint32_t kern_event_new(void) { return 1; }
uint32_t kern_event_readable(uint32_t writable) { return writable; }
void kern_event_signal(uint32_t writable) { (void)writable; }
void *ipc_guest(uint64_t addr, uint64_t size) { (void)size; return (void *)(uintptr_t)addr; }
uint32_t ipc_session_new(const char *name, IpcHandler handler, void *state, void (*close)(void *state)) {
    (void)name, (void)handler, (void)state, (void)close;
    return 1;
}
void ipc_unimplemented(const IpcSession *s, const IpcRequest *rq) { (void)s, (void)rq; exit(2); }
void ipc_out(IpcResponse *rs, const void *p, uint32_t n) { (void)rs, (void)p, (void)n; }
const void *ipc_arg(const IpcRequest *rq, uint32_t off, uint32_t n) { (void)rq, (void)off, (void)n; exit(2); }
void menu_sound_mix(float *out, uint32_t frames, int channels, int rate) { (void)out, (void)frames, (void)channels, (void)rate; }
const char *cfg_get(const char *name) { return !strcmp(name, "NX2NAVI_AUDIO") ? "none" : NULL; }
const char *cfg_value(const char *name) { return !strcmp(name, "NX2NAVI_AUDIO_CHANNELS") ? "stereo" : "default"; }
int cfg_bool(const char *name) {
    if (!strcmp(name, "NX2NAVI_AUDIO_RAMP")) return ramp_setting;
    printf("unexpected setting %s\n", name);
    exit(2);
}
double cfg_double(const char *name) { (void)name; return 100; }
unsigned cfg_version(void) { return 1; }
int audio_buffer_empty(const AudrenWaveBuffer *b) { return !b->address || !b->size || b->end <= b->start; }
/* no effects in these renderers */
AudioEffectDsp *audio_effect_create(unsigned type, const void *param, unsigned revision) { (void)type, (void)param, (void)revision; return NULL; }
void audio_effect_free(AudioEffectDsp *s) { (void)s; }
void audio_effect_update(AudioEffectDsp *s, const void *param) { (void)s, (void)param; }
void audio_effect_process(AudioEffectDsp *s, float *const input[6], float *const output[6], int frames) { (void)s, (void)input, (void)output, (void)frames; }
void audio_effect_statistics(AudioEffectDsp *s, void *output, unsigned bytes) { (void)s, (void)output, (void)bytes; }
Reverb3d *audio_reverb_create(const Reverb3dParam *p) { (void)p; return NULL; }
void audio_reverb_free(Reverb3d *s) { (void)s; }
void audio_reverb_update(Reverb3d *s, const Reverb3dParam *p) { (void)s, (void)p; }
void audio_reverb_process(Reverb3d *s, float *const input[6], float *const output[6], int frames) { (void)s, (void)input, (void)output, (void)frames; }

/* ---- a renderer: the final mix (4 buffers, the mixer's buffers 0-3), one submix (2 buffers, 4-5) that goes
   nowhere, three voices, one splitter with one destination ---- */
#define FRAME 240
#define SUBMIX 4   /* the submix's first buffer */
static int16_t pcm[4 * FRAME];   /* every sample 16384: 0.5 */

static AudioRenderer *renderer_open(void) {
    AudioRenderer *r = calloc(1, sizeof *r);
    r->param = (AudrenParameter){.sample_rate = 48000, .sample_count = FRAME, .mix_buffer_count = 8, .submix_count = 1,
                                 .voice_count = 3, .sink_count = 1, .effect_count = 0, .splitter_count = 1,
                                 .splitter_destination_count = 1, .revision = AUDREN_REV(11)};
    r->revision = r->param.revision;
    r->voices = calloc(3, sizeof *r->voices);
    r->mixes = calloc(2, sizeof *r->mixes);
    r->sinks = calloc(1, sizeof *r->sinks);
    r->effects = calloc(1, sizeof *r->effects);
    r->channels = calloc(3 * 6, sizeof *r->channels);
    r->mixes[0] = (AudrenMixIn){.volume = 1, .sample_rate = 48000, .buffer_count = 4, .in_use = 1, .mix_id = 0,
                                .dest_mix_id = -1, .dest_splitter_id = SPLITTER_UNUSED};
    r->mixes[1] = (AudrenMixIn){.volume = 1, .sample_rate = 48000, .buffer_count = 2, .in_use = 1, .mix_id = 1,
                                .dest_mix_id = 0, .dest_splitter_id = SPLITTER_UNUSED};
    r->sinks[0] = (AudrenSinkIn){.type = SINK_DEVICE, .in_use = 1, .input_count = 2, .inputs = {0, 1}};
    for (size_t i = 0; i < sizeof pcm / sizeof *pcm; i++) pcm[i] = 16384;
    audio_mixer_open(r);
    audio_mixer_set_running(r, 1);
    return r;
}

static void renderer_close(AudioRenderer *r) {
    audio_mixer_close(r);
    free(r->voices), free(r->mixes), free(r->sinks), free(r->effects), free(r->channels), free(r);
}

/* One render pass, counted as audren.c render_thread counts it; `drop`: a catch-up frame. The voices are not
   advanced: every sample is the same. */
static void render(AudioRenderer *r, int drop) {
    audio_mixer_render(r, (double)FRAME / 48000, drop);
    r->elapsed_frames++;
}
static void pass(AudioRenderer *r) { render(r, 0); }

/* A voice as the game's update leaves a new one (audren.c update_voice with is_new: voice_reset, then started). Of
   what the slot's last voice was mixed with, voice_reset clears only ramp_pass, so the rest is kept here too. */
static ServerVoice *voice_new(AudioRenderer *r, int i, float volume, int32_t mix, uint32_t splitter) {
    ServerVoice *v = &r->voices[i], was = *v;
    memset(v, 0, sizeof *v);
    v->ramp_mix = was.ramp_mix, v->ramp_splitter = was.ramp_splitter, v->ramp_channels = was.ramp_channels;
    v->ramp_buffers = was.ramp_buffers, v->ramp_volume = was.ramp_volume;
    memcpy(v->ramp_gain, was.ramp_gain, sizeof v->ramp_gain);
    v->in.id = (uint32_t)i, v->in.in_use = 1, v->in.play_state = PLAY_STARTED, v->in.sample_format = FMT_PCM16;
    v->in.sample_rate = 48000, v->in.channel_count = 1, v->in.pitch = 1, v->in.volume = volume;
    v->in.mix_id = mix, v->in.splitter_id = splitter;
    v->in.channel_ids[0] = i * 6;
    v->state = SV_STARTED;
    v->buffers[0] = (AudrenWaveBuffer){.address = (uintptr_t)pcm, .size = sizeof pcm, .start = 0, .end = 4 * FRAME};
    v->valid[0] = 1;
    return v;
}

/* The splitter section of an update (REV11 layout): splitter 0 with destination 0, which goes to `mix` with `volume`
   on that mix's buffer `buffer`. */
static void splitter_set(AudioRenderer *r, int used, int32_t mix, int buffer, float volume) {
    uint8_t wire[0x20 + 16 + 4 + 12 + 0x70 + 12] = {0};
    uint32_t header[3] = {0x48444e53u, 1, 1}, info[4] = {0x49444e53u, 0, 0, 1}, dest[2] = {0x44444e53u, 0};
    int32_t id = 0;
    float volumes[24] = {0};
    volumes[buffer] = volume;
    memcpy(wire, header, sizeof header);
    memcpy(wire + 0x20, info, sizeof info);
    memcpy(wire + 0x30, &id, 4);
    uint8_t *d = wire + 0x40;
    memcpy(d, dest, sizeof dest);
    memcpy(d + 8, volumes, sizeof volumes);
    memcpy(d + 0x68, &mix, 4);
    d[0x6c] = (uint8_t)used;
    AudrenMixIn mixes[2] = {r->mixes[0], r->mixes[1]};
    audio_mixer_update_routing(r, wire, sizeof wire, (const uint8_t *)mixes, sizeof mixes, 2);
}

static const float *buf(AudioRenderer *r, int b) { return mixbuf(r->mixer, b); }

/* Every sample of the pass went in with the same gain. */
static int flat(const float *d, float level) {
    for (int k = 0; k < FRAME; k++)
        if (fabsf(d[k] - level) > 1e-6f) return 0;
    return 1;
}

/* The pass went from `from` to `to` in equal steps: sample k on the line from + k (to - from) / FRAME, so the first
   sample is the level before and the pass after continues at `to`. */
static int ramped(const float *d, float from, float to) {
    for (int k = 0; k < FRAME; k++) {
        if (fabsf(d[k] - (from + (to - from) * (float)k / FRAME)) > 2e-5f) return 0;
        if (k && to > from ? d[k] <= d[k - 1] : k && d[k] >= d[k - 1]) return 0;
    }
    return d[0] == from;
}

static void voice_into_mix(void) {
    AudioRenderer *r = renderer_open();
    uint64_t passes, ramps;
    r->channels[0].mix[0] = 1, r->channels[0].mix[1] = 0.5f;
    ServerVoice *v = voice_new(r, 0, 0, 0, SPLITTER_UNUSED);
    pass(r);
    CHECK(flat(buf(r, 0), 0) && flat(buf(r, 1), 0), "a voice at volume 0 is silent");
    v->in.volume = 1;
    pass(r);
    CHECK(ramped(buf(r, 0), 0, 0.5f), "volume 0 to 1: the pass rises in equal steps (first %g, middle %g, last %g)",
          buf(r, 0)[0], buf(r, 0)[FRAME / 2], buf(r, 0)[FRAME - 1]);
    CHECK(ramped(buf(r, 1), 0, 0.25f), "each buffer of the mix ramps to its own gain");
    CHECK(flat(buf(r, 2), 0) && flat(buf(r, 3), 0), "buffers the voice does not go into stay silent");
    audio_mixer_ramp_stats(r, &passes, &ramps);
    CHECK(passes == 1 && ramps == 2, "one pass ramped, two buffers (%llu, %llu)", (unsigned long long)passes, (unsigned long long)ramps);
    pass(r);
    CHECK(flat(buf(r, 0), 0.5f) && flat(buf(r, 1), 0.25f), "the pass after the step holds the new volume");
    audio_mixer_ramp_stats(r, &passes, &ramps);
    CHECK(passes == 1 && ramps == 2, "an unchanged volume is not counted as a ramp");
    r->channels[0].mix[0] = 0.5f;   /* the channel's mix volume moves, the voice's does not */
    pass(r);
    CHECK(ramped(buf(r, 0), 0.5f, 0.25f) && flat(buf(r, 1), 0.25f), "a changed mix volume ramps, the other buffer does not");
    v->in.volume = 0;
    pass(r);
    CHECK(ramped(buf(r, 0), 0.25f, 0) && ramped(buf(r, 1), 0.25f, 0), "a volume set to 0 fades across the pass");
    pass(r);
    CHECK(flat(buf(r, 0), 0), "and is silent after it");

    /* a voice that starts is not ramped, whatever the slot held before */
    r->channels[6].mix[2] = 1;
    voice_new(r, 1, 1, 0, SPLITTER_UNUSED);
    pass(r);
    CHECK(flat(buf(r, 2), 0.5f), "a new voice starts at its volume (first sample %g)", buf(r, 2)[0]);
    r->voices[1].in.volume = 0.5f;
    pass(r);
    CHECK(ramped(buf(r, 2), 0.5f, 0.25f), "and ramps from its second pass on");
    voice_new(r, 1, 1, 0, SPLITTER_UNUSED);
    pass(r);
    CHECK(flat(buf(r, 2), 0.5f), "a voice started again in the same slot starts at its volume");

    /* not mixed in the pass before: paused, then played at another volume */
    r->voices[1].state = SV_PAUSED;
    pass(r);
    CHECK(flat(buf(r, 2), 0), "a paused voice is silent");
    r->voices[1].state = SV_STARTED, r->voices[1].in.volume = 0.25f;
    pass(r);
    CHECK(flat(buf(r, 2), 0.125f), "a voice that was silent in the pass before starts at its volume");

    /* another destination: the submix */
    r->voices[1].in.mix_id = 1, r->voices[1].in.volume = 1;
    r->channels[6].mix[0] = 1;
    pass(r);
    CHECK(flat(buf(r, SUBMIX), 0.5f) && flat(buf(r, 2), 0), "a voice moved to another mix starts at its volume there");
    r->voices[1].in.volume = 0.5f;
    pass(r);
    CHECK(ramped(buf(r, SUBMIX), 0.5f, 0.25f), "and ramps there afterwards");

    /* the renderer stopped and started: the pass before is not the sound before */
    audio_mixer_set_running(r, 0);
    audio_mixer_set_running(r, 1);
    r->voices[1].in.volume = 1;
    pass(r);
    CHECK(flat(buf(r, SUBMIX), 0.5f), "the first pass after a start takes every volume as it is");
    renderer_close(r);
}

static void voice_into_splitter(void) {
    AudioRenderer *r = renderer_open();
    splitter_set(r, 1, 0, 3, 0);
    ServerVoice *v = voice_new(r, 2, 1, 0, 0);
    pass(r);
    CHECK(flat(buf(r, 3), 0), "a destination at volume 0 is silent");
    splitter_set(r, 1, 0, 3, 1);
    pass(r);
    CHECK(ramped(buf(r, 3), 0, 0.5f), "destination volume 0 to 1: the pass rises in equal steps (first %g, last %g)",
          buf(r, 3)[0], buf(r, 3)[FRAME - 1]);
    pass(r);
    CHECK(flat(buf(r, 3), 0.5f), "the pass after the step holds the new volume");
    v->in.volume = 0.5f;
    pass(r);
    CHECK(ramped(buf(r, 3), 0.5f, 0.25f), "the voice's own volume ramps through the destination");
    v->in.volume = 1;
    splitter_set(r, 1, 0, 3, 0.5f);   /* both move: from the product before to the product now, here the same gain */
    pass(r);
    CHECK(flat(buf(r, 3), 0.25f), "voice volume doubled and destination volume halved: the gain does not move");

    splitter_set(r, 1, 1, 0, 1);   /* the destination goes to the submix */
    pass(r);
    CHECK(flat(buf(r, SUBMIX), 0.5f) && flat(buf(r, 3), 0), "a destination moved to another mix starts at its volume there");
    splitter_set(r, 0, 1, 0, 1);
    pass(r);
    CHECK(flat(buf(r, SUBMIX), 0), "an unused destination is silent");
    splitter_set(r, 1, 1, 0, 0.5f);
    pass(r);
    CHECK(flat(buf(r, SUBMIX), 0.25f), "a destination used again starts at its volume");
    splitter_set(r, 1, 1, 0, 1);
    voice_new(r, 2, 1, 0, 0);
    pass(r);
    CHECK(flat(buf(r, SUBMIX), 0.5f), "a new voice into a destination whose volume moved starts at the new gain");
    renderer_close(r);
}

/* A submix into a splitter ramps the destination's volumes like a voice does. */
static void mix_into_splitter(void) {
    AudioRenderer *r = renderer_open();
    r->channels[0].mix[0] = 1;
    r->mixes[1].dest_splitter_id = 0, r->mixes[1].buffer_count = 1;
    splitter_set(r, 1, 0, 3, 0.5f);
    voice_new(r, 0, 1, 1, SPLITTER_UNUSED);
    pass(r);
    CHECK(flat(buf(r, SUBMIX), 0.5f) && flat(buf(r, 3), 0.25f), "the submix reaches the final mix through the destination");
    splitter_set(r, 1, 0, 3, 1);
    pass(r);
    CHECK(ramped(buf(r, 3), 0.25f, 0.5f), "the destination's volume ramps for a mix");
    r->mixes[1].volume = 0.5f;
    pass(r);
    CHECK(ramped(buf(r, 3), 0.5f, 0.25f), "and so does the mix's own volume");
    renderer_close(r);
}

static void ramps_off(void) {
    ramp_setting = 0;
    AudioRenderer *r = renderer_open();
    r->channels[0].mix[0] = 1;
    ServerVoice *v = voice_new(r, 0, 0, 0, SPLITTER_UNUSED);
    pass(r);
    v->in.volume = 1;
    pass(r);
    CHECK(flat(buf(r, 0), 0.5f), "NX2NAVI_AUDIO_RAMP=0: the new volume applies to the whole pass");
    renderer_close(r);
    ramp_setting = 1;
}

/* What the device paces: the device's requests (ring_pull, the SDL callback's part) and the render thread's question
   (audio_mixer_owed) by hand, on a mixer that takes itself to have a device. */
static void catch_up(void) {
    AudioRenderer *r = renderer_open();
    Mixer *m = r->mixer;
    enum { PERIOD = 2 * FRAME };
    static float out[2 * (PERIOD + FRAME)];
    const int prefill = m->prefill;   /* render frames the output waits for; even on every platform */
    m->device_ok = 1, m->period = PERIOD, m->target = prefill * FRAME;
    r->channels[0].mix[0] = 1, r->channels[0].mix[1] = 1;
    ServerVoice *v = voice_new(r, 0, 1, 0, SPLITTER_UNUSED);

    ring_pull(m, out, PERIOD);
    ring_pull(m, out, PERIOD);
    CHECK(out[0] == 0 && audio_mixer_owed(r) == 0, "the silence before the first prefill is not owed");
    for (int i = 0; i < prefill; i++) pass(r);
    uint64_t queued = atomic_load(&m->wr) - atomic_load(&m->rd);
    CHECK(queued == (uint64_t)prefill * FRAME, "the frames are queued for the device (%llu samples)", (unsigned long long)queued);
    for (int i = 0; i < prefill; i += 2) {
        ring_pull(m, out, PERIOD);
        CHECK(out[0] == 0.5f && out[2 * PERIOD - 1] == 0.5f, "the device plays what was queued");
    }
    CHECK(audio_mixer_owed(r) == 0, "nothing is owed while the ring keeps up");

    /* the render thread is held up for three device periods: an underrun, then silence until the prefill is back */
    for (int i = 0; i < 3; i++) ring_pull(m, out, PERIOD);
    CHECK(out[0] == 0 && atomic_load(&m->underruns) == 1, "the device plays silence");
    v->in.volume = 0.5f;   /* the game's update that came meanwhile */
    for (int i = 0; i < 6; i++) {
        uint64_t wr = atomic_load(&m->wr), frames = r->elapsed_frames;
        CHECK(audio_mixer_owed(r) == 1, "three periods of silence: render frame %d of 6 is owed", i + 1);
        render(r, 1);
        CHECK(atomic_load(&m->wr) == wr && r->elapsed_frames == frames + 1, "a catch-up frame is rendered and not queued");
        if (i == 0) CHECK(ramped(buf(r, 0), 0.5f, 0.25f), "it is mixed in full: the volume the game set ramps in it");
    }
    CHECK(audio_mixer_owed(r) == 0, "after six frames nothing more is owed");
    for (int i = 0; i < prefill; i++) pass(r);
    queued = atomic_load(&m->wr) - atomic_load(&m->rd);
    CHECK(queued == (uint64_t)prefill * FRAME, "the ring is filled to the same target afterwards");
    ring_pull(m, out, PERIOD);
    CHECK(out[0] == 0.25f && out[2 * PERIOD - 1] == 0.25f, "the device goes on with the frames after the dropped ones");
    CHECK(audio_mixer_owed(r) == 0, "the device's requests while the ring keeps up add nothing");

    /* less than a render frame of silence waits for the next gap */
    for (int i = 2; i < prefill; i += 2) ring_pull(m, out, PERIOD);   /* the ring is empty again, */
    for (int i = 0; i < prefill; i++) pass(r);                        /* and filled */
    for (int i = 2; i < prefill; i += 2) ring_pull(m, out, PERIOD);   /* down to one period */
    ring_pull(m, out, PERIOD + 100);                                  /* a request for 100 samples more than that */
    CHECK(atomic_load(&m->underruns) == 2 && audio_mixer_owed(r) == 0, "100 samples of silence are not a frame yet");
    ring_pull(m, out, 200);                                           /* still waiting for the prefill: 300 in all */
    CHECK(audio_mixer_owed(r) == 1 && audio_mixer_owed(r) == 0, "300 samples of silence: one frame is owed, 60 samples stay");

    /* a stop forgets what was owed, and the wait for the prefill after the start is not owed */
    for (int i = 0; i < 3; i++) ring_pull(m, out, PERIOD);
    audio_mixer_set_running(r, 0);
    ring_pull(m, out, PERIOD);
    audio_mixer_set_running(r, 1);
    ring_pull(m, out, PERIOD);
    ring_pull(m, out, PERIOD);
    CHECK(audio_mixer_owed(r) == 0, "nothing is owed across a stop");

    /* more than AUDIO_CATCHUP_MAX_MS of silence is not caught up */
    for (int i = 0; i < prefill; i++) pass(r);
    for (int i = 0; i < prefill / 2 + 1 + 48000 / PERIOD * (AUDIO_CATCHUP_MAX_MS + 100) / 1000; i++) ring_pull(m, out, PERIOD);
    CHECK(audio_mixer_owed(r) == -1, "over %d ms of silence is given up", AUDIO_CATCHUP_MAX_MS);
    CHECK(audio_mixer_owed(r) == 0, "and nothing of it is owed afterwards");
    m->device_ok = 0;
    renderer_close(r);
}

int main(void) {
    voice_into_mix();
    voice_into_splitter();
    mix_into_splitter();
    ramps_off();
    catch_up();
    printf("audio_mixer_check: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
