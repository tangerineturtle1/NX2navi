#include "audio_capture.h"
#include <SDL3/SDL.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct MicCapture {
    SDL_AudioStream *stream;
    SDL_AudioDeviceID device;
    unsigned char *ring;
    int cap, head, used, bpf;
};

const char *mic_error(void) { return SDL_GetError(); }
int mic_device_count(void) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        return 0;
    int n = 0;
    SDL_AudioDeviceID *ids = SDL_GetAudioRecordingDevices(&n);
    SDL_free(ids);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return n;
}

/* The SDL stream lock serializes the callback and every ring operation.
   Discard the oldest frames on overflow, so a paused game cannot grow RAM. */
static void SDLCALL received(void *userdata, SDL_AudioStream *stream, int added, int total) {
    (void)added;
    (void)total;
    MicCapture *m = userdata;
    unsigned char scratch[8192];
    int chunk = (int)sizeof scratch / m->bpf * m->bpf;
    for (;;) {
        int n = SDL_GetAudioStreamData(stream, scratch, chunk);
        if (n <= 0)
            break;
        n -= n % m->bpf;
        if (!n)
            break;
        const unsigned char *src = scratch;
        if (n > m->cap) {
            src += n - m->cap;
            n = m->cap;
        }
        int drop = m->used + n - m->cap;
        if (drop > 0) {
            m->head = (m->head + drop) % m->cap;
            m->used -= drop;
        }
        int tail = (m->head + m->used) % m->cap;
        int first = n < m->cap - tail ? n : m->cap - tail;
        memcpy(m->ring + tail, src, first);
        memcpy(m->ring, src + first, n - first);
        m->used += n;
    }
}

MicCapture *mic_open(int format, int channels, int rate, int frames) {
    SDL_AudioFormat sf;
    switch (format) {
    case 1:
        sf = SDL_AUDIO_S8;
        break;
    case 2:
        sf = SDL_AUDIO_S16LE;
        break;
    case 4:
        sf = SDL_AUDIO_S32LE;
        break;
    case 5:
        sf = SDL_AUDIO_F32LE;
        break;
    default:
        SDL_SetError("unsupported guest sample format %d", format);
        return NULL;
    }
    if (channels < 1 || channels > 8 || rate < 8000 || rate > 192000 || frames < 1) {
        SDL_SetError("invalid capture format or capacity");
        return NULL;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
        return NULL;
    MicCapture *m = calloc(1, sizeof *m);
    if (!m) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return NULL;
    }
    m->bpf = channels * (SDL_AUDIO_BITSIZE(sf) / 8);
    if (frames > rate * 2)
        frames = rate * 2;
    m->cap = frames * m->bpf;
    m->ring = malloc(m->cap);
    if (!m->ring) {
        free(m);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return NULL;
    }
    SDL_AudioSpec spec = {.format = sf, .channels = channels, .freq = rate};
    SDL_AudioSpec source;
    if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &source, NULL))
        m->stream = SDL_CreateAudioStream(&source, &spec);
    if (!m->stream || !SDL_SetAudioStreamPutCallback(m->stream, received, m)) {
        mic_close(m);
        return NULL;
    }
    return m;
}
int mic_start(MicCapture *m) {
    if (!m)
        return 0;
    if (m->device)
        return 1;
    SDL_AudioSpec spec;
    if (!SDL_GetAudioStreamFormat(m->stream, NULL, &spec))
        return 0;
    SDL_AudioDeviceID device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec);
    if (!device)
        return 0;
    if (!SDL_BindAudioStream(device, m->stream)) {
        SDL_CloseAudioDevice(device);
        return 0;
    }
    m->device = device;
    return 1;
}
void mic_stop(MicCapture *m) {
    if (m && m->device) {
        SDL_CloseAudioDevice(m->device); /* unbinds the stream; unread frames remain in the ring */
        m->device = 0;
    }
}
void mic_close(MicCapture *m) {
    if (!m)
        return;
    mic_stop(m);
    if (m->stream)
        SDL_DestroyAudioStream(m->stream);
    free(m->ring);
    free(m);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}
void mic_purge(MicCapture *m) {
    if (!m)
        return;
    SDL_LockAudioStream(m->stream);
    SDL_ClearAudioStream(m->stream);
    m->head = m->used = 0;
    SDL_UnlockAudioStream(m->stream);
}
int mic_available(MicCapture *m) {
    if (!m)
        return 0;
    SDL_LockAudioStream(m->stream);
    int frames = m->used / m->bpf;
    SDL_UnlockAudioStream(m->stream);
    return frames;
}
int mic_capacity(MicCapture *m) { return m ? m->cap / m->bpf : 0; }
int mic_read(MicCapture *m, void *out, int frames) {
    if (!m || !out || frames <= 0)
        return 0;
    SDL_LockAudioStream(m->stream);
    int avail = m->used / m->bpf;
    if (frames > avail)
        frames = avail;
    int n = frames * m->bpf;
    int first = n < m->cap - m->head ? n : m->cap - m->head;
    memcpy(out, m->ring + m->head, first);
    memcpy((unsigned char *)out + first, m->ring, n - first);
    m->head = (m->head + n) % m->cap;
    m->used -= n;
    SDL_UnlockAudioStream(m->stream);
    return frames;
}
void mic_gain(MicCapture *m, float gain) {
    if (m)
        SDL_SetAudioStreamGain(m->stream, gain);
}
