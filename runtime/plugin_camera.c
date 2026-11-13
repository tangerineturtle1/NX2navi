// The scene's camera for plugins (plugin_api.h camera, world_to_screen; docs/plugins.md "The camera"): where the
// game's main view looks from, so that a plugin can put a name over a character or a marker on a place.
//
// How (game 1.1.1; the address and offsets are in plugin_game_ui.c). The game updates each of its cameras once a frame
// through one function, whose argument is the camera's state: the camera's actor (by whose name the cameras are told
// apart), the position, the target and the up vector the update settled on, and a perspective projection (near, far,
// vertical angle, width / height) that the renderer builds its matrices from. A hook of that function, registered at
// a plugin's first question, copies those numbers after the update ran. Two cameras belong to the scene: "PlayerCamera"
// follows Link and "EventCamera" is the one the game's events direct; in play the event camera holds the player
// camera's view, so the event camera's numbers are used whenever it was updated lately, else the player camera's.
// The ratio is read from the projection, not from the camera: it is the one the widescreen patch gave the renderer.
//
// The numbers are those of the frame the game is working on, which is the frame after the one in the window: a marker
// trails a fast turn of the camera by that one frame.
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "host.h"
#include "platform/platform.h"
#include "plugin_game_ui.h"
#include "plugins.h"

typedef struct { float pos[3], at[3], up[3], near_plane, far_plane, fovy, aspect; uint64_t ns; } View;
enum { CAM_PLAYER, CAM_EVENT, CAMS, STATES = 8 };
static const char *const cam_name[CAMS] = {"PlayerCamera", "EventCamera"};
#define FRESH_NS 2000000000ull  // a camera whose last update is older has left the scene (a loading screen)

static PlatMutex lock = PLAT_MUTEX_INIT;
static View views[CAMS];                          // lock
static struct { uint64_t state, actor; int cam; } known[STATES];   // lock: the camera states seen; cam -1 not the scene's
static const Nx2PluginApi *api;
static const PluginGameUi *G;
static int hooked;                                // 0 not tried, 1 hooked, -1 not possible
static float pic[4];                              // lock: the game picture's rectangle in the last overlay (x, y, w, h)

void plugin_camera_overlay(const Nx2Draw *d) {
    float p[4] = {d->picture_x, d->picture_y, d->picture_width, d->picture_height};
    plat_mutex_lock(&lock);
    memcpy(pic, p, sizeof pic);
    plat_mutex_unlock(&lock);
}

static int finite3(const float *v) { return v[0] - v[0] == 0 && v[1] - v[1] == 0 && v[2] - v[2] == 0; }

// The camera update's hook, on the game's thread that updates the camera: the update first, then its result.
static void h_update(Nx2HookCall *c, void *user) {
    (void)user;
    uint64_t state = c->x[0], actor = 0, lens = 0, proj = 0;
    api->call_original(c);
    if (!api->read(state + G->cam_actor, &actor, 8) || !actor) return;
    plat_mutex_lock(&lock);
    int i = 0, cam = -1, have;
    while (i < STATES && known[i].state && (known[i].state != state || known[i].actor != actor)) i++;
    if (i == STATES) i = (int)((state >> 4) % STATES);      // more states than places: one of them is looked up again
    have = known[i].state == state && known[i].actor == actor;
    if (have) cam = known[i].cam;
    plat_mutex_unlock(&lock);
    if (!have) {
        char name[16] = "";
        uint64_t np = 0;
        if (api->read(actor + G->actor_name, &np, 8) && np) api->read(np, name, sizeof name - 1);
        for (int k = 0; k < CAMS; k++)
            if (!strcmp(name, cam_name[k])) cam = k;
        plat_mutex_lock(&lock);
        known[i].state = state, known[i].actor = actor, known[i].cam = cam;
        plat_mutex_unlock(&lock);
    }
    if (cam < 0) return;
    View v = {.ns = plat_now_ns()};
    if (!api->read(state + G->cam_pos, v.pos, 12) || !api->read(state + G->cam_at, v.at, 12) ||
        !api->read(state + G->cam_up, v.up, 12) || !api->read(state + G->cam_lens, &lens, 8) || !lens ||
        !api->read(lens + G->lens_proj, &proj, 8) || !proj || !api->read(proj + G->proj_near, &v.near_plane, 4) ||
        !api->read(proj + G->proj_far, &v.far_plane, 4) || !api->read(proj + G->proj_fovy, &v.fovy, 4) ||
        !api->read(proj + G->proj_aspect, &v.aspect, 4))
        return;
    if (!finite3(v.pos) || !finite3(v.at) || !finite3(v.up) || !(v.fovy > 0.01f && v.fovy < 3.1f) ||
        !(v.aspect > 0.1f && v.aspect < 10) || !(v.near_plane > 0) || !(v.far_plane > v.near_plane))
        return;
    plat_mutex_lock(&lock);
    views[cam] = v;
    plat_mutex_unlock(&lock);
}

static int ready(void) {
    int h = __atomic_load_n(&hooked, __ATOMIC_ACQUIRE);
    if (h) return h > 0;
    plat_mutex_lock(&lock);
    if (!hooked) {
        api = nx2navi_plugin_api(NX2_PLUGIN_API_VERSION), G = plugin_game_ui();
        int ok = G && G->cam_update && api->hook_function(api->game_info()->main_base + G->cam_update, h_update, NULL);
        if (ok) rt_log("plugins: camera: the game's camera update is hooked for a plugin's camera questions");
        else rt_log("plugins: camera: not available (no table for this build of the game, or no free hook)");
        __atomic_store_n(&hooked, ok ? 1 : -1, __ATOMIC_RELEASE);
    }
    h = hooked;
    plat_mutex_unlock(&lock);
    return h > 0;
}

// The scene's view now, and the picture's rectangle; 0 when no camera of the scene was updated lately.
static int view_now(View *out, float rect[4]) {
    if (!ready()) return 0;
    uint64_t now = plat_now_ns();
    plat_mutex_lock(&lock);
    int cam = views[CAM_EVENT].ns && now - views[CAM_EVENT].ns < FRESH_NS ? CAM_EVENT :
              views[CAM_PLAYER].ns && now - views[CAM_PLAYER].ns < FRESH_NS ? CAM_PLAYER : -1;
    if (cam >= 0) *out = views[cam];
    if (rect) memcpy(rect, pic, sizeof pic);
    plat_mutex_unlock(&lock);
    return cam >= 0;
}

// camera_reset (plugin_api.h): the follow camera goes behind the player when the player presses the game's camera
// button, so the runtime presses it: plugin_camera_reset notes the wish, and the next reads of the controls that reach
// the game (input.c input_poll, not while the runtime's menu or a plugin holds the input) carry the button for
// RESET_NS, long enough for the game to see a press at 20 frames a second and short enough to stay a tap.
#define RESET_NS 160000000ull
static int reset_asked;
static uint64_t reset_until;      // the controls' thread only

int plugin_camera_reset(void) {
    const PluginGameUi *g = plugin_game_ui();
    if (!g || !g->cam_reset_button) return 0;
    __atomic_store_n(&reset_asked, 1, __ATOMIC_RELEASE);
    return 1;
}

void plugin_camera_poll(uint64_t *buttons) {
    if (__atomic_load_n(&reset_asked, __ATOMIC_RELAXED) && __atomic_exchange_n(&reset_asked, 0, __ATOMIC_ACQ_REL))
        reset_until = plat_now_ns() + RESET_NS;
    if (!reset_until) return;
    if (plat_now_ns() < reset_until) *buttons |= 1ull << plugin_game_ui()->cam_reset_button;
    else reset_until = 0;
}

static void cross(const float *a, const float *b, float *o) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
}

static int unit(float *v) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(l > 1e-6f)) return 0;
    v[0] /= l, v[1] /= l, v[2] /= l;
    return 1;
}

// The camera's axes in the world: right, up and back (it looks along -back). 0 for a view without a direction.
static int axes(const View *v, float r[3], float u[3], float b[3]) {
    for (int i = 0; i < 3; i++) b[i] = v->pos[i] - v->at[i];
    if (!unit(b)) return 0;
    cross(v->up, b, r);
    if (!unit(r)) return 0;
    cross(b, r, u);
    return 1;
}

int plugin_camera_get(Nx2Camera *out) {
    View v;
    float r[3], u[3], b[3];
    if (!out || out->size < offsetof(Nx2Camera, position) || !view_now(&v, NULL) || !axes(&v, r, u, b)) return 0;
    Nx2Camera c = {.near_plane = v.near_plane, .far_plane = v.far_plane, .fov_y = v.fovy, .aspect = v.aspect};
    memcpy(c.position, v.pos, 12), memcpy(c.target, v.at, 12), memcpy(c.up, u, 12);
    const float *row[3] = {r, u, b};
    for (int i = 0; i < 3; i++) {
        for (int k = 0; k < 3; k++) c.view[4 * i + k] = row[i][k];
        c.view[4 * i + 3] = -(row[i][0] * v.pos[0] + row[i][1] * v.pos[1] + row[i][2] * v.pos[2]);
    }
    c.view[15] = 1;
    float f = 1 / tanf(v.fovy * 0.5f), n = v.near_plane, far = v.far_plane;
    c.projection[0] = f / v.aspect, c.projection[5] = f;
    c.projection[10] = (far + n) / (n - far), c.projection[11] = 2 * far * n / (n - far), c.projection[14] = -1;
    c.size = out->size < sizeof c ? out->size : (uint32_t)sizeof c;
    memcpy(out, &c, c.size);
    return 1;
}

int plugin_camera_to_screen(const float world[3], float out_xy[2]) {
    View v;
    float r[3], u[3], b[3], rect[4];
    if (!world || !out_xy || !finite3(world) || !view_now(&v, rect) || !axes(&v, r, u, b) || !(rect[2] > 0 && rect[3] > 0))
        return 0;
    float d[3] = {world[0] - v.pos[0], world[1] - v.pos[1], world[2] - v.pos[2]};
    float x = r[0] * d[0] + r[1] * d[1] + r[2] * d[2], y = u[0] * d[0] + u[1] * d[1] + u[2] * d[2],
          depth = -(b[0] * d[0] + b[1] * d[1] + b[2] * d[2]);
    if (!(depth > v.near_plane)) return 0;     // behind the camera, or nearer than it draws
    float t = tanf(v.fovy * 0.5f) * depth;
    out_xy[0] = rect[0] + (x / (t * v.aspect) * 0.5f + 0.5f) * rect[2];
    out_xy[1] = rect[1] + (0.5f - y / t * 0.5f) * rect[3];
    return 1;
}
