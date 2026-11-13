// The stutter map (NX2NAVI_STUTTER_MAP=1, stutter_map.c): for a play session, which frames hitched, in which area, what
// the frame waited for, and which pipelines and shaders draws met missing, so the next pipeline list and shader
// preparation can take them in.
#pragma once
#include <stdint.h>

extern int stutter_map_on;   // set once at start (stutter_map_init); every hook below returns at once while it is 0

void stutter_map_init(void);
// An area's load (perf.c perf_scene, from romfs.c): hitches from here on are counted in that area, and those within
// SM_LOAD_NS of the load apart from play.
void stutter_area(const char *name);
// A draw met its pipeline (shader 0) or one of its shaders (shader 1) missing and the replay thread spent ns on it
// before it was bound or skipped (vk_exec_draw.c, beside wait_note). vi, fi: the vertex (or mesh) and fragment shaders'
// input ids, NULL when not known (still translating, or no fragment stage).
void stutter_miss(int shader, int mesh, const uint8_t *vi, const uint8_t *fi, uint64_t ns, int skipped);
// A pipeline a draw missed became usable ms after the miss (vk_pipeline.c ready_note): recorded 1 when pipeline-keys.bin
// (or the seed list) named it, fresh 1 when one of its shaders was translated in this session (not prepared).
void stutter_ready(int mesh, const uint8_t *vi, const uint8_t *fi, double ms, int recorded, int fresh, int fast);
// Every present (perf.c ft_frame): d the frame's interval, part the time it went to by cause (names[i], n of them;
// the last one is the part nothing recorded explains), cause the one the frame is charged to.
void stutter_frame(uint64_t frame, uint64_t now, uint64_t d, const uint64_t *part, const char *const *names, int n,
                   int cause);
void stutter_report(const char *tag);   // the map so far; every 10 minutes of play and at exit
