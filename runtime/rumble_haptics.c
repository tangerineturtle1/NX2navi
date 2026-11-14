// Contains code adapted from RenHDrum, Copyright (c) 2026 Three-Dola (MIT; see THIRD_PARTY_NOTICES.md).
// HD rumble on a DualSense through its haptic actuators (input.c HD_AUDIO; NX2NAVI_RUMBLE_HD=1). Over USB the
// controller is also a four-channel audio device: the first two channels are its speaker and headphone jack, the third
// and fourth drive its left and right voice-coil actuators directly. The game's vibration value per side (two bands,
// each a frequency and an amplitude, ov_input.c VibrationValue) is played there as the sum of two sines at the bands'
// frequencies, so the actuators follow the game's frequencies instead of the two fixed ones of rumble emulation. Over
// Bluetooth the controller has no audio device and keeps its motors.
//
// The amplitudes this game sends are tuned for the Switch 2's own actuators and are very small (most of them under
// 0.05), so each band first goes through a piecewise curve that lifts them (haptics_lift: nothing at or below 0.001,
// then straight lines through 0.004 at 0.001, 0.55 at 0.1, 0.9 at 0.5 and 1 at 1), then through a square root, as the
// felt strength of a voice coil follows the root of its drive; the two bands are scaled down together when they add up
// past full scale. The output never goes past 0.8 of full scale, to spare the coils.
//
// Samples are made in slots of SLOT frames (5.3 ms): each slot plays the strongest value posted during the slot before
// it, so a tick of a single update (the game updates every 5 ms) followed at once by zero is still played. Within a
// slot each band's amplitude and frequency move toward the slot's value with a 4 ms time constant, and each band's
// phase runs on, so changes make no click. A side whose value is not renewed for STALE_NS (the game paused or frozen,
// or vibration turned off) falls silent.
#include <math.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "host.h"
#include "platform/platform.h"
#include "rumble_haptics.h"

#define RATE 48000
#define CHANNELS 4
#define SLOT 256
#define SMOOTH_MS 4.0
#define CEILING 0.8f
#define STALE_NS 250000000ull

static float clampf(float v, float lo, float hi) { return v > lo ? (v < hi ? v : hi) : lo; }   // NaN -> lo

float haptics_lift(float a) {
    const float t = 0.001f, p1 = 0.004f, p2 = 0.55f, p3 = 0.9f, p4 = 1.0f;
    if (!(a > t)) return 0;   // NaN too
    if (a >= 1) return p4;
    float v = a < 0.1f ? p1 + (a - t) * (p2 - p1) / (0.1f - t)
            : a < 0.5f ? p2 + (a - 0.1f) * (p3 - p2) / 0.4f
                       : p3 + (a - 0.5f) * (p4 - p3) / 0.5f;
    return clampf(v, 0, 1);
}

HapticsSide haptics_side(const RumbleHdValue *v) {
    float lo = sqrtf(haptics_lift(clampf(v->amp_low, 0, 1))), hi = sqrtf(haptics_lift(clampf(v->amp_high, 0, 1)));
    if (lo + hi > 1) {
        float k = 1 / (lo + hi);
        lo *= k, hi *= k;
    }
    HapticsSide s = {{lo, hi}, {v->freq_low > 0 ? clampf(v->freq_low, 20, 1000) : 160,
                                v->freq_high > 0 ? clampf(v->freq_high, 20, 1000) : 320}};
    return s;
}

void haptics_synth(HapticsVoice *voice, const HapticsSide *target, float gain, float *out, int frames, int stride) {
    const float k = (float)(1 - exp(-1000.0 / (RATE * SMOOTH_MS)));
    for (int i = 0; i < frames; i++) {
        double s = 0;
        for (int b = 0; b < 2; b++) {
            voice->amp[b] += (target->amp[b] - voice->amp[b]) * k;
            voice->freq[b] += (target->freq[b] - voice->freq[b]) * k;
            voice->phase[b] += voice->freq[b] / (double)RATE;
            if (voice->phase[b] >= 1) voice->phase[b] -= 1;
            s += voice->amp[b] * sin(2 * M_PI * voice->phase[b]);
        }
        out[i * stride] = (float)(fmax(-1, fmin(1, s)) * gain);
    }
}

// ---- the posted values, the slots and the stream
static PlatMutex lock = PLAT_MUTEX_INIT;   // guards everything below up to `stream`
static HapticsSide latest[2], held[2];     // the newest value per side, and the strongest since the slot began
static float held_raw[2];                  // the game's amplitude sum of held
static int held_set[2];                    // a value was posted since the slot began
static float posted_strength = 1;
static uint64_t posted_at;
static SDL_AudioStream *stream;
static int audio_inited;

static HapticsVoice voices[2];   // the pull side only
static HapticsSide slot[2];
static int slot_left;

void haptics_post(const RumbleHdValue v[2], const int live[2], float strength) {
    plat_mutex_lock(&lock);
    for (int k = 0; k < 2; k++) {
        RumbleHdValue x = v[k];
        if (!live[k]) x.amp_low = x.amp_high = 0;
        HapticsSide side = haptics_side(&x);
        float raw = clampf(x.amp_low, 0, 1) + clampf(x.amp_high, 0, 1);
        latest[k] = side;
        if (!held_set[k] || raw >= held_raw[k]) held[k] = side, held_raw[k] = raw;   // the slot keeps its peak
        held_set[k] = 1;
    }
    posted_strength = strength;
    posted_at = plat_now_ns();
    plat_mutex_unlock(&lock);
}

static void slot_start(void) {
    plat_mutex_lock(&lock);
    int fresh = posted_at && plat_now_ns() - posted_at <= STALE_NS;
    for (int k = 0; k < 2; k++) {
        slot[k] = held_set[k] ? held[k] : latest[k];
        if (!fresh) slot[k].amp[0] = slot[k].amp[1] = 0;
        held[k] = latest[k], held_set[k] = 0, held_raw[k] = 0;
    }
    plat_mutex_unlock(&lock);
    slot_left = SLOT;
}

static void SDLCALL pull(void *userdata, SDL_AudioStream *s, int additional, int total) {
    (void)userdata, (void)total;
    float buf[SLOT * CHANNELS];
    plat_mutex_lock(&lock);
    float gain = fminf(1, CEILING * clampf(posted_strength, 0, 4));
    plat_mutex_unlock(&lock);
    while (additional > 0) {
        if (!slot_left) slot_start();
        int frames = additional / (int)sizeof(float) / CHANNELS;
        if (frames <= 0) frames = 1;
        if (frames > slot_left) frames = slot_left;
        memset(buf, 0, sizeof(float) * (size_t)frames * CHANNELS);   // the speaker channels stay silent
        for (int k = 0; k < 2; k++) haptics_synth(&voices[k], &slot[k], gain, buf + 2 + k, frames, CHANNELS);
        slot_left -= frames;
        SDL_PutAudioStreamData(s, buf, frames * CHANNELS * (int)sizeof(float));
        additional -= frames * CHANNELS * (int)sizeof(float);
    }
}

// The DualSense's playback device: a name with "DualSense" in it and four channels or more. 0 when there is none.
static SDL_AudioDeviceID find(char *name, size_t cap) {
    int n = 0;
    SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&n), found = 0;
    for (int i = 0; ids && i < n && !found; i++) {
        const char *nm = SDL_GetAudioDeviceName(ids[i]);
        SDL_AudioSpec spec;
        int frames;
        if (nm && strstr(nm, "DualSense") && SDL_GetAudioDeviceFormat(ids[i], &spec, &frames) &&
            spec.channels >= CHANNELS) {
            found = ids[i];
            snprintf(name, cap, "%s", nm);
        }
    }
    SDL_free(ids);
    return found;
}

int haptics_open(char *why, size_t cap) {
    plat_mutex_lock(&lock);
    int ok = 0;
    char name[128] = "";
    SDL_AudioDeviceID id = 0;
    if (stream) {
        snprintf(why, cap, "its haptic actuators, already open");
        ok = 1;
    } else if (!audio_inited && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        snprintf(why, cap, "no audio: %s", SDL_GetError());
    } else if (audio_inited = 1, !(id = find(name, sizeof name))) {
        snprintf(why, cap, "no four-channel DualSense audio device (it has one over USB only)");
    } else {
        SDL_AudioSpec spec = {SDL_AUDIO_F32, CHANNELS, RATE};
        memset(voices, 0, sizeof voices);
        memset(slot, 0, sizeof slot);
        slot_left = 0;
        stream = SDL_OpenAudioDeviceStream(id, &spec, pull, NULL);
        if (stream && SDL_ResumeAudioStreamDevice(stream)) {
            snprintf(why, cap, "its haptic actuators, the third and fourth channels of \"%s\"", name);
            ok = 1;
        } else {
            snprintf(why, cap, "its audio device \"%s\" did not open: %s", name, SDL_GetError());
            if (stream) SDL_DestroyAudioStream(stream);
            stream = NULL;
        }
    }
    plat_mutex_unlock(&lock);
    return ok;
}

void haptics_close(void) {
    plat_mutex_lock(&lock);
    SDL_AudioStream *s = stream;
    stream = NULL;
    posted_at = 0;
    plat_mutex_unlock(&lock);
    if (s) SDL_DestroyAudioStream(s);   // waits for a pull in progress; the actuators stop with the stream
}
