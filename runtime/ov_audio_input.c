// Audio input for the nn::audio input-stream calls the game imports (the Ocarina can be played into a microphone):
// each stream reads the default recording device through audio_capture.c, which SDL converts to the format the game
// asks for. The calls are overridden in the game (HOST_OVERRIDE) because the audio service refuses input streams. A
// stream that failed to open is unknown to every call here, so purging or closing it is safe; before, the game purged
// a stream that never opened and crashed. NX2NAVI_MICROPHONE=0 refuses every input stream, as the game saw before.
#include "audio_capture.h"
#include "host.h"
#include "ipc.h"
#include "platform/platform.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define BAD_INPUT 0x70899u   // nn::audio's invalid-argument result (module 153, description 900), as audio_mixer.c
typedef struct {
    uint64_t key;
    MicCapture *mic;
    int format[3], bpf, running;
    uint64_t frames_read, next_log;
} Input;
static Input inputs[8];
static const char input_name[] = "DefaultMicrophone";   // the name the game is given for the default device
static int mic_on(void) {   // NX2NAVI_MICROPHONE (default 1)
    static int on = -1;
    if (on < 0) {
        on = rt_knob("MICROPHONE", "1") != 0;
        if (!on) rt_log("mic: input streams refused (NX2NAVI_MICROPHONE=0)");
    }
    return on;
}
static Input *find(uint64_t key) {
    for (int i = 0; i < 8; i++)
        if (inputs[i].key == key && key)
            return &inputs[i];
    return NULL;
}
static void name_out(uint64_t addr) {
    char *p = ipc_guest(addr, 256);
    memset(p, 0, 256);
    memcpy(p, input_name, sizeof input_name);
}
static void list_devices(Cpu *c) {
    int n = mic_on() ? mic_device_count() : 0;
    if ((int)c->x[1] > 0 && n > 0)
        name_out(c->x[0]);
    rt_log("mic: input enumeration: %d recording devices", n);
    ret_x(c, (int)c->x[1] > 0 && n > 0 ? 1 : 0);
}
static void default_device(Cpu *c) {
    name_out(c->x[0]);
    ret_x(c, 0);
}

static void close_input(Cpu *c) {
    Input *s = find(c->x[0]);
    if (s) {
        rt_log("mic: close, %llu real audio frames delivered", (unsigned long long)s->frames_read);
        mic_close(s->mic);
        memset(s, 0, sizeof *s);
    }
    if (c->x[0]) {
        uint64_t zero = 0;
        memcpy(ipc_guest(c->x[0], 8), &zero, 8);
    }
    ret_x(c, 0);
}
static void open_input(Cpu *c) {
    uint64_t key = c->x[0];
    int f[3];
    memcpy(f, ipc_guest(c->x[4], 12), 12);
    int frames = (int)c->x[5];
    rt_log("mic: OpenInputStream format=%d channels=%d rate=%d capacity=%d", f[0], f[1], f[2], frames);
    Input *s = find(key);
    if (s) {
        mic_close(s->mic);
        memset(s, 0, sizeof *s);
    }
    for (int i = 0; !s && i < 8; i++)
        if (!inputs[i].key)
            s = &inputs[i];
    uint64_t zero = 0;
    memcpy(ipc_guest(key, 8), &zero, 8);
    if (!s || !mic_on()) {
        ret_x(c, BAD_INPUT);
        return;
    }
    MicCapture *m = mic_open(f[0], f[1], f[2], frames);
    if (!m) {
        rt_log("mic: cannot prepare the input stream: %s", mic_error());
        ret_x(c, BAD_INPUT);
        return;
    }
    s->key = key;
    s->mic = m;
    memcpy(s->format, f, 12);
    s->bpf = f[1] * (f[0] == 1 ? 1 : f[0] == 2 ? 2 : 4);
    s->next_log = (uint64_t)f[2] * 5;
    uint64_t impl = key + 8;
    memcpy(ipc_guest(key, 8), &impl, 8);
    rt_log("mic: input stream ready; bounded buffer %d frames; device opens when capture starts", mic_capacity(m));
    ret_x(c, 0);
}
static void start_input(Cpu *c) {
    Input *s = find(c->x[0]);
    if (!s || !mic_start(s->mic)) {
        rt_log("mic: start failed: %s", mic_error());
        ret_x(c, BAD_INPUT);
        return;
    }
    s->running = 1;
    rt_log("mic: capture started");
    ret_x(c, 0);
}
static void stop_input(Cpu *c) {
    Input *s = find(c->x[0]);
    if (s) {
        mic_stop(s->mic);
        if (s->running)
            rt_log("mic: capture stopped; recording device released");
        s->running = 0;
    }
    ret_x(c, 0);
}
static void purge_input(Cpu *c) {
    Input *s = find(c->x[0]);
    if (s)
        mic_purge(s->mic);
    ret_x(c, 0);
}
static void filled(Cpu *c) {
    Input *s = find(c->x[0]);
    ret_x(c, s ? mic_available(s->mic) : 0);
}
static void state(Cpu *c) {
    Input *s = find(c->x[0]);
    ret_x(c, s ? (s->running ? 2 : 1) : 0);
}
static void read_input(Cpu *c) {
    Input *s = find(c->x[0]);
    int frames = (int)c->x[2];
    if (!s || frames <= 0) {
        ret_x(c, 0);
        return;
    }
    int available = mic_available(s->mic);
    if (frames > available)
        frames = available;
    if (!frames) {
        ret_x(c, 0);
        return;
    }
    int n = mic_read(s->mic, ipc_guest(c->x[1], (uint64_t)frames * s->bpf), frames);
    s->frames_read += n;
    if (s->frames_read >= s->next_log) {
        rt_log("mic: delivered %llu real frames (%d Hz)", (unsigned long long)s->frames_read, s->format[2]);
        s->next_log = s->frames_read + (uint64_t)s->format[2] * 30;
    }
    ret_x(c, n);
}
static void statistics(Cpu *c) { memset(ipc_guest(c->x[8], 24), 0, 24); }

static PlatMutex input_lock = PLAT_MUTEX_INIT;
#define LOCKED_HANDLER(fn)                                                                                             \
    static void fn##_locked(Cpu *c) {                                                                                  \
        plat_mutex_lock(&input_lock);                                                                                  \
        fn(c);                                                                                                         \
        plat_mutex_unlock(&input_lock);                                                                                \
    }
LOCKED_HANDLER(list_devices)
LOCKED_HANDLER(default_device)
LOCKED_HANDLER(open_input)
LOCKED_HANDLER(close_input)
LOCKED_HANDLER(start_input)
LOCKED_HANDLER(stop_input)
LOCKED_HANDLER(purge_input)
LOCKED_HANDLER(filled)
LOCKED_HANDLER(state)
LOCKED_HANDLER(read_input)
LOCKED_HANDLER(statistics)

HOST_OVERRIDE("_ZN2nn5audio23ListVirtualInputDevicesEPNS0_15AudioDeviceNameEi", list_devices_locked);
HOST_OVERRIDE("_ZN2nn5audio28GetDefaultVirtualInputDeviceEPNS0_15AudioDeviceNameE", default_device_locked);
HOST_OVERRIDE("_ZN2nn5audio26GetTrueRawAudioInputDeviceEPNS0_15AudioDeviceNameE", default_device_locked);
HOST_OVERRIDE("_ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEi",
              open_input_locked);
HOST_OVERRIDE("_ZN2nn5audio16CloseInputStreamEPNS0_15InputStreamTypeE", close_input_locked);
HOST_OVERRIDE("_ZN2nn5audio16StartInputStreamEPNS0_15InputStreamTypeE", start_input_locked);
HOST_OVERRIDE("_ZN2nn5audio15StopInputStreamEPNS0_15InputStreamTypeE", stop_input_locked);
HOST_OVERRIDE("_ZN2nn5audio16PurgeInputStreamEPNS0_15InputStreamTypeE", purge_input_locked);
HOST_OVERRIDE("_ZN2nn5audio30GetInputStreamFilledFrameCountEPKNS0_15InputStreamTypeE", filled_locked);
HOST_OVERRIDE("_ZN2nn5audio19GetInputStreamStateEPKNS0_15InputStreamTypeE", state_locked);
HOST_OVERRIDE("_ZN2nn5audio15ReadInputStreamEPNS0_15InputStreamTypeEPvi", read_input_locked);
HOST_OVERRIDE("_ZN2nn5audio24GetInputStreamStatisticsEPNS0_15InputStreamTypeEb", statistics_locked);
