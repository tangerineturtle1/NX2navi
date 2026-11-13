// runtime/rumble_hd.c: a vibration value as the bytes of a linear actuator's rumble report (known values, the clamps,
// the neutral value), a report's data from the two sides, and which reports are written when the game sends a batch.
// Built and run by tests/rumble_hd_check.py; prints each failed check and exits 1 on any. With --sweep it prints
// values and their bytes, one per line, for the script to decode with formulae of its own.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rumble_hd.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int enc_is(float al, float fl, float ah, float fh, float strength, uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3,
                  uint8_t got[4]) {
    RumbleHdValue v = {al, fl, ah, fh};
    rumble_hd_encode(&v, strength, got);
    return got[0] == b0 && got[1] == b1 && got[2] == b2 && got[3] == b3;
}
#define ENC(al, fl, ah, fh, st, b0, b1, b2, b3) do { uint8_t g_[4]; \
    CHECK(enc_is(al, fl, ah, fh, st, b0, b1, b2, b3, g_), "%g@%g Hz %g@%g Hz x%g gave %02x %02x %02x %02x, expected %02x %02x %02x %02x", \
          (double)(al), (double)(fl), (double)(ah), (double)(fh), (double)(st), g_[0], g_[1], g_[2], g_[3], b0, b1, b2, b3); } while (0)

static void sweep(void) {
    static const float amps[] = {0, 0.001f, 0.0038f, 0.004f, 0.006f, 0.0078f, 0.0095f, 0.0098f, 0.012f, 0.02f, 0.05f, 0.09f,
                                 0.113f, 0.116f, 0.15f, 0.2f, 0.226f, 0.229f, 0.25f, 0.3f, 0.4f, 0.5f, 0.66f, 0.8f, 0.9f,
                                 0.99f, 1, 1.3f, 4};
    for (size_t a = 0; a < sizeof amps / sizeof *amps; a++)
        for (size_t b = 0; b < sizeof amps / sizeof *amps; b += 3)
            for (float f = 20; f < 3000; f *= 1.0717f) {
                RumbleHdValue v = {amps[a], f, amps[b], f * 1.9f};
                uint8_t o[4];
                rumble_hd_encode(&v, 1, o);
                printf("%.9g %.9g %.9g %.9g %u %u %u %u\n", (double)v.amp_low, (double)v.freq_low, (double)v.amp_high,
                       (double)v.freq_high, o[0], o[1], o[2], o[3]);
            }
}

#define MS 1000000ull

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--sweep")) { sweep(); return 0; }

    // ---- known values
    // The neutral value, as the game sends it and as the controller expects it.
    ENC(0, 160, 0, 320, 1, 0x00, 0x01, 0x40, 0x40);
    CHECK(!memcmp(rumble_hd_neutral, "\x00\x01\x40\x40\x00\x01\x40\x40", 8), "the neutral data is not 00 01 40 40 twice");
    // Both bands at 150 Hz and amplitude 0.9: frequency step 125 (low 0x3d, high 0x74), amplitude step 95.
    ENC(0.9f, 150, 0.9f, 150, 1, 0x74, 0xbe, 0xbd, 0x6f);
    // Full amplitude at the neutral frequencies: amplitude step 100, the last one.
    ENC(1, 160, 1, 320, 1, 0x00, 0xc9, 0x40, 0x72);
    // Amplitude 0.5 is step 68; one band alone leaves the other at step 0.
    ENC(0.5f, 160, 0, 320, 1, 0x00, 0x01, 0x40, 0x62);
    ENC(0, 160, 0.5f, 320, 1, 0x00, 0x89, 0x40, 0x40);
    // An odd amplitude step puts its low bit on top of the low frequency byte: 0.245 is step 35.
    ENC(0.245f, 160, 0.245f, 320, 1, 0x00, 0x47, 0xc0, 0x51);

    // ---- the clamps
    // Frequencies outside a band go to its ends: low 1..0x7f, high 4..0x1fc.
    ENC(0.5f, 10, 0.5f, 5000, 1, 0xfc, 0x89, 0x01, 0x62);
    ENC(0.5f, 5000, 0.5f, 10, 1, 0x04, 0x88, 0x7f, 0x62);
    ENC(0.5f, RUMBLE_HD_LOW_MIN, 0.5f, RUMBLE_HD_HIGH_MIN, 1, 0x04, 0x88, 0x01, 0x62);
    ENC(0.5f, RUMBLE_HD_LOW_MAX, 0.5f, RUMBLE_HD_HIGH_MAX, 1, 0xfc, 0x89, 0x7f, 0x62);
    ENC(0.5f, 0, 0.5f, -3, 1, 0x04, 0x88, 0x01, 0x62);
    ENC(0.5f, NAN, 0.5f, NAN, 1, 0x04, 0x88, 0x01, 0x62);
    // Amplitudes above 1, by themselves or through the strength, stop at step 100; the strength scales below it.
    ENC(7, 160, 7, 320, 1, 0x00, 0xc9, 0x40, 0x72);
    ENC(0.5f, 160, 0.5f, 320, 3, 0x00, 0xc9, 0x40, 0x72);
    ENC(1, 160, 1, 320, 0.5f, 0x00, 0x89, 0x40, 0x62);
    ENC(INFINITY, 160, INFINITY, 320, 1, 0x00, 0xc9, 0x40, 0x72);
    // No amplitude is the neutral value whatever the frequencies say: negative, NaN, too small, strength 0.
    ENC(0, 55, 0, 1000, 1, 0x00, 0x01, 0x40, 0x40);
    ENC(-1, 55, -0.5f, 1000, 1, 0x00, 0x01, 0x40, 0x40);
    ENC(NAN, 55, NAN, 1000, 1, 0x00, 0x01, 0x40, 0x40);
    ENC(0.003f, 55, 0.001f, 1000, 1, 0x00, 0x01, 0x40, 0x40);
    ENC(1, 55, 1, 1000, 0, 0x00, 0x01, 0x40, 0x40);
    ENC(1, 55, 1, 1000, -2, 0x00, 0x01, 0x40, 0x40);

    // ---- the amplitude scale
    static const struct { int step; float amp; } table[] = {{0, 0}, {1, 0.007843f}, {2, 0.011823f}, {6, 0.023648f},
        {15, 0.112491f}, {16, 0.117471f}, {31, 0.224982f}, {32, 0.229904f}, {64, 0.459808f}, {100, 1.002867f}};
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) {
        float a = rumble_hd_step_amp(table[i].step);
        CHECK(fabsf(a - table[i].amp) <= table[i].amp * 0.003f, "step %d is %f, the table says %f", table[i].step, (double)a,
              (double)table[i].amp);
    }
    for (int k = 0; k <= RUMBLE_HD_AMP_STEPS; k++) {
        CHECK(rumble_hd_amp_step(rumble_hd_step_amp(k)) == k, "step %d's amplitude %f gives step %d", k,
              (double)rumble_hd_step_amp(k), rumble_hd_amp_step(rumble_hd_step_amp(k)));
        if (k) CHECK(rumble_hd_step_amp(k) > rumble_hd_step_amp(k - 1), "step %d is not above step %d", k, k - 1);
    }
    int top = 0;
    for (float a = -1; a < 20; a += 0.0007f) {
        int k = rumble_hd_amp_step(a);
        if (k > top) top = k;
        CHECK(k >= 0 && k <= RUMBLE_HD_AMP_STEPS, "amplitude %f gives step %d", (double)a, k);
    }
    CHECK(top == RUMBLE_HD_AMP_STEPS && RUMBLE_HD_AMP_STEPS == 100, "the highest step reached is %d", top);

    // ---- a report's data from the two sides
    RumbleHdValue v[2] = {{0.5f, 160, 0, 320}, {0, 160, 0.9f, 150}};
    int both[2] = {1, 1}, left[2] = {1, 0}, none[2] = {0, 0};
    uint8_t d[8];
    rumble_hd_data(v, both, 1, 0, d);
    CHECK(!memcmp(d, "\x00\x01\x40\x62\x74\xbe\x40\x40", 8), "two sides: %02x %02x %02x %02x | %02x %02x %02x %02x", d[0], d[1],
          d[2], d[3], d[4], d[5], d[6], d[7]);
    rumble_hd_data(v, left, 1, 0, d);
    CHECK(!memcmp(d, "\x00\x01\x40\x62\x00\x01\x40\x40", 8), "a side at rest is not neutral");
    rumble_hd_data(v, none, 1, 0, d);
    CHECK(!memcmp(d, rumble_hd_neutral, 8), "both sides at rest are not neutral");
    rumble_hd_data(v, both, 1, 1, d);   // one actuator: the stronger side (the right one, 0.9) in both halves
    CHECK(!memcmp(d, "\x74\xbe\x40\x40\x74\xbe\x40\x40", 8), "one actuator: %02x %02x %02x %02x | %02x %02x %02x %02x", d[0],
          d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    rumble_hd_data(v, left, 1, 1, d);   // ... and the side that is not at rest when the stronger one is
    CHECK(!memcmp(d, "\x00\x01\x40\x62\x00\x01\x40\x62", 8), "one actuator, right side at rest");
    rumble_hd_data(v, both, 0, 0, d);
    CHECK(!memcmp(d, rumble_hd_neutral, 8), "strength 0 is not neutral");

    // ---- the output report
    uint8_t r[49];
    memset(r, 0xee, sizeof r);
    rumble_hd_report((const uint8_t *)"\x74\xbe\xbd\x6f\x00\x01\x40\x40", 0x25, r, sizeof r);
    CHECK(r[0] == 0x10 && r[1] == 0x05 && !memcmp(r + 2, "\x74\xbe\xbd\x6f\x00\x01\x40\x40", 8), "report head %02x %02x", r[0], r[1]);
    for (int i = 10; i < 49; i++) CHECK(r[i] == 0, "report byte %d is %02x", i, r[i]);

    // ---- from a batch to the reports written: a tick every 5 ms, 15 ms between reports
    static const uint8_t A[8] = {1, 2, 3, 4, 5, 6, 7, 8}, B[8] = {9, 9, 9, 9, 9, 9, 9, 9}, C[8] = {3, 3, 3, 3, 3, 3, 3, 3},
                         D[8] = {4, 4, 4, 4, 4, 4, 4, 4}, E[8] = {5, 5, 5, 5, 5, 5, 5, 5};
    RumbleHd h = {0};
    uint8_t o[8];
    uint64_t t0 = 1000 * MS;
    CHECK(!rumble_hd_due(&h, 1, t0, RUMBLE_HD_GAP_NS, o) && rumble_hd_idle(&h, 1, t0), "a report is due before the game sent anything");
    rumble_hd_post(&h, rumble_hd_neutral, t0);
    CHECK(!rumble_hd_due(&h, 1, t0, RUMBLE_HD_GAP_NS, o) && rumble_hd_idle(&h, 1, t0), "the neutral value is written to a controller at rest");
    // three values in one batch: the newest is written, at once
    rumble_hd_post(&h, A, t0);
    rumble_hd_post(&h, B, t0);
    rumble_hd_post(&h, C, t0 + MS);
    struct { uint64_t at; uint8_t d[8]; } rep[64];
    int n = 0;
    for (uint64_t t = t0 + MS; t <= t0 + 400 * MS; t += 5 * MS) {
        if (t == t0 + 6 * MS) rumble_hd_post(&h, D, t);    // 5 ms after the first report: too soon to write
        if (t == t0 + 11 * MS) rumble_hd_post(&h, E, t);   // replaces D before D was written
        if (rumble_hd_due(&h, 1, t, RUMBLE_HD_GAP_NS, o) && n < 64) {
            rep[n].at = t - t0;
            memcpy(rep[n++].d, o, 8);
            rumble_hd_sent(&h, o, t);
        }
    }
    CHECK(n >= 2 && rep[0].at == MS && !memcmp(rep[0].d, C, 8), "the first report is not the batch's newest value, at once");
    CHECK(n >= 2 && rep[1].at == 16 * MS && !memcmp(rep[1].d, E, 8), "the second report is not the newest value 15 ms later (at %llu ms)",
          n >= 2 ? (unsigned long long)(rep[1].at / MS) : 0ull);
    int held = 0, neutral = 0;
    uint64_t last_e = 0, first_neutral = 0, last_neutral = 0;
    for (int i = 0; i < n; i++) {
        CHECK(memcmp(rep[i].d, A, 8) && memcmp(rep[i].d, B, 8) && memcmp(rep[i].d, D, 8), "a replaced value was written at %llu ms",
              (unsigned long long)(rep[i].at / MS));
        if (i) CHECK(rep[i].at - rep[i - 1].at >= RUMBLE_HD_GAP_NS, "two reports %llu ms apart", (unsigned long long)((rep[i].at - rep[i - 1].at) / MS));
        if (!memcmp(rep[i].d, E, 8)) {
            if (last_e) CHECK(rep[i].at - last_e == RUMBLE_HD_REFRESH_NS, "a held value was written again after %llu ms",
                              (unsigned long long)((rep[i].at - last_e) / MS));
            last_e = rep[i].at, held++;
            CHECK(!neutral, "a value after the neutral one");
        }
        if (!memcmp(rep[i].d, rumble_hd_neutral, 8)) {
            if (!neutral++) first_neutral = rep[i].at;
            last_neutral = rep[i].at;
        }
    }
    // E came at 11 ms and is stale 250 ms later: written at 16 ms and every 30 ms until then
    CHECK(held == 9, "the held value was written %d times", held);
    CHECK(neutral == RUMBLE_HD_NEUTRAL_REPORTS, "the neutral value was written %d times", neutral);
    CHECK(first_neutral > 261 * MS && first_neutral <= 261 * MS + RUMBLE_HD_GAP_NS, "the neutral value came %llu ms in",
          (unsigned long long)(first_neutral / MS));
    CHECK(last_neutral - first_neutral == 2 * RUMBLE_HD_GAP_NS, "the neutral reports are not 15 ms apart");
    CHECK(rumble_hd_idle(&h, 1, t0 + 400 * MS) && !rumble_hd_buzzing(&h), "not idle after the neutral reports");

    // the path turned off while the game sends: the neutral value, whatever the game's data
    RumbleHd g = {0};
    rumble_hd_post(&g, A, t0);
    CHECK(rumble_hd_due(&g, 1, t0, RUMBLE_HD_GAP_NS, o) && !memcmp(o, A, 8), "a first value is not due at once");
    rumble_hd_sent(&g, o, t0);
    CHECK(!rumble_hd_idle(&g, 1, t0 + MS) && rumble_hd_buzzing(&g), "idle while a value is held");
    CHECK(!rumble_hd_due(&g, 0, t0 + 5 * MS, RUMBLE_HD_GAP_NS, o), "off: a report inside the gap");
    CHECK(rumble_hd_due(&g, 0, t0 + 15 * MS, RUMBLE_HD_GAP_NS, o) && !memcmp(o, rumble_hd_neutral, 8), "off: no neutral report");
    CHECK(rumble_hd_due(&g, 0, t0 + 6 * MS, 5 * MS, o) && !memcmp(o, rumble_hd_neutral, 8), "a 5 ms gap is not honoured");
    rumble_hd_sent(&g, o, t0 + 15 * MS);
    CHECK(!rumble_hd_buzzing(&g) && !rumble_hd_idle(&g, 0, t0 + 16 * MS), "idle before the neutral value was repeated");
    // back on with the game's data still fresh: it is written again
    CHECK(rumble_hd_due(&g, 1, t0 + 30 * MS, RUMBLE_HD_GAP_NS, o) && !memcmp(o, A, 8), "on again: the game's value is not due");
    rumble_hd_sent(&g, o, t0 + 30 * MS);
    // another controller: what was written is forgotten, the game's value is due at once
    rumble_hd_forget(&g);
    CHECK(!rumble_hd_buzzing(&g), "buzzing after forget");
    CHECK(rumble_hd_due(&g, 1, t0 + 31 * MS, RUMBLE_HD_GAP_NS, o) && !memcmp(o, A, 8), "after forget: the value is not due at once");
    CHECK(!rumble_hd_due(&g, 1, t0 + 300 * MS, RUMBLE_HD_GAP_NS, o) && rumble_hd_idle(&g, 1, t0 + 300 * MS),
          "after forget: a stale value is written to a controller that got nothing");

    printf(fails ? "rumble hd: %d failures\n" : "rumble hd: ok\n", fails);
    return fails != 0;
}
