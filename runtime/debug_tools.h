// The debug menu's access to the game (debug_tools.c). Everything but dbg_tick is for the menu's thread.
#pragma once
#include <stdint.h>

#ifndef __cplusplus
#include "cpu.h"
void dbg_tick(Cpu *c);                 // the game's thread, at its pad read (ov_input.c): does what the menu asked for
// ... and the pad's state before the game gets it: the free camera takes the sticks and four buttons while it flies
void dbg_pad(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry);
void dbg_freecam_host(Cpu *c);         // the free camera's patch site (recomp/patches-v111.txt, set freecam)
#endif

#ifdef __cplusplus
extern "C" {
#endif
int dbg_supported(void);               // this game build's structures are known (Ocarina of Time 1.1.1)

int dbg_player_pos(float xyz[3]);      // Link's position in metres; 0 while there is no player
int dbg_player_rot(float m[9]);        // Link's rotation, a 3x3 matrix by rows; 0 while there is no player
int dbg_area(char out[64]);            // the area the game is in, by its name in the game's files; 0 unknown
// Every 64-character text of the game data (the area among them) with the hash of its name; returns their number.
int dbg_texts(void (*cb)(uint32_t hash, const char *text, void *ctx), void *ctx);
uint32_t dbg_name_hash(const char *name);   // the hash the game knows a flag or a text by
float dbg_speed_factor(void);          // the game's speed: 1 normal, 0.05 to 8 (frame_rate.c multiplies the game's step)
void dbg_speed_set(float factor);

// The game's on/off flags, sorted by hash. dbg_flags_refresh reads their values and returns their number (0 until a
// save is loaded). A change is made by the game's own function at the next frame.
int dbg_flags_refresh(void);
uint32_t dbg_flag_hash(int i);
int dbg_flag_value(int i);
const char *dbg_flag_name(int i);      // NULL: no name found for it
void dbg_flag_set(uint32_t hash, int on);
int dbg_names(int *found);             // 0 not started, 1 being read from the game's files, 2 read (*found of them)

// Individual collection flags, in the order of the game's own 100-entry collection list. -1 means no loaded value.
int dbg_skulls(void);
const char *dbg_skull_area(int i);
int dbg_skull_pos(int i, float xyz[3]);
int dbg_skull_collected(int i);
void dbg_skull_set(int i, int on);       // also sets the token count to the number collected
int dbg_skull_tokens(void);              // the token count the game shows and its rewards read; -1 not loaded
void dbg_skull_tokens_match(void);       // sets the token count to the number collected
// Travel uses the nearest named entrance in the placement's area; it does not put Link at the spider's coordinates.
const char *dbg_skull_entrance(int i);   // NULL when no known entrance has coordinates
int dbg_skull_travel(int i);

// Teleporting and events. The places are the game's areas with their named start positions, and the events its event
// files that hold a script: both lists come from the player's game files with the flag names (0 until dbg_names
// answers 2). dbg_warp changes area as the game's own doors do; dbg_event_start starts an event by its name. One at a
// time: nothing is taken while dbg_busy. dbg_status is the last thing that happened, in words.
int dbg_places(void);
const char *dbg_place_area(int i);
const char *dbg_place_pos(int i);      // never "": a teleport without a listed position takes the area's first
void dbg_warp(const char *area, const char *pos);
// The start position of area nearest to xyz (metres) among those with coordinates: its name and distance; 0 none.
int dbg_place_nearest(const char *area, const float xyz[3], char name[64], float *dist);
int dbg_events(void);
const char *dbg_event_name(int i);
void dbg_event_start(const char *name);
int dbg_busy(void);
void dbg_status(char out[160]);

// The free camera: the game draws from a camera of the menu's instead of its own. With sticks on, the pad flies it and
// Link stands; with sticks off it stays where it is and the pad plays the game. speed in metres a second.
int dbg_freecam_available(void);       // the build has the camera's patch set
void dbg_freecam_set(int on, int sticks, float speed);
void dbg_freecam_reset(void);          // back to the game's view, free again from there
int dbg_freecam(int *sticks, float *speed, double pos[3]);   // on or off
void dbg_freecam_angles(float *yaw, float *pitch);           // radians: it looks along (cos p sin y, sin p, cos p cos y)
// The game's thread: the free camera, which is on, put at a place and direction outright.
void dbg_freecam_place(const double pos[3], float yaw, float pitch);
// The game's own camera, before the free camera stands in for it: dbg_camera_watch(1) has the game's camera update
// report it (it does anyway while the free camera is on), dbg_camera_seen gives the last position, target and up vector
// of the camera that follows Link (which 0) or of the events' camera (1) and how many updates were seen so far.
void dbg_camera_watch(int on);
uint64_t dbg_camera_seen(int which, double pos[3], double at[3], float up[3]);
// The game's thread: Link runs to a point of the area (x, z in metres), reached within near_m. dbg_route_state: 1 on
// his way, 0 there or no route, 2 given up.
void dbg_route_to(float x, float z, float near_m);
int dbg_route_state(void);
void dbg_route_stop(void);             // the game's thread: the route ends where Link is
int dbg_freecam_flying(void);          // on, with the sticks and ground avoidance: vk_dlaa.c reads the ground's distances back
void dbg_freecam_avoid_set(int on);    // ground avoidance (off by default): the camera stays a metre above ground-like surfaces
int dbg_freecam_avoid(void);
// The replay's thread: nine view distances of the lower view (three rows of three, the top row first; 0 sky), with the
// tangent of half the view's vertical angle and its width over its height
void dbg_freecam_ground(const float *dist, int n, float tan_half_y, float aspect);

// Memory: values of 1, 2 or 4 bytes in the game's heap.
int dbg_search_new(uint32_t value, int size);       // returns the number of places
int dbg_search_next(int mode, uint32_t value);      // 0 equals value, 1 changed, 2 unchanged, 3 greater, 4 less
int dbg_search_count(int *over);                    // *over: the first search found more than are kept
uint64_t dbg_search_addr(int i);
int dbg_search_size(void);
void dbg_search_clear(void);
int dbg_mem_read(uint64_t addr, int size, uint32_t *v);
void dbg_mem_write(uint64_t addr, uint64_t value, int size);
int dbg_freeze(uint64_t addr, uint64_t value, int size, int on);   // written every frame while on; 0: the list is full
int dbg_frozen(uint64_t addr);
int dbg_frozen_count(void);
void dbg_unfreeze_all(void);
#ifdef __cplusplus
}
#endif
