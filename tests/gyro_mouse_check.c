// Player space gyro (runtime/gyro_mouse.c) with the rest of the runtime stubbed: the yaw a turn about the vertical makes
// with the controller held flat, upright and tilted between; the yaw a sideways roll adds; pitch; the sensitivity; the
// bias a still controller loses; the turn dropped while nothing takes it; the setting gates.
// Built by tests/gyro_mouse_check.py.
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "gyro_mouse.c"

static const char *mode = "player", *scope = "always", *space_value = "player";
static double sens_value = 1, sens_y = 1, deadzone_value = 0, tighten_value = 0;
static int touch, touch_hold_value = 1;
static unsigned version = 1;
static int motion_on = 1, patch_on = 1;
static uint64_t clock_ns = 1000000000ull;
unsigned cfg_version(void) { return version; }
const char *cfg_value(const char *name) {
    return !strcmp(name, "NX2NAVI_GYRO_MODE") ? mode : !strcmp(name, "NX2NAVI_GYRO_SCOPE") ? scope
         : !strcmp(name, "NX2NAVI_GYRO_SPACE") ? space_value : "";
}
double cfg_double(const char *name) {
    return !strcmp(name, "NX2NAVI_GYRO_SENS_X") ? sens_value : !strcmp(name, "NX2NAVI_GYRO_SENS_Y") ? sens_y
         : !strcmp(name, "NX2NAVI_GYRO_DEADZONE") ? deadzone_value
         : !strcmp(name, "NX2NAVI_GYRO_TIGHTEN") ? tighten_value : 0;
}
int cfg_bool(const char *name) { return strcmp(name, "NX2NAVI_GYRO_TOUCH_HOLD") ? 0 : touch_hold_value; }
int input_motion_enabled(void) { return motion_on; }
int input_direct_mouse_available(void) { return patch_on; }
uint64_t plat_now_ns(void) { return clock_ns; }
void plat_mutex_lock(PlatMutex *m) { (void)m; }
void plat_mutex_unlock(PlatMutex *m) { (void)m; }

#define DEG (M_PI / 180)
static int near(double a, double b, double tol) { return fabs(a - b) <= tol; }

// One second of 250 Hz samples of rate w (rad/s) with this up direction and gravity on the accelerometer, then a take.
static void run(const double w[3], const double up[3], double seconds, double deg[2]) {
    double acc[3] = {-up[0], -up[1], -up[2]};
    for (int i = 0; i < (int)(seconds * 250 + 0.5); i++) {
        clock_ns += 4000000;
        gyro_mouse_feed(acc, w, up, touch, 0.004);
        if (i % 4 == 3) {   // the camera takes at 60-ish Hz; sum what it got
            double d[2];
            gyro_mouse_take(d);
            deg[0] += d[0], deg[1] += d[1];
        }
    }
}

int main(void) {
    double out[2];
    const double flat[3] = {0, 0, 1}, upright[3] = {0, 1, 0};
    const double tilt[3] = {0, sqrt(0.5), sqrt(0.5)};   // 45 degrees between

    // a turn about the vertical, 1 rad/s to the left (counter-clockwise from above), is the same yaw however it is held
    gyro_player_space((const double[3]){0, 0, 1}, flat, out);
    assert(near(out[0], -1, 1e-9) && near(out[1], 0, 1e-9));
    gyro_player_space((const double[3]){0, 1, 0}, upright, out);
    assert(near(out[0], -1, 1e-9));
    gyro_player_space((const double[3]){0, sqrt(0.5), sqrt(0.5)}, tilt, out);
    assert(near(out[0], -1, 1e-9));
    // held flat, a sideways roll (about the top edge's axis) adds yaw, up to the relaxed share of the combined rate
    gyro_player_space((const double[3]){0, 1, 0}, tilt, out);
    assert(near(out[0], -1, 1e-9));                     // let out twice: sqrt(0.5) * 2 passes the combined rate 1
    gyro_player_space((const double[3]){0, 1, 0.3}, tilt, out);
    assert(near(out[0], -hypot(1, 0.3), 1e-9));        // here the combined rate is the limit
    gyro_player_space((const double[3]){0, 0.2, 0}, flat, out);
    assert(near(out[0], 0, 1e-9));    // flat, a pure roll is not a turn about the vertical
    // pitch is the x axis
    gyro_player_space((const double[3]){0.5, 0, 0}, upright, out);
    assert(near(out[1], 0.5, 1e-9) && near(out[0], 0, 1e-9));

    // world space: the whole turn about the vertical (no let-out), and a level pitch axis while rolled sideways
    gyro_space_turn(GYRO_SPACE_WORLD, (const double[3]){0, 1, 0}, tilt, out);
    assert(near(out[0], -sqrt(0.5), 1e-9));                // no let-out, unlike player space
    const double rolled[3] = {sqrt(0.5), 0, sqrt(0.5)};    // rolled 45 degrees sideways
    gyro_space_turn(GYRO_SPACE_WORLD, (const double[3]){0.5 * sqrt(0.5), 0, -0.5 * sqrt(0.5)}, rolled, out);
    assert(near(out[1], 0.5, 1e-9) && near(out[0], 0, 1e-9));   // a tilt about the level axis: all pitch, no turn
    gyro_space_turn(GYRO_SPACE_WORLD, (const double[3]){0.5, 0, 0}, rolled, out);
    assert(near(out[1], 0.5 * sqrt(0.5), 1e-9));               // about the rolled x axis: its level share
    // local spaces: the face's axis (dial), the top edge's axis (wheel), both; pitch the x axis
    gyro_space_turn(GYRO_SPACE_YAW, (const double[3]){0.3, 0.2, 1}, flat, out);
    assert(near(out[0], -1, 1e-9) && near(out[1], 0.3, 1e-9));
    gyro_space_turn(GYRO_SPACE_ROLL, (const double[3]){0, 0.4, 1}, flat, out);
    assert(near(out[0], 0.4, 1e-9));                        // the right side going down turns right
    gyro_space_turn(GYRO_SPACE_YAW_ROLL, (const double[3]){0, 0.4, 1}, flat, out);
    assert(near(out[0], 0.4 - 1, 1e-9));
    // the laser pointer: up and down follow the top edge's elevation, so tilting up 10 degrees and back returns to 0
    // whatever the rate reads (here none at all), and the dead zone and tightening leave it alone
    space_value = "laser", deadzone_value = 5, tighten_value = 5, version++;
    {
        double d[2], sum = 0, acc0[3] = {0, 0, -1}, w0[3] = {0, 0, 0};
        gyro_mouse_take(d);   // a first take: the ones after it count (gyro_mouse_take drops a stale one)
        for (int step = 0; step <= 10; step++) {   // tilt up 10 degrees, one a sample
            double a = step * DEG, up_t[3] = {0, sin(a), cos(a)};
            clock_ns += 4000000, gyro_mouse_feed(acc0, w0, up_t, 0, 0.004);
        }
        gyro_mouse_take(d), sum += d[1];
        assert(near(sum, 10, 1e-6));
        for (int step = 10; step >= 0; step--) {
            double a = step * DEG, up_t[3] = {0, sin(a), cos(a)};
            clock_ns += 4000000, gyro_mouse_feed(acc0, w0, up_t, 0, 0.004);
        }
        gyro_mouse_take(d), sum += d[1];
        assert(near(sum, 0, 1e-6));
    }
    space_value = "player", deadzone_value = 0, tighten_value = 0, version++;

    // the gates
    assert(gyro_mouse_on());
    motion_on = 0, version++;
    assert(!gyro_mouse_on());
    motion_on = 1, patch_on = 0, version++;
    assert(!gyro_mouse_on());
    patch_on = 1, mode = "game", version++;
    assert(!gyro_mouse_on());
    mode = "player", version++;
    assert(gyro_mouse_on() && !gyro_mouse_aim_only());
    scope = "aim", version++;
    assert(gyro_mouse_on() && gyro_mouse_aim_only());
    scope = "always", version++;

    // feed and take: 30 degrees a second to the right for a second, held upright
    double deg[2] = {0, 0}, warm[2] = {0, 0};
    run((const double[3]){0, 0, 0}, upright, 0.1, warm);   // the first take only starts the count
    run((const double[3]){0, -30 * DEG, 0}, upright, 1, deg);
    assert(near(deg[0], 30, 0.5) && near(deg[1], 0, 1e-6));
    // twice the sensitivity, twice the turn; and 20 degrees up
    sens_value = 2, sens_y = 2, version++;
    deg[0] = deg[1] = 0;
    run((const double[3]){20 * DEG, -30 * DEG, 0}, upright, 1, deg);
    assert(near(deg[0], 60, 1) && near(deg[1], 40, 1));
    // the horizontal and vertical sensitivities apply to their own axis alone
    sens_value = 3, sens_y = 0.5, version++;
    deg[0] = deg[1] = 0;
    run((const double[3]){20 * DEG, -30 * DEG, 0}, upright, 1, deg);
    assert(near(deg[0], 90, 1.5) && near(deg[1], 10, 0.5));
    sens_value = 1, sens_y = 1, version++;

    // a still controller with a bias of 1 degree a second: no slow rate is held back, so most of it gets through in the
    // first second (the bias is learnt once it has been still for a second), and it is learnt within a few seconds
    deg[0] = deg[1] = 0;
    run((const double[3]){0, 0, 1 * DEG}, flat, 1, deg);
    assert(deg[0] < -0.6);
    run((const double[3]){0, 0, 1 * DEG}, flat, 8, warm);
    assert(near(bias[2], 1 * DEG, 0.1 * DEG));
    deg[0] = deg[1] = 0;
    run((const double[3]){0, 0, 1 * DEG}, flat, 2, deg);
    assert(fabs(deg[0]) < 0.05);
    // ... and a real turn is not taken for bias
    deg[0] = deg[1] = 0;
    run((const double[3]){0, 0, 1 * DEG + 45 * DEG}, flat, 1, deg);
    assert(near(deg[0], -45, 1) && near(bias[2], 1 * DEG, 0.1 * DEG));

    // a dead zone of 5 degrees a second: a turn of 4 is dropped, a turn of 30 loses 5
    deadzone_value = 5, version++;
    bias[0] = bias[1] = bias[2] = 0, still_s = 0;
    run((const double[3]){0, 0, 0}, upright, 0.1, warm);   // what the last section left pending is taken here
    deg[0] = deg[1] = 0;
    run((const double[3]){0, -4 * DEG, 0}, upright, 1, deg);
    assert(fabs(deg[0]) < 1e-9);
    deg[0] = deg[1] = 0;
    run((const double[3]){0, -30 * DEG, 0}, upright, 1, deg);
    assert(near(deg[0], 25, 0.5));
    deadzone_value = 0, version++;

    // tightening of 2 degrees a second: a turn of 1 goes at half speed, a turn of 30 is untouched; on top of a dead zone
    // of 5 it acts on what is past the dead zone (a turn of 6 leaves 1, halved)
    tighten_value = 2, version++;
    run((const double[3]){0, 0, 0}, upright, 0.1, warm);
    deg[0] = deg[1] = 0;
    run((const double[3]){0, -1 * DEG, 0}, upright, 1, deg);
    assert(near(deg[0], 0.5, 0.05));
    deg[0] = deg[1] = 0;
    run((const double[3]){0, -30 * DEG, 0}, upright, 1, deg);
    assert(near(deg[0], 30, 0.5));
    deadzone_value = 5, version++;
    bias[0] = bias[1] = bias[2] = 0, still_s = 0;   // the turn of 1 was slow enough to count as still and teach bias
    run((const double[3]){0, 0, 0}, upright, 0.1, warm);
    deg[0] = deg[1] = 0;
    run((const double[3]){0, -6 * DEG, 0}, upright, 1, deg);
    assert(near(deg[0], 0.5, 0.05));
    deadzone_value = 0, tighten_value = 0, version++;

    // a finger on the touchpad holds the turn; lifted, the turn goes on; with the setting off the finger does nothing
    touch = 1;
    run((const double[3]){0, 0, 0}, upright, 0.1, warm);
    deg[0] = deg[1] = 0;
    run((const double[3]){0, -30 * DEG, 0}, upright, 1, deg);
    assert(deg[0] == 0 && deg[1] == 0);
    touch = 0;
    run((const double[3]){0, -30 * DEG, 0}, upright, 1, deg);
    assert(near(deg[0], 30, 0.5));
    touch = 1, touch_hold_value = 0, version++;
    deg[0] = deg[1] = 0;
    run((const double[3]){0, -30 * DEG, 0}, upright, 1, deg);
    assert(near(deg[0], 30, 0.5));
    touch = 0, touch_hold_value = 1, version++;

    // nothing takes for a while (a menu): what was turned meanwhile is dropped, not added at once afterwards
    double acc[3] = {0, 0, -1}, w[3] = {0, 0, 1 * DEG + 90 * DEG};
    for (int i = 0; i < 100; i++) clock_ns += 4000000, gyro_mouse_feed(acc, w, flat, 0, 0.004);
    double d[2];
    gyro_mouse_take(d);
    assert(d[0] == 0 && d[1] == 0);

    puts("gyro_mouse_check: ok");
    return 0;
}
