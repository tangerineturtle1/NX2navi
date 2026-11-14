// HD rumble on a DualSense's haptic actuators, through its USB audio device (rumble_haptics.c).
#pragma once
#include <stddef.h>

#include "rumble_hd.h"

// Opens the DualSense's audio device and starts playing (silence until a post). 1 on success; why says what was
// opened or why not, for the log. Any thread.
int haptics_open(char *why, size_t cap);
// The game's newest value per side (v[0] left, v[1] right), live[s] 0 for a side at rest; the output is scaled by
// strength (1 = the default rumble strength).
void haptics_post(const RumbleHdValue v[2], const int live[2], float strength);
// Stops playing and closes the device (the actuators fall still).
void haptics_close(void);

// One actuator's target: amplitude and frequency of the low band [0] and the high band [1].
typedef struct { float amp[2], freq[2]; } HapticsSide;
// One actuator's two sines as they are playing, carried from one block of samples to the next.
typedef struct { double phase[2]; float amp[2], freq[2]; } HapticsVoice;
// The game's amplitude (0..1) lifted for the DualSense: 0 at or below 0.001, 1 at 1.
float haptics_lift(float a);
// One side's target from the game's value: each amplitude lifted, then its square root, the two scaled down together
// past full scale; frequencies limited to 20..1000 Hz (160 and 320 when not set).
HapticsSide haptics_side(const RumbleHdValue *v);
// frames samples of one actuator into out (every stride floats), the voice gliding toward target, times gain.
void haptics_synth(HapticsVoice *voice, const HapticsSide *target, float gain, float *out, int frames, int stride);
