// Contains code Copyright (c) 2026 MacNativeGames contributors, Copyright (c) Ryujinx Team and Contributors (MIT; see
// THIRD_PARTY_NOTICES.md).
#ifndef NX2NAVI_AUDIO_REVERB_H
#define NX2NAVI_AUDIO_REVERB_H
#include <stdint.h>
typedef struct {
    uint8_t input[6], output[6]; uint16_t channel_max, channels;
    uint32_t reserved, sample_rate;
    float room_hf, hf_reference, decay_time, hf_decay_ratio, room_gain, reflections_gain,
          reverb_gain, diffusion, reflection_delay, reverb_delay, density, dry_gain;
    uint8_t status;
} Reverb3dParam;
_Static_assert(__builtin_offsetof(Reverb3dParam, status) == 72, "reverb3d wire layout");
typedef struct Reverb3d Reverb3d;
/* Why the game's block `p` cannot be used, as a constant string, or NULL. create and update end the process on such
   a block, so the caller asks first. */
const char *audio_reverb_invalid(const Reverb3dParam *p);
Reverb3d *audio_reverb_create(const Reverb3dParam *p);
void audio_reverb_free(Reverb3d *s);
void audio_reverb_update(Reverb3d *s, const Reverb3dParam *p);
void audio_reverb_process(Reverb3d *s, float *const input[6], float *const output[6], int frames);
#endif
