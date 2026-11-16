#!/usr/bin/env bash
# Copyright (c) 2026 JonathanOrr. MIT License: see LICENSE.xplane-fsr3 in this folder. Changed for NX2navi.
# Generates the Vulkan shader permutation headers of the FSR 3.1 upscaler, frame interpolation and optical flow on Linux.
# Mirrors sdk/include/FidelityFX/gpu/<component>/CMakeCompile*Shaders.txt and sdk/include/FidelityFX/gpu/CMakeCompileShaders.txt
# (FidelityFX SDK v1.1.4) for the GLSL path: four header sets per pass (wave32, wave64, 16-bit, wave64 16-bit).
# Usage: gen_shaders.sh <FidelityFX_SC> <sdk dir> <output dir> [component] [pass name filter]
#   component: fsr3upscaler, frameinterpolation, opticalflow or all (the default)
set -euo pipefail
SC=$1; SDK=$2; OUT=$3; WHICH=${4:-all}; FILTER=${5:-}
GPU=$SDK/include/FidelityFX/gpu
mkdir -p "$OUT"
API=(-compiler=glslang -e CS --target-env vulkan1.2 -S comp -Os -DFFX_GLSL=1)

compile() {   # <component> then BASE..., --, PERM...
    local comp=$1; shift
    local base=() perm=()
    while [[ $1 != -- ]]; do base+=("$1"); shift; done; shift
    perm=("$@")
    local inc=(-I"$GPU" -I"$GPU/$comp")
    for shader in "$SDK"/src/backends/vk/shaders/"$comp"/*.glsl; do
        name=$(basename "$shader" .glsl)
        [[ -n "$FILTER" && "$name" != *"$FILTER"* ]] && continue
        for variant in ":0" "_wave64:0" "_16bit:1" "_wave64_16bit:1"; do
            suffix=${variant%%:*}; half=${variant##*:}
            "$SC" "${base[@]}" "${API[@]}" "${perm[@]}" -name="${name}${suffix}" \
                -DFFX_HALF="$half" "${inc[@]}" -output="$OUT" "$shader"
        done
    done
}

if [[ $WHICH == all || $WHICH == fsr3upscaler ]]; then
    compile fsr3upscaler -reflection -deps=gcc -DFFX_GPU=1 \
        -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0 \
        -DFFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0 \
        -DFFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1 \
        -DFFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0 \
        -DFFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2 -- \
        '-DFFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE={0,1}' \
        '-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT={0,1}' \
        '-DFFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS={0,1}' \
        '-DFFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS={0,1}' \
        '-DFFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH={0,1}' \
        '-DFFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING={0,1}'
fi
if [[ $WHICH == all || $WHICH == frameinterpolation ]]; then
    compile frameinterpolation -reflection -deps=gcc -DFFX_GPU=1 \
        -DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0 \
        -DFFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0 \
        -DFFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1 \
        -DFFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0 \
        -DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2 -- \
        '-DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS={0,1}' \
        '-DFFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS={0,1}' \
        '-DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH={0,1}'
fi
if [[ $WHICH == all || $WHICH == opticalflow ]]; then
    compile opticalflow -reflection -deps=gcc -DFFX_GPU=1 -- \
        '-DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT={0,1}'
fi
