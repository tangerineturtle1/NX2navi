// HD rumble: the game's vibration values for a controller with linear actuators (Switch Pro Controller, Joy-Con), as
// the bytes of the controller's rumble output report, and the decision when a report is written. Such an actuator takes
// a frequency and an amplitude for a low and a high band, which is what the game sends (ov_input.c VibrationValue), so
// nothing is reduced to motor strengths here. Pure: no SDL and no platform calls; input.c (hd_tick) writes what this
// file decides. Checked by tests/rumble_hd_check.py.
#pragma once
#include <stdint.h>

// One actuator's value as the game sends it: amplitudes 0..1, frequencies in Hz.
typedef struct { float amp_low, freq_low, amp_high, freq_high; } RumbleHdValue;

enum {
    RUMBLE_HD_SIDE = 4,          // bytes of one actuator's value in a report
    RUMBLE_HD_DATA = 8,          // ... and of a report's two: the left actuator's, then the right one's
    RUMBLE_HD_REPORT_ID = 0x10,  // the output report that carries nothing but rumble data
    RUMBLE_HD_REPORT = 10,       // its bytes that count: the id, the report's number, the data
    RUMBLE_HD_AMP_STEPS = 100,   // the highest amplitude step written (amplitude 1.0): the steps above it overdrive the actuator
    RUMBLE_HD_NEUTRAL_REPORTS = 3,   // how many times in a row the neutral value is written when the vibration ends
};
// The frequencies an actuator takes: the low band's and the high band's ends, in Hz. Others are clamped to them.
#define RUMBLE_HD_LOW_MIN 40.875885f
#define RUMBLE_HD_LOW_MAX 626.286133f
#define RUMBLE_HD_HIGH_MIN 81.75177f
#define RUMBLE_HD_HIGH_MAX 1252.572266f
#define RUMBLE_HD_STALE_NS 250000000ull     // values the game has not renewed for this long count as neutral
#define RUMBLE_HD_REFRESH_NS 30000000ull    // a value that still holds is written again this often
#define RUMBLE_HD_GAP_NS 15000000ull        // the default time between two reports (rumble_hd_due's gap)

// The value of an actuator at rest (no amplitude, 160 Hz and 320 Hz), twice.
extern const uint8_t rumble_hd_neutral[RUMBLE_HD_DATA];

// The amplitude step (0..RUMBLE_HD_AMP_STEPS) nearest to a, and that step's amplitude. A NaN or a value below half of
// the first step is step 0; 1 and everything above it is the last step.
int rumble_hd_amp_step(float a);
float rumble_hd_step_amp(int step);

// One actuator's value as its four report bytes. strength scales both amplitudes (1 = as the game sent them) before
// they are clamped to 0..1. With both amplitudes at step 0 the bytes are the neutral value's, whatever the frequencies.
void rumble_hd_encode(const RumbleHdValue *v, float strength, uint8_t out[RUMBLE_HD_SIDE]);

// A report's data from the game's newest value per side (v[0] left, v[1] right). live[s] 0: that side is at rest
// (never sent, stale, or vibration not permitted). one: the controller has a single actuator (one Joy-Con): the
// stronger side's value goes into both halves.
void rumble_hd_data(const RumbleHdValue v[2], const int live[2], float strength, int one, uint8_t out[RUMBLE_HD_DATA]);

// The output report: id, number (0..15, going round), data, zeros up to n bytes (n >= RUMBLE_HD_REPORT).
void rumble_hd_report(const uint8_t data[RUMBLE_HD_DATA], unsigned number, uint8_t *out, unsigned n);

// What one controller is to get and what it got. All zero at the start.
typedef struct {
    uint8_t want[RUMBLE_HD_DATA];   // the newest data from the game (rumble_hd_post) ...
    uint64_t want_at;               // ... and when it came
    int have;                       // ... once there is one
    uint8_t sent[RUMBLE_HD_DATA];   // the data of the last report written (rumble_hd_sent) ...
    uint64_t sent_at;               // ... and when
    int any;                        // ... once there is one
    int neutral_left;               // neutral reports still to write after the last one
} RumbleHd;

// The game's newest data: it replaces what was not written yet (nothing is queued).
void rumble_hd_post(RumbleHd *h, const uint8_t data[RUMBLE_HD_DATA], uint64_t now);
// 1 when a report is to be written at `now`, with its data in out. on 0: the controller gets the neutral value whatever
// the game sent (the setting is off, the process ends). A changed value waits until gap_ns have passed since the last
// report; a value that still holds is written again every RUMBLE_HD_REFRESH_NS, since the actuators stop by themselves
// when no report comes; data older than RUMBLE_HD_STALE_NS is neutral; the neutral value is written
// RUMBLE_HD_NEUTRAL_REPORTS times, gap_ns apart, and then nothing until the game posts something else.
int rumble_hd_due(const RumbleHd *h, int on, uint64_t now, uint64_t gap_ns, uint8_t out[RUMBLE_HD_DATA]);
// The report with data was written at `now`.
void rumble_hd_sent(RumbleHd *h, const uint8_t data[RUMBLE_HD_DATA], uint64_t now);
// 1 when nothing is to be written until the next rumble_hd_post (or until `on` changes).
int rumble_hd_idle(const RumbleHd *h, int on, uint64_t now);
// 1 while the last report written was not the neutral value: the actuators may still be running.
int rumble_hd_buzzing(const RumbleHd *h);
// Another controller takes the reports: what was written no longer counts, the game's newest data stays.
void rumble_hd_forget(RumbleHd *h);
