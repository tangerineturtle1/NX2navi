// DualSense haptics (runtime/rumble_haptics.c) without a controller: the Switch 2 amplitude curve, one side's target
// (square root, the bands scaled down together, frequency limits), the sines a target makes (frequency, level, the
// output ceiling, a glide without a jump), the slot's peak hold that keeps a tick of one update, and a side that stops
// being renewed falling silent. The audio device is never opened. Built by tests/rumble_haptics_check.py.
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "rumble_haptics.c"

static uint64_t clock_ns = 1000000000ull;
uint64_t plat_now_ns(void) { return clock_ns; }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }

static int near(double a, double b, double tol) { return fabs(a - b) <= tol; }

// Plays n frames of one side's target from a fresh voice: peak and the number of upward zero crossings.
static void measure(const HapticsSide *t, float gain, int n, float *peak, int *crossings) {
    static float out[48000];
    HapticsVoice v = {{0, 0}, {t->amp[0], t->amp[1]}, {t->freq[0], t->freq[1]}};   // settled: no glide
    haptics_synth(&v, t, gain, out, n, 1);
    *peak = 0, *crossings = 0;
    for (int i = 0; i < n; i++) {
        if (fabsf(out[i]) > *peak) *peak = fabsf(out[i]);
        if (i && out[i - 1] < 0 && out[i] >= 0) ++*crossings;
    }
}

int main(void) {
    // the curve: nothing at or below 0.001, 0.02 lifted to about 0.1088, the points at 0.1, 0.5 and 1
    assert(haptics_lift(0.0005f) == 0 && haptics_lift(0.001f) == 0 && haptics_lift(NAN) == 0);
    assert(near(haptics_lift(0.02f), 0.1088, 0.001));
    assert(near(haptics_lift(0.1f), 0.55, 1e-5) && near(haptics_lift(0.5f), 0.9, 1e-5) && haptics_lift(1) == 1);
    for (float a = 0.0011f; a < 1; a += 0.01f) assert(haptics_lift(a + 0.005f) >= haptics_lift(a));   // never falls

    // one side: the square root of the lifted amplitude; two loud bands scaled down to full scale together
    RumbleHdValue small = {0.02f, 160, 0, 320};
    small = haptics_lifted(&small);
    HapticsSide s = haptics_side(&small);
    assert(near(s.amp[0], sqrt(0.1088), 0.002) && s.amp[1] == 0 && s.freq[0] == 160);
    RumbleHdValue loud = {1, 100, 1, 600};
    s = haptics_side(&loud);
    assert(near(s.amp[0], 0.5, 1e-5) && near(s.amp[1], 0.5, 1e-5));
    RumbleHdValue odd = {0.5f, 5, 0.1f, 3000};   // frequencies outside 20..1000 are limited; unset ones are 160 / 320
    s = haptics_side(&odd);
    assert(s.freq[0] == 20 && s.freq[1] == 1000);
    RumbleHdValue unset = {0.5f, 0, 0.5f, -1};
    s = haptics_side(&unset);
    assert(s.freq[0] == 160 && s.freq[1] == 320);

    // the sines: a 160 Hz band crosses zero 160 times a second; the output never passes the gain
    HapticsSide tone = {{1, 0}, {160, 320}};
    float peak;
    int cross;
    measure(&tone, 0.8f, 48000, &peak, &cross);
    assert(near(cross, 160, 1) && near(peak, 0.8, 0.01));
    HapticsSide both = {{0.5f, 0.5f}, {100, 300}};
    measure(&both, 0.8f, 48000, &peak, &cross);
    assert(peak <= 0.8f + 1e-6);
    // from silence, the level glides up: no jump in the first samples (4 ms time constant)
    {
        HapticsVoice v = {{0, 0}, {0, 0}, {160, 320}};
        float out[48];
        haptics_synth(&v, &tone, 0.8f, out, 48, 1);   // 1 ms
        for (int i = 0; i < 48; i++) assert(fabsf(out[i]) < 0.25f);
    }

    // a vibration's start: two slots at 1.41 times, six at 0.71 times, then as it is; silence starts it over; a boost
    // keeps two loud bands within full scale
    {
        int since = -1;
        float want[] = {1.41f, 1.41f, 0.71f, 0.71f, 0.71f, 0.71f, 0.71f, 0.71f, 1, 1};
        for (int i = 0; i < 10; i++) {
            RumbleHdValue x = {0.4f, 160, 0, 320};
            haptics_crisp(&since, &x);
            assert(near(x.amp_low, 0.4 * want[i], 1e-5));
        }
        RumbleHdValue quiet = {0, 160, 0, 320}, x = {0.4f, 160, 0, 320};
        haptics_crisp(&since, &quiet);
        haptics_crisp(&since, &x);
        assert(since == 1 && near(x.amp_low, 0.4 * 1.41, 1e-5));
        since = -1;
        RumbleHdValue big = {0.6f, 160, 0.6f, 320};
        haptics_crisp(&since, &big);
        assert(near(big.amp_low + big.amp_high, 1.2, 1e-5));   // the sum stays at its own 1.2, not 1.69
    }

    // the slot's peak hold: a tick followed at once by zero, both posted within one slot, is what the next slot plays
    RumbleHdValue tick[2] = {{0.05f, 160, 0, 320}, {0.05f, 160, 0, 320}}, zero[2] = {{0, 160, 0, 320}, {0, 160, 0, 320}};
    int live[2] = {1, 1};
    slot_start();   // a slot begins
    haptics_post(tick, live, 1);
    clock_ns += 5000000;
    haptics_post(zero, live, 1);
    slot_start();   // the next slot plays the peak of the last one
    assert(slot[0].amp[0] > 0.5f && slot[1].amp[0] > 0.5f);
    slot_start();   // and the one after that the newest value, zero
    assert(slot[0].amp[0] == 0 && slot[1].amp[0] == 0);
    // a side at rest (live 0) is silent even with a value
    int rest[2] = {1, 0};
    haptics_post(tick, rest, 1);
    slot_start();
    assert(slot[0].amp[0] > 0.5f && slot[1].amp[0] == 0);

    // the game stops renewing the value: after 250 ms the slots are silent
    haptics_post(tick, live, 1);
    slot_start();
    assert(slot[0].amp[0] > 0.5f);
    clock_ns += 300000000ull;
    slot_start();
    assert(slot[0].amp[0] == 0 && slot[1].amp[0] == 0);

    puts("rumble_haptics_check: ok");
    return 0;
}
