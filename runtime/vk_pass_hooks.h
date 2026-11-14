// Which pass begin runs the frame's pass hooks (vk_exec.c vk_exec_pass_begin: vk_contact_pass_begin, vk_dlaa_pass).
// Header only, so that tests/pass_hooks_check.c checks the rule without a device.
//
// The hooks follow the game's frame pass by pass, and what they keep is the last pass's that ran them: the light
// target the upscaler takes is the one of the last scene pass before the output pass (vk_dlaa.c vk_dlaa_pass). A pass
// is begun more than once while it is recorded: a barrier, a texture lookup that waits for its turn or a pass of the
// replay's own ends it, and the next draw begins it again. On one thread the second begin runs the hooks in the same
// place of the stream, with the same targets, and changes nothing. A parallel group's pass is recorded by several
// threads, and its hooks run in the group's first chunk. With the submitting thread going on past the group
// (NX2NAVI_REPLAY_PIPE) a second begin in that chunk came after the hooks of passes later in the stream had run, and
// put the group's own target back as the light target: the upscaler then took an earlier image of the frame for one
// frame, a flash of the whole scene with the interface intact (Kakariko, DLSS Performance, 3 workers, 4 groups out:
// 51 and 52 such frames in two runs of 100 s, none with the helpers off). So a group's hooks run once, at its first
// chunk's first pass.
#pragma once

// in_chunk: the calling thread records a chunk of a parallel group; idx: the chunk's place in its group; ran: the
// group's hooks ran already (its first chunk began a pass).
static inline int pass_hooks_due(int in_chunk, int idx, int ran) { return !in_chunk || (!idx && !ran); }
