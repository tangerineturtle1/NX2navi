// The action prompt's diamond of face buttons laid out for a pad layout (action_prompt.c).
#ifndef ACTION_PROMPT_H
#define ACTION_PROMPT_H

#include <stddef.h>
#include <stdint.h>

// The shared interface layouts of the game, which hold the action prompt (DoCommand_00).
#define ACTION_PROMPT_ARCHIVE "UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs"

// Lays the diamond of DoCommand_00.buil (p[0..n), in place) out for the face buttons by label (label nonzero: A
// bottom, B right, X left, Y top) or by position (label = 0: the Switch's places, A right, B bottom, X top, Y left).
// The runtime asks for label 1 only (romfs.c action_prompt_overlay); label 0 takes a layout by label back. Every field
// is found and checked before any is written, and read again after. Returns the number of fields changed; 0 when the
// diamond is laid out that way already; -1 when the file is not the layout this expects (why says which field
// differs; the file is left as it was).
int action_prompt_lay_out(uint8_t *p, size_t n, int label, char *why, size_t why_n);

// The same on the decompressed Common layout archive (a SARC holding DoCommand_00.buil), on every copy of the layout
// it holds: all of them change or none. The count is over all copies; -1 also for a damaged archive, more than 16
// copies, or copies that share some of their bytes.
int action_prompt_archive(uint8_t *sarc, size_t n, int label, char *why, size_t why_n);

// The Common layout archive's zstd file in[0..n) with the diamond laid out for label, compressed again with the
// dictionary of Pack/ZsDic.pack.zs (zsdic) the original names: *out (malloc'd) and *out_n. Returns 1 when *out was
// made, 0 when the diamond is laid out that way already (nothing to serve), -1 when it cannot be (logged).
int action_prompt_build(const void *in, size_t n, const void *zsdic, size_t zsdic_n, int label, void **out,
                        size_t *out_n);

#endif
