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
// past full scale. The output never goes past 0.8 of full scale, to spare the coils. Between the curve and the root, a
// vibration's start is sharpened (haptics_crisp): its first two slots play 1.41 times the value (+3 dB), the next six
// 0.71 times (-3 dB), then the value itself, so a hit is felt as a hit and not as a swell.
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

RumbleHdValue haptics_lifted(const RumbleHdValue *v) {
    RumbleHdValue r = *v;
    r.amp_low = haptics_lift(clampf(v->amp_low, 0, 1));
    r.amp_high = haptics_lift(clampf(v->amp_high, 0, 1));
    return r;
}

void haptics_crisp(int *since, RumbleHdValue *v) {
    enum { ACCENT_SLOTS = 2, DIP_SLOTS = 6 };
    if (v->amp_low * 65535 < 1 && v->amp_high * 65535 < 1) {   // silent: the next sound is a new start
        *since = -1;
        return;
    }
    if (*since < 0) *since = 0;
    float gain = *since < ACCENT_SLOTS ? 1.41f : *since < ACCENT_SLOTS + DIP_SLOTS ? 0.71f : 1;
    if (*since < ACCENT_SLOTS + DIP_SLOTS) ++*since;
    if (gain == 1) return;
    float lo = fminf(1, v->amp_low * gain), hi = fminf(1, v->amp_high * gain);
    if (gain > 1) {   // a boost keeps the sum within the larger of 1 and the value's own sum
        float limit = fmaxf(1, v->amp_low + v->amp_high), sum = lo + hi;
        if (sum > limit) lo *= limit / sum, hi *= limit / sum;
    }
    v->amp_low = lo, v->amp_high = hi;
}

HapticsSide haptics_side(const RumbleHdValue *v) {
    float lo = sqrtf(clampf(v->amp_low, 0, 1)), hi = sqrtf(clampf(v->amp_high, 0, 1));
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
static RumbleHdValue latest[2], held[2];   // lifted: the newest value per side, the strongest since the slot began
static int held_set[2];                    // a value was posted since the slot began
static float posted_strength = 1;
static uint64_t posted_at;
static SDL_AudioStream *stream;
static int audio_inited;

static HapticsVoice voices[2];   // the pull side only
static HapticsSide slot[2];
static int slot_left;
static int since_onset[2] = {-1, -1};   // per side: slots since the vibration started (haptics_crisp), -1 silent

void haptics_post(const RumbleHdValue v[2], const int live[2], float strength) {
    plat_mutex_lock(&lock);
    for (int k = 0; k < 2; k++) {
        RumbleHdValue x = v[k];
        if (!live[k]) x.amp_low = x.amp_high = 0;
        x = haptics_lifted(&x);
        latest[k] = x;
        if (!held_set[k] || x.amp_low + x.amp_high >= held[k].amp_low + held[k].amp_high) held[k] = x;   // the peak
        held_set[k] = 1;
    }
    posted_strength = strength;
    posted_at = plat_now_ns();
    plat_mutex_unlock(&lock);
}

static void slot_start(void) {
    plat_mutex_lock(&lock);
    int fresh = posted_at && plat_now_ns() - posted_at <= STALE_NS;
    RumbleHdValue play[2];
    for (int k = 0; k < 2; k++) {
        play[k] = held_set[k] ? held[k] : latest[k];
        if (!fresh) play[k].amp_low = play[k].amp_high = 0;
        held[k] = latest[k], held_set[k] = 0;
    }
    plat_mutex_unlock(&lock);
    for (int k = 0; k < 2; k++) {
        haptics_crisp(&since_onset[k], &play[k]);
        slot[k] = haptics_side(&play[k]);
    }
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

// The DualSense's playback device: a name with "DualSense" or "Wireless Controller" in it (Windows uses either) and four
// channels or more. 0 when there is none.
static SDL_AudioDeviceID find(char *name, size_t cap) {
    int n = 0;
    SDL_AudioDeviceID *ids = SDL_GetAudioPlaybackDevices(&n), found = 0;
    for (int i = 0; ids && i < n && !found; i++) {
        const char *nm = SDL_GetAudioDeviceName(ids[i]);
        SDL_AudioSpec spec;
        int frames;
        if (nm && (strstr(nm, "DualSense") || strstr(nm, "Wireless Controller")) && SDL_GetAudioDeviceFormat(ids[i], &spec, &frames) &&
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
        for (int k = 0; k < 2; k++) voices[k].freq[0] = 160, voices[k].freq[1] = 320;   // the bands' rest frequencies
        memset(slot, 0, sizeof slot);
        slot_left = 0;
        since_onset[0] = since_onset[1] = -1;
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
