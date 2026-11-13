// The build options of the FSR 3.1 upscaler passes (runtime/shaders/fsr3_*.comp, runtime/vk_fsr3.c), included after
// #version, for the inputs runtime/vk_dlaa.c hands over: linear HDR scene light, motion vectors at the render size and
// without the jitter, depth not inverted. Everything runs in 32-bit floats (no 16-bit arithmetic); the reprojection's
// Lanczos weights come from the 128-entry table vk_fsr3.c make_shared uploads.
#define FFX_GPU 1
#define FFX_GLSL 1

#define FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF 0
#define FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF 0
#define FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF 1
#define FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF 0
#define FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE 2

#define FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE 1
#define FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT 1
#define FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS 1
#define FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS 0
#define FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH 0
#ifndef FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING
#define FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING 0
#endif
