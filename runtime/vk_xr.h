// VR through OpenXR (vk_xr.cpp), NX2NAVI_VR=1 at start. Off, the functions below do nothing and make no OpenXR call.
#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// vk_device.c create_instance: the OpenXR instance and the headset found, and the Vulkan instance extensions the runtime
// needs appended to ext[*n..cap)
void vk_xr_instance_exts(const char **ext, uint32_t *n, uint32_t cap);
// vk_device.c pick_device: the GPU the headset's runtime draws with (VK_NULL_HANDLE: no VR), and the one picked: VR goes
// off when they differ
VkPhysicalDevice vk_xr_device(VkInstance instance);
void vk_xr_device_picked(VkPhysicalDevice picked);
// vk_device.c create_device: the device extensions the runtime needs appended to ext[*n..cap), those the device has
void vk_xr_device_exts(const VkExtensionProperties *have, uint32_t n_have, const char **ext, uint32_t *n, uint32_t cap);
// vk_device.c create_device: the index of the graphics family's queue VR copies on, first_free when the family has it
// (the device is then made with one queue more), else UINT32_MAX and VR off
uint32_t vk_xr_queue(uint32_t family_queues, uint32_t first_free);
// vk_device.c, the device made: VR's queue and semaphore, and the session's thread started
void vk_xr_device_ready(VkInstance instance, VkPhysicalDevice phys, VkDevice device, uint32_t family);

// The head's view (NX2NAVI_VR_VIEW=head): the game's camera follows the headset. vr_game.c switches its camera hooks
// on when this is 1 at start
int vk_xr_head_wanted(void);
// Depth stereo (NX2NAVI_VR_DEPTH above 0 with the head's view): each eye's picture made from the game's one and its
// depth; vk_dlaa.c makes the scene's depth and planes ready for each present while this is 1
int vk_xr_depth_wanted(void);
// vk_dlaa.c, once the scene is drawn and before later passes reuse its depth target: the scene's depth (a sampled
// view, in GENERAL) copied for this frame's depth stereo; binds a compute pipeline and push descriptors of its own
void vk_xr_keep_depth(VkCommandBuffer cb, VkImageView depth, uint32_t w, uint32_t h);
// vk_dlaa.c, with the depth: the view the scene was drawn with (rows 0-2 of its 3x4 view: right, up, back, each with
// its translation), so the picture is matched to the camera update that gave it (the game draws a frame behind them)
void vk_xr_scene_camera(const float rows[12]);
// The head's view is on now (the session running, the menu closed): vr_game.c's first person applies
int vk_xr_head_active(void);
// vr_game.c, the gameplay camera's update: 1 when the head's view applies now, the camera's position, target and up
// (game units) then turned and moved by the head (its yaw kept, the horizon level); the pose is kept for the picture
// drawn from it
int vk_xr_camera(double pos[3], double at[3], float up[3]);
// Stereo Accurate (frame_rate.c): 1 while the game's time is held for every second update, the right eye's
int vk_xr_holds_time(void);
// vr_game.c, before vk_xr_camera: the game seconds the update running now steps (-1 unknown), which tells Stereo
// Accurate's eye: the held update draws the right one
void vk_xr_update_step(double seconds);
// vr_game.c, the same update's angle load: 1 and the vertical angle (radians) the headset's lens needs, after a
// vk_xr_camera that applied
int vk_xr_lens(float *angle);
// input.c, each pad read: the hand controllers' buttons ORed onto *buttons (input_map.h bits) and each stick
// (left[2], right[2]: x, y, up +y, -32767..32767) replaced by the controller's where it is pushed farther; 1 when a
// controller is in use now
int vk_xr_pad(uint64_t *buttons, int32_t left[2], int32_t right[2]);
// ui/menu.cpp, each frame: the wheel open (NX2NAVI_VR_CONTROLS=meta): 1 the D-pad's (the left trigger held), 2 the
// items' (the right trigger), 0 none; *slot the slot that hand's stick is held toward (0 up, clockwise; -1 none)
int vk_xr_wheel(int *slot);
// vr_game.c, the game's thread: a wheel's slot used since the last call (kind as vk_xr_wheel's), 0 none; and whether
// a wheel may open now (not while the game's own menus pause it)
int vk_xr_wheel_used(int *kind, int *slot);
void vk_xr_wheel_allow(int allow);
// vr_game.c, the game's thread: Link plays the ocarina now (the controllers' notes then on the right stick)
void vk_xr_ocarina(int on);
// vr_game.c, the game's thread: Link holds his sword and shield (or another weapon) out now, so the draw gesture puts
// them away
void vk_xr_weapon_out(int out);
// vr_game.c, each frame: Link is a child (VR's hands draw his own for his age on a hand that holds nothing)
void vk_xr_child(int child);
// vr_game.c, the game's thread: the sword and shield equipped, by their actors' names ("" none)
void vk_xr_gear(const char *sword, const char *shield);
// vr_game.c, the game's thread: the item Link uses now, by its model (vr_model.c's name), "" none
void vk_xr_hand_item(const char *model);
// vr_game.c, the game's thread: in play now (not a menu, a pause, an event), the Touch controllers' play roles on
void vk_xr_in_play(int on);
// vr_game.c, the gameplay camera's update: while the controller steers the aim, the view's base yaw held (on); and
// where the sword hand's controller points, a unit direction in the game's world; 0 when its pose is not known
void vk_xr_aim_freeze(int on);
int vk_xr_pointer(double dir[3]);
// vr_game.c, the gameplay camera's update before vk_xr_camera: Link aims (on) along the game's own camera's ray
// (origin, unit direction; game units): the head's view draws the reticle where it lands
void vk_xr_aim(int on, const double origin[3], const double dir[3]);
// The game's own view this frame (the head's view not applied); its pictures go on the screen
void vk_xr_untracked(void);

// vk_exec_draw.c untranslated_dispatch_fallback, the game's scene upscaled into target (w x h, format), the HUD still to
// be drawn over it: the image VR keeps it in (the caller copies it there in its stream), VK_NULL_HANDLE when the HUD's
// panel is not made now. *init 1: the image is new (UNDEFINED; the caller makes it GENERAL first)
VkImage vk_xr_scene_image(VkImage target, uint32_t w, uint32_t h, VkFormat format, int *init);

// gfx_present.c present_image: 1 when VR takes this present's window image (then in TRANSFER_SRC_OPTIMAL for
// vk_xr_capture); 0 records nothing
int vk_xr_wants_capture(int hdr);
// The game's picture (picture, its layout and rect {x, y, w, h}) when it was drawn from the head, else the window's
// image, copied into one of VR's own images; *signal: what the present's submit signals for it
void vk_xr_capture(VkCommandBuffer cb, VkImage window, VkExtent2D extent, VkFormat format, VkImage picture,
                   VkImageLayout picture_layout, const int rect[4], VkSemaphoreSubmitInfo *signal);
// The present's submit went out with that signal: the image is the newest
void vk_xr_captured(const VkSemaphoreSubmitInfo *signal);

#ifdef __cplusplus
}
#endif
