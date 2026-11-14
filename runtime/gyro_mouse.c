// Custom gyro (NX2NAVI_GYRO_MODE=player, the menu's Custom, live): the gamepad's gyro turns the camera through the
// mouse camera's path (mouse_direct.c camera_input_oot) instead of reaching the game as its six-axis sensor.
// NX2NAVI_GYRO_SPACE (live) picks how the controller's rotation becomes a turn left or right and up or down
// (gyro_space_turn):
//   player    (the default) player space, below.
//   world     the turn about the real vertical, whole, and the tilt about the controller's left-right axis made level.
//   yaw       local space: the turn about the axis out of the controller's face (a flat controller turned like a dial).
//   roll      local space: the roll about the axis out of its top edge (a flat controller turned like a wheel).
//   yaw_roll  local space: the two added.
//   laser     a pointer: the turn about the real vertical as in world space, and up or down the change of the angle the
//             top edge points above the horizon, read from gravity, so pointing back at a spot brings the aim back to
//             it with no drift; no dead zone or tightening, which would move the pointer off where the controller points.
// Pitch is the tilt about the controller's own left-right axis in every space but world and laser.
// Player space: yaw follows the turn
// about the real vertical, wherever the controller points: the turn about gravity, measured on the controller's two
// axes other than its pitch axis (x), then let out up to twice its size (a 60 degree buffer) but never past the
// combined rate of those two axes, so a turn the hands make by rolling the controller sideways counts as well as one made by twisting it
// flat. Pitch is the turn about the controller's own x axis. Holding the controller flat, upright or anywhere between
// then turns the camera the same way for the same movement of the hands.
//
// The sensor thread (input.c motion_main, every 4 ms) feeds every report the controller sent, each with the time since
// the one before it by the controller's own clock and the fusion's up direction (motion_fusion.c motion_up), so no
// report is skipped or counted twice whatever the controller's rate; the camera input update takes the turn gathered
// since its last call, in degrees.
// NX2NAVI_GYRO_SCOPE (live): always, or aim, only while the game's aiming camera is on (mouse_direct.c camera_aiming),
// where the game would turn its own gyro aiming on.
// While the player space gyro turns the camera, the game's six-axis sensor reads at rest instead (ov_input.c six_fill),
// so the game's own gyro aiming does not add a second turn. With the aim scope it reads the real motion while no known
// item aims (gyro_mouse_owns_sensor), so the game's own gyro aiming still covers an aimed item the player space gyro
// does not detect, and never one it does.
//
// A still controller is not quite still in its readings: its gyro has a small bias. While the controller lies or is
// held still (gravity alone on the accelerometer, the rate under STILL_RATE) for STILL_SECONDS, the rate is averaged into
// the bias, which every sample loses. Nothing else is done to the rate unless the player asks: no smoothing and, by
// default, no dead zone, so the camera turns exactly with the controller.
//
// NX2NAVI_GYRO_DEADZONE (live, degrees a second, 0 = none): a turn slower than this is dropped and a faster one loses
// this much of its speed, so the turn grows from nothing past the edge instead of jumping.
// NX2NAVI_GYRO_TIGHTEN (live, degrees a second, default 1.5, 0 = none), after the dead zone: a turn slower than this is scaled down
// in proportion to its speed (half the threshold turns at half speed), one at or above it is untouched, so a held
// controller's tremor fades without an edge while a deliberate turn keeps its speed.
// NX2NAVI_GYRO_TOUCH_HOLD (live, default on): while a finger rests on the gamepad's touchpad (input.c motion_main
// reads it) nothing is turned, so the controller can be brought back to the centre and the turn goes on from there
// when the finger lifts, as a mouse is lifted off the desk.
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "gyro_mouse.h"
#include "input.h"
#include "platform/platform.h"

#define YAW_RELAX 2.0
#define STILL_RATE 0.06        // rad/s (3.4 degrees a second)
#define STILL_ACC 0.03         // G off 1
#define STILL_SECONDS 1.0
#define BIAS_SECONDS 2.0       // the bias average's time constant once still
#define STALE_NS 100000000ull  // no take for this long (a menu, a load): the turn gathered meanwhile is dropped
#define RAD_DEG (180.0 / M_PI)

void gyro_player_space(const double w[3], const double up[3], double out[2]) {
    double n = sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    double u1 = n > 0 ? up[1] / n : 0, u2 = n > 0 ? up[2] / n : 1;
    double world = w[1] * u1 + w[2] * u2, limit = hypot(w[1], w[2]);
    double yaw = fmin(fabs(world) * YAW_RELAX, limit);
    out[0] = -copysign(yaw, world);   // a turn to the left (counter-clockwise from above) turns the camera left
    out[1] = w[0];                    // the top edge tilting up looks up
}

// World space: the whole turn about the real vertical, and the tilt about the controller's left-right axis with its
// vertical part taken off, so a controller held rolled sideways still looks up when its top edge rises.
static void gyro_world_space(const double w[3], const double up[3], double out[2]) {
    double n = sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    double u[3] = {n > 0 ? up[0] / n : 0, n > 0 ? up[1] / n : 0, n > 0 ? up[2] / n : 1};
    out[0] = -(w[0] * u[0] + w[1] * u[1] + w[2] * u[2]);
    double side[3] = {1 - u[0] * u[0], -u[0] * u[1], -u[0] * u[2]};   // the left-right axis, level
    double s = sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
    out[1] = s > 1e-6 ? (w[0] * side[0] + w[1] * side[1] + w[2] * side[2]) / s : w[0];
}

void gyro_space_turn(int space, const double w[3], const double up[3], double out[2]) {
    switch (space) {
    case GYRO_SPACE_WORLD: case GYRO_SPACE_LASER: gyro_world_space(w, up, out); return;
    case GYRO_SPACE_YAW: out[0] = -w[2], out[1] = w[0]; return;            // counter-clockwise from above: left
    case GYRO_SPACE_ROLL: out[0] = w[1], out[1] = w[0]; return;            // the right side going down: right
    case GYRO_SPACE_YAW_ROLL: out[0] = w[1] - w[2], out[1] = w[0]; return;
    default: gyro_player_space(w, up, out); return;
    }
}

// Laser pointer: the angle, in degrees, the controller's top edge points above the horizon.
static double elevation_deg(const double up[3]) {
    double n = sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    double e = n > 0 ? up[1] / n : 0;
    return asin(e < -1 ? -1 : e > 1 ? 1 : e) * RAD_DEG;
}

static int mode_player = -1, scope_aim, space;
static const char *const space_names[] = {"player", "world", "yaw", "roll", "yaw_roll", "laser"};
static double sens[2] = {2, 2};   // horizontal, vertical
static double deadzone, tighten = 1.5;  // degrees a second
static int touch_hold = 1;
static unsigned seen;

static void settings(void) {
    unsigned v = cfg_version();
    if (mode_player >= 0 && v == seen) return;
    seen = v;
    const char *m = cfg_value("NX2NAVI_GYRO_MODE");
    mode_player = m && !strcmp(m, "player");
    const char *g = cfg_value("NX2NAVI_GYRO_SCOPE");
    scope_aim = g && !strcmp(g, "aim");
    const char *sp = cfg_value("NX2NAVI_GYRO_SPACE");
    space = GYRO_SPACE_PLAYER;
    for (int i = 0; i < (int)(sizeof space_names / sizeof *space_names); i++)
        if (sp && !strcmp(sp, space_names[i])) space = i;
    static const char *const names[2] = {"NX2NAVI_GYRO_SENS_X", "NX2NAVI_GYRO_SENS_Y"};
    for (int i = 0; i < 2; i++) {
        double s = cfg_double(names[i]);
        sens[i] = s < 0 ? 0 : s > 10 ? 10 : s;
    }
    double d = cfg_double("NX2NAVI_GYRO_DEADZONE");
    deadzone = d > 0 ? (d > 10 ? 10 : d) : 0;
    double t = cfg_double("NX2NAVI_GYRO_TIGHTEN");
    tighten = t > 0 ? (t > 10 ? 10 : t) : 0;
    touch_hold = cfg_bool("NX2NAVI_GYRO_TOUCH_HOLD");
}

int gyro_mouse_on(void) {
    settings();
    return mode_player && input_motion_enabled() && input_direct_mouse_available();
}

int gyro_mouse_aim_only(void) {
    settings();
    return scope_aim;
}

// The camera input update runs every frame of gameplay; an answer older than this (a menu, a load) counts as no aim.
#define AIMING_STALE_NS 100000000ull
static volatile int aiming_now;
static volatile uint64_t aiming_at;

void gyro_mouse_set_aiming(int aiming) {
    aiming_at = plat_now_ns();
    aiming_now = aiming;
}

int gyro_mouse_owns_sensor(void) {
    if (!gyro_mouse_on()) return 0;
    if (!scope_aim) return 1;
    uint64_t at = aiming_at, now = plat_now_ns();
    return aiming_now && at && now - at <= AIMING_STALE_NS;
}

static PlatMutex lock = PLAT_MUTEX_INIT;
static double pending[2];   // degrees: right, up
static double bias[3], still_s;
static double laser_elev;     // laser pointer: the elevation at the last sample
static int laser_have;        // ... which is set
static uint64_t last_take;

void gyro_mouse_feed(const double acc[3], const double gyro[3], const double up[3], int touched, double dt) {
    settings();
    uint64_t now = plat_now_ns();
    plat_mutex_lock(&lock);
    if (!(dt > 0) || dt > 0.05) dt = 0;   // the first sample, or a gap: nothing turns across it
    double a = sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]), w[3];
    for (int i = 0; i < 3; i++) w[i] = gyro[i] - bias[i];
    double rate = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    if (fabs(a - 1) < STILL_ACC && rate < STILL_RATE) {
        still_s += dt;
        if (still_s > STILL_SECONDS && dt > 0) {
            double k = dt / BIAS_SECONDS;
            for (int i = 0; i < 3; i++) bias[i] += k * w[i], w[i] -= k * w[i];
        }
    } else still_s = 0;
    double ps[2], add[2];
    gyro_space_turn(space, w, up, ps);
    int laser = space == GYRO_SPACE_LASER;
    double keep = 1;
    if (!laser) {   // a pointer follows the controller exactly: no dead zone or tightening
        double speed = hypot(ps[0], ps[1]) * RAD_DEG;
        keep = deadzone <= 0 ? 1 : speed > deadzone ? (speed - deadzone) / speed : 0;
        double left = speed * keep;   // the speed past the dead zone
        if (tighten > 0 && left < tighten) keep *= left / tighten;
    }
    if (touched && touch_hold) keep = 0;   // the ratchet: the controller moves, the camera stays
    add[0] = ps[0] * keep * RAD_DEG * dt;
    add[1] = ps[1] * keep * RAD_DEG * dt;
    if (laser) {   // up and down from where the top edge points, which gravity gives without drift
        double e = elevation_deg(up);
        add[1] = laser_have && dt > 0 ? (e - laser_elev) * keep : 0;
        laser_elev = e, laser_have = 1;
    } else laser_have = 0;
    if (last_take && now - last_take > STALE_NS) pending[0] = pending[1] = 0;
    else
        for (int i = 0; i < 2; i++) pending[i] += add[i];
    plat_mutex_unlock(&lock);
}

int gyro_mouse_take(double deg[2]) {
    settings();
    uint64_t now = plat_now_ns();
    plat_mutex_lock(&lock);
    int fresh = last_take && now - last_take <= STALE_NS;
    last_take = now;
    deg[0] = fresh ? pending[0] * sens[0] : 0;
    deg[1] = fresh ? pending[1] * sens[1] : 0;
    pending[0] = pending[1] = 0;
    plat_mutex_unlock(&lock);
    return deg[0] != 0 || deg[1] != 0;
}
