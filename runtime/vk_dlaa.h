// DLAA (vk_dlaa.c): NVIDIA DLSS at the scene's own size, behind NX2NAVI_DLAA (nx2navi.ini dlaa = 1).
#pragma once
#include "vk.h"
#include "vk_ffx.h"

// The narrowest scene the DLSS pass and the motion passes recognise by its D32F depth: Ultra Performance at a 1080p
// output renders 640 wide, smaller windows less.
#define DLAA_SCENE_MIN_W 320u

int vk_dlaa_wanted(void);   // the setting is on (the device may still lack DLSS)
// DLSS Frame Generation through NGX itself (vk_dlaa.c vk_ngxfg_*, gfx_present.c's presenter): NX2NAVI_FRAMEGEN = dlss
// where Streamline does not run (every build but the Windows one, where these all answer 0).
typedef struct { VkImage img; VkImageView view; VkFormat fmt; uint32_t w, h; } VkNgxImage;   // in GENERAL
int vk_ngxfg_wanted(void);   // this run can generate frames through NGX (DLSS upscaling on at the start)
int vk_ngxfg_max(void);      // generated frames per rendered one the GPU allows (1 = 2x, 5 = 6x), 0 when unavailable
int vk_ngxfg_on(void);       // ... and NX2NAVI_FRAMEGEN = dlss now (the menu switches it live)
void vk_ngxfg_reset(void);   // the next generation starts without history (a resize, the menu)
// Records count generated frames into out[] and a copy of bb into real; returns how many were made (0: none, and real
// is not written).
int vk_ngxfg_evaluate(VkCommandBuffer cb, const VkNgxImage *bb, const VkNgxImage *hudless, const VkNgxImage *out, int count,
                      const VkNgxImage *real);
int vk_dlaa_fsr3(void);   // the setting is on and its upscaler is AMD FSR 3.1, which loads
int vk_dlaa_needs_restart(void);   // the menu's upscaler cannot start in this run
// Frame generation (vk_fg.cpp): the depth and motion vectors of the frame DLSS or FSR 3.1 last ran on, with its jitter
// and whether its history reset; 1 once per evaluation (0: none since the last call). The images stay valid until the
// next frame's scene draws over them, after this present. Without an upscaler, while frame generation runs, the same
// of the last frame armed for it alone (vk_dlaa.c fg_only_frame): the scene's size, a jitter of 0.
int vk_dlaa_fg_inputs(VkFfxImage *depth, VkFfxImage *mv, float *jx, float *jy, int *reset);
// 1 while frame generation runs without an upscaler and the frame is armed for its inputs alone (vk_dlaa.c
// fg_only_frame): the motion passes run as for the upscaler (vk_usnap.c, vk_exec_draw.c, vk_mvprev.c), nothing is
// jittered. Read at each present.
int vk_dlaa_fg_only(void);
// ... their views, and this frame's camera for DLSS Frame Generation (vk_dlssg.c): matrices for row vectors (v * M, the
// rows stored in order), as Streamline takes them; 0 without a camera that frame.
// znear and zfar: the view distances the depth buffer holds as 0 and 1 (1 and 0 when inverted), from the frame's own
// projection and the device's depth mode (vk_dlaa.c proj_planes); with an infinite far plane (infinite) zfar is
// VK_DLAA_FAR_INFINITE, a distance no surface reaches.
#define VK_DLAA_FAR_INFINITE 1e8f
typedef struct {
    float view_to_clip[16], clip_to_view[16], clip_to_prev_clip[16], prev_clip_to_clip[16];
    float pos[3], up[3], right[3], fwd[3];
    float fov_y, aspect;
    float znear, zfar;
    int infinite, inverted;
} VkDlaaCamera;
int vk_dlaa_fg_views(VkImageView *depth, VkImageView *mv);
// The picture without the HUD at the size of the game's picture, in *fmt: RGBA8 UNORM bytes like it (sRGB-encoded), or
// RGBA16F linear with HDR on; made at each evaluation while vk_dlaa_hudless_wanted (frame generation sets it), from the
// upscaler's picture or, without an upscaler, from the game's own scaled picture; 1 once per evaluation.
extern int vk_dlaa_hudless_wanted;
int vk_dlaa_fg_hudless(VkImage *img, VkImageView *view, uint32_t *w, uint32_t *h, VkFormat *fmt);
int vk_dlaa_fg_camera(VkDlaaCamera *c);     // ... and its temporal upscaler is AMD FSR 3.1 (NX2NAVI_TAA = fsr3) instead of DLSS
int vk_dlaa_ffx_camera(VkFfxCamera *c);     // the same camera as FSR takes it (vk_ffx.h); 0 without one that frame
// vk_device.c: NGX's instance and device extensions join the lists (room for max names); vk_dlaa_init after the
// device exists brings NGX up and checks that DLSS is available.
void vk_dlaa_instance_exts(const char **ext, uint32_t *n, uint32_t max);
void vk_dlaa_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n, uint32_t max);
void vk_dlaa_init(void);
// vk_device.c destroy_all, once the device is idle and before the allocator and the device go (Linux): releases the NGX
// features and parameters, frees every image, buffer, pipeline and query pool DLAA made, resets its state for a later
// device and shuts NGX down for this one. Safe when NGX never came up, when nothing was made, and more than once.
// Windows: does nothing.
void vk_dlaa_release(void);
// 1 when dlaa is on and NGX came up with DLSS (after vk_dlaa_init): a DLSS mode's smaller scene is scaled up by DLSS.
int vk_dlaa_available(void);
// The video memory DLSS reports for its features (NGX's statistics), for the log; -1 while unknown.
int64_t vk_dlaa_vram(void);
// vk_exec.c vk_exec_pass_begin, once the pass's targets are resolved and before its barrier: arms the frame on the
// scene's light target or, at the first quarter-width pass after it, runs DLAA on that target. Each present ends the
// frame (vk_dlaa_frame).
typedef struct VkExec VkExec;
void vk_dlaa_pass(VkExec *ex);
// vk_exec.c vk_exec_pass_begin, in every replay thread, after the pass hooks: with the upscaler run before the bloom to
// the picture's size (NX2NAVI_DLSS_PREBLOOM), the passes after it that draw into the light target or the bloom
// composite's target are moved to images of the picture's size (vk_dlaa.c "before the bloom").
void vk_dlaa_retarget(VkExec *ex);
// The scale from the game's viewport and scissor to such a moved pass; 0 for any other pass (vk_exec.c vk_exec_scissor,
// vk_dlaa_viewport).
int vk_dlaa_target_scale(const VkExec *ex, float *sx, float *sy);
// vk_exec_draw.c untranslated_dispatch_fallback, after the stand-in of the game's own upscaler wrote its output image
// (in GENERAL, of format fmt and w x h): DLSS or FSR runs there on the armed scene and its picture goes over it.
void vk_dlaa_upscaler(VkExec *ex, VkImage out, VkFormat fmt, uint32_t w, uint32_t h);
// 1 while DLSS or FSR 3.1 runs on the scene (the setting is on, the upscaler came up and has not failed): it scales the
// scene itself, and vk_dlaa_upscaler puts its picture over the stand-in's. The thread that records the frame asks.
int vk_dlaa_active(void);
void vk_dlaa_frame(void);
// 1 once, at the present of the first frame whose scene has a new size after a setting changed (vk_dlaa.c).
int vk_dlaa_resized_take(void);
// vk_exec_draw.c, each draw before its fixed state is recorded: notes the scene camera block (vk_dlaa_draw), and moves
// the viewport of the scene's draws by the frame's jitter (vk_dlaa_viewport, when the viewport is recorded).
void vk_dlaa_draw(VkExec *ex);
void vk_dlaa_viewport(VkExec *ex, VkViewport *v);
void vk_dlaa_pass_end(VkExec *ex);       // vk_exec.c, a pass just ended: the edges of a jittered pass's targets are filled
void vk_dlaa_jitter_check(VkExec *ex);   // before each draw's fixed state: records the viewport again where its jitter changes
// vk_mvsurvey.c (NX2NAVI_MV_SURVEY): notes the uniform blocks of each scene draw, beside vk_dlaa_draw; each present
// compares them with the previous frames' and logs a summary now and then.
void vk_mvsurvey_draw(VkExec *ex);
void vk_mvsurvey_frame(void);
void vk_mvprev_draw(VkExec *ex);   // vk_mvprev.c: notes the scene draw's object and bone blocks
// Per-object motion (vk_exec_draw.c): the moving scene draws are listed again during the replay with
// their vertex shader's last-frame variant; vk_dlaa.c mv_pass records them over the camera motion into ex->cb (and
// leaves every binding of the replay to be set again; draws whose camera block is not cam are left out).
// vk_dlaa_prev_camera: last frame's camera block and this frame's jitter, 0 when there is no history.
int vk_mvobj_execute(VkExec *ex, uint64_t cam, VkImageView mv, VkImageView depth, VkFormat depth_fmt, uint32_t w,
                     uint32_t h);
int vk_dlaa_prev_camera(VkDescriptorBufferInfo *out, float jitter[2]);
// vk_mvcheck.c (NX2NAVI_MV_CHECK, test): mv_pass hands it the motion image after the camera pass and, after the object
// pass, the motion image again and the scene light target it ran on; it measures how well the vectors fit the picture.
void vk_mvcheck_camera(VkCommandBuffer cb, VkImage mv, uint32_t w, uint32_t h, float jx, float jy);
void vk_mvcheck_final(VkCommandBuffer cb, uint64_t fence, VkImage mv, VkImage cimg, VkFormat cfmt, uint32_t level,
                      uint32_t layer, uint32_t w, uint32_t h, float jx, float jy);
const char *vk_mvobj_frame_masks(void);   // vk_exec_draw.c: the last motion list, per position and changed banks
void vk_mvcheck_output(VkCommandBuffer cb, VkImage out, uint32_t ow, uint32_t oh);   // after DLSS on a check frame
// vk_exec_draw.c, once a draw's fragment shader is known: 1 skips the draw (the game's FXAA while NX2NAVI_FXAA is 0, its
// scaling draw when DLSS wrote the scaled picture).
int vk_dlaa_skip_draw(VkExec *ex, const GfxShader *fs);

int vk_dlaa_pose_depth(uint64_t frame, uint32_t w, uint32_t h, VkDescriptorImageInfo *image, float history[4]);
