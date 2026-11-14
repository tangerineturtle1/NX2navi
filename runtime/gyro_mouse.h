// Player space gyro through the mouse camera (gyro_mouse.c).
#pragma once
#include <stdint.h>

// 1 while NX2NAVI_GYRO_MODE is player, motion is on and the mouse camera's patch set runs (live).
int gyro_mouse_on(void);
// One sensor sample (input.c motion_main): acceleration in G, rate in rad/s, both in the Switch controller frame, and
// the fusion's up direction in that frame; touched is 1 while a finger rests on the gamepad's touchpad; dt the seconds
// since the sample before it (0 for the first, or after a gap: nothing turns).
void gyro_mouse_feed(const double acc[3], const double gyro[3], const double up[3], int touched, double dt);
// 1 while NX2NAVI_GYRO_SCOPE is aim: the turn counts only while the game's aiming camera is on (mouse_direct.c
// camera_aiming).
int gyro_mouse_aim_only(void);
// With NX2NAVI_GYRO_SCOPE=aim: the camera input update's answer, 1 while the game aims with an item the gyro turns.
void gyro_mouse_set_aiming(int aiming);
// 1 while the player space gyro has the controller's motion: always with NX2NAVI_GYRO_SCOPE=always, with aim only while
// it turns an aim. The game's six-axis sensor reads a controller at rest then (ov_input.c six_fill), and the real
// motion otherwise, so the game's own gyro aiming works with the items the player space gyro does not know.
int gyro_mouse_owns_sensor(void);
// The camera turn gathered since the last call, in degrees (right times NX2NAVI_GYRO_SENS_X, up times
// NX2NAVI_GYRO_SENS_Y); 1 when nonzero.
int gyro_mouse_take(double deg[2]);
// The rate w (rad/s) in player space: out[0] the turn to the right, out[1] the turn up, rad/s.
void gyro_player_space(const double w[3], const double up[3], double out[2]);
// NX2NAVI_GYRO_SPACE, in the order of its values (cfg.c).
enum { GYRO_SPACE_PLAYER, GYRO_SPACE_WORLD, GYRO_SPACE_YAW, GYRO_SPACE_ROLL, GYRO_SPACE_YAW_ROLL, GYRO_SPACE_LASER };
// The rate w (rad/s) as a turn in that space: out[0] to the right, out[1] up, rad/s (the laser pointer's out[1] is the
// world space one; gyro_mouse_feed takes its up and down from the elevation instead).
void gyro_space_turn(int space, const double w[3], const double up[3], double out[2]);
