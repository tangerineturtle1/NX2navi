// HD rumble for controllers with linear actuators: the encoding of a vibration value and the report schedule
// (rumble_hd.h). The byte layout and the step tables are those of the Switch controllers' rumble output report.
//
// One actuator's value is four bytes, for a high band (hf, 9 bits; its amplitude ha, 7 bits) and a low band (lf,
// 7 bits; its amplitude la, 7 bits):
//   byte 0  hf bits 0-7
//   byte 1  hf bit 8 in bit 0, ha in bits 1-7
//   byte 2  lf in bits 0-6, la bit 0 in bit 7
//   byte 3  0x40 + la bits 1-6
// Frequencies are steps of 1/32 octave above 10 Hz: n = round(32 * log2(f / 10)). The low band takes n 0x41..0xBF
// (40.9 to 626.3 Hz) as lf = n - 0x40, the high band n 0x61..0xDF (81.8 to 1252.6 Hz) as hf = (n - 0x60) * 4.
// Amplitudes are steps 0..100 on a scale that is finer where the amplitude is larger (rumble_hd_step_amp), written to
// both bands as ha = la = step. Steps above 100 exist in the format and drive the actuator past what it is built for;
// rumble_hd_amp_step never returns one.
#include <math.h>
#include <string.h>

#include "rumble_hd.h"

const uint8_t rumble_hd_neutral[RUMBLE_HD_DATA] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};

// The amplitude scale: step 0 is off, step 1 the smallest amplitude there is, steps 2 to 15 a quarter octave apart,
// 16 to 31 a sixteenth, 32 to 100 a thirty-second, ending at 1.0.
#define AMP_STEP1 0.007843f
#define AMP_STEP2 0.011823f

float rumble_hd_step_amp(int step) {
    if (step <= 0) return 0;
    if (step == 1) return AMP_STEP1;
    if (step < 16) return AMP_STEP2 * powf(2, (float)(step - 2) / 4);
    if (step < 32) return powf(2, (float)step / 16) / 17;
    if (step > RUMBLE_HD_AMP_STEPS) step = RUMBLE_HD_AMP_STEPS;
    return powf(2, (float)step / 32) / 8.7f;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int rumble_hd_amp_step(float a) {
    if (!(a >= AMP_STEP1 / 2)) return 0;   // also a NaN
    if (a >= 1) return RUMBLE_HD_AMP_STEPS;
    // The three ranges meet half way (on the log scale) between their neighbouring steps: 1 and 2, 15 and 16, 31 and 32.
    if (a < 0.00963f) return 1;
    if (a < 0.1150f) return clampi(2 + (int)lroundf(4 * log2f(a / AMP_STEP2)), 2, 15);
    if (a < 0.2276f) return clampi((int)lroundf(16 * log2f(a * 17)), 16, 31);
    return clampi((int)lroundf(32 * log2f(a * 8.7f)), 32, RUMBLE_HD_AMP_STEPS);
}

// The frequency step of f Hz within lo..hi (step numbers); a NaN or a frequency at or below 0 is the lowest.
static int freq_step(float f, int lo, int hi) {
    if (!(f > 0)) return lo;
    float n = 32 * log2f(f / 10);
    return n <= (float)lo ? lo : n >= (float)hi ? hi : clampi((int)lroundf(n), lo, hi);
}

void rumble_hd_encode(const RumbleHdValue *v, float strength, uint8_t out[RUMBLE_HD_SIDE]) {
    float s = strength > 0 ? strength : 0;
    int la = rumble_hd_amp_step(v->amp_low * s), ha = rumble_hd_amp_step(v->amp_high * s);
    if (!la && !ha) { memcpy(out, rumble_hd_neutral, RUMBLE_HD_SIDE); return; }
    int lf = freq_step(v->freq_low, 0x41, 0xBF) - 0x40, hf = (freq_step(v->freq_high, 0x61, 0xDF) - 0x60) * 4;
    out[0] = (uint8_t)(hf & 0xff);
    out[1] = (uint8_t)((hf >> 8) | ha << 1);
    out[2] = (uint8_t)(lf | (la & 1) << 7);
    out[3] = (uint8_t)(0x40 + (la >> 1));
}

void rumble_hd_data(const RumbleHdValue v[2], const int live[2], float strength, int one, uint8_t out[RUMBLE_HD_DATA]) {
    memcpy(out, rumble_hd_neutral, RUMBLE_HD_DATA);
    float power[2] = {0, 0};
    for (int s = 0; s < 2; s++) {
        if (!live[s]) continue;
        rumble_hd_encode(&v[s], strength, out + s * RUMBLE_HD_SIDE);
        // how strong the side is, for a controller that can play only one: the two bands' amplitudes as energy
        float al = v[s].amp_low > 0 ? v[s].amp_low : 0, ah = v[s].amp_high > 0 ? v[s].amp_high : 0;
        power[s] = al * al + ah * ah;
    }
    if (!one) return;
    int pick = power[1] > power[0];
    memcpy(out + (1 - pick) * RUMBLE_HD_SIDE, out + pick * RUMBLE_HD_SIDE, RUMBLE_HD_SIDE);
}

void rumble_hd_report(const uint8_t data[RUMBLE_HD_DATA], unsigned number, uint8_t *out, unsigned n) {
    memset(out, 0, n);
    out[0] = RUMBLE_HD_REPORT_ID;
    out[1] = (uint8_t)(number & 0xf);
    memcpy(out + 2, data, RUMBLE_HD_DATA);
}

static int is_neutral(const uint8_t d[RUMBLE_HD_DATA]) { return !memcmp(d, rumble_hd_neutral, RUMBLE_HD_DATA); }

// What the controller is to play at `now`: the game's data while it is fresh and the path is on, else the neutral value.
static const uint8_t *target(const RumbleHd *h, int on, uint64_t now) {
    return on && h->have && now - h->want_at <= RUMBLE_HD_STALE_NS ? h->want : rumble_hd_neutral;
}

void rumble_hd_post(RumbleHd *h, const uint8_t data[RUMBLE_HD_DATA], uint64_t now) {
    memcpy(h->want, data, RUMBLE_HD_DATA);
    h->want_at = now;
    h->have = 1;
}

int rumble_hd_due(const RumbleHd *h, int on, uint64_t now, uint64_t gap_ns, uint8_t out[RUMBLE_HD_DATA]) {
    const uint8_t *t = target(h, on, now);
    int due;
    if (!h->any) due = !is_neutral(t);   // a controller that got nothing yet is at rest already
    else if (memcmp(t, h->sent, RUMBLE_HD_DATA)) due = now - h->sent_at >= gap_ns;
    else if (is_neutral(t)) due = h->neutral_left > 0 && now - h->sent_at >= gap_ns;
    else due = now - h->sent_at >= RUMBLE_HD_REFRESH_NS;
    if (due) memcpy(out, t, RUMBLE_HD_DATA);
    return due;
}

void rumble_hd_sent(RumbleHd *h, const uint8_t data[RUMBLE_HD_DATA], uint64_t now) {
    if (!is_neutral(data)) h->neutral_left = 0;
    else if (!h->any || !is_neutral(h->sent)) h->neutral_left = RUMBLE_HD_NEUTRAL_REPORTS - 1;
    else if (h->neutral_left > 0) h->neutral_left--;
    memcpy(h->sent, data, RUMBLE_HD_DATA);
    h->sent_at = now;
    h->any = 1;
}

int rumble_hd_idle(const RumbleHd *h, int on, uint64_t now) {
    if (!is_neutral(target(h, on, now))) return 0;
    return !h->any || (is_neutral(h->sent) && h->neutral_left == 0);
}

int rumble_hd_buzzing(const RumbleHd *h) { return h->any && !is_neutral(h->sent); }

void rumble_hd_forget(RumbleHd *h) {
    h->any = 0;
    h->neutral_left = 0;
    h->sent_at = 0;
    memset(h->sent, 0, sizeof h->sent);
}
