"""Build and run tests/astc_trim_check.c: runtime/vk_astc.c vk_astc_scratch_trim, which every present runs, against the
upload path's lock order (cmd_lock, then the decoder's lock). The trim gives way while another thread holds the upload
lock, goes through once it is free, and never deadlocks with an upload that asks for the scratch meanwhile.

No GPU, no game files, any Python. The recursive lock is runtime/platform/common.c's; the rest of the runtime that
vk_astc.c names is stubbed (each stub returns 0), and the compute shaders' SPIR-V headers, when tools/build_shaders.py
has not made them, are stand-ins written to the test's build folder.

Usage: astc_trim_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

# The symbols vk_astc.c names that neither the check nor platform/common.c defines. None runs on the trim's path.
STUBS = ['cfg_get', 'cfg_value', 'plat_mem_info', 'plat_now_ns', 'rc_vkCmdBindPipeline', 'rc_vkCmdCopyBuffer',
         'rc_vkCmdCopyBufferToImage', 'rc_vkCmdDispatch', 'rc_vkCmdPipelineBarrier2', 'rc_vkCmdPushConstants',
         'rc_vkCmdWriteTimestamp2', 'rec_push', 'rt_fatal', 'rt_nvn_api_version', 'vkCreateComputePipelines',
         'vkCreateDescriptorSetLayout', 'vkCreatePipelineLayout', 'vkCreateQueryPool', 'vkCreateShaderModule',
         'vkDestroyDescriptorSetLayout', 'vkDestroyPipeline', 'vkDestroyPipelineLayout', 'vkDestroyQueryPool',
         'vkDestroyShaderModule', 'vkGetPhysicalDeviceFeatures2', 'vkGetPhysicalDeviceFormatProperties',
         'vkGetPhysicalDeviceQueueFamilyProperties', 'vkGetQueryPoolResults', 'vkResetQueryPool', 'vk_crumb', 'vk_dpa',
         'vk_staging', 'vk_vma_destroy_buffer']
SHADERS = ['astc_decode', 'bc3_encode']


def compiler():
    """$CC, else MSYS2 UCRT64's clang, else clang on PATH (as tests/diff/common.py picks it, without its Unicorn)."""
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def vma_include():
    """The folder holding vk_mem_alloc.h: $VMA_INCLUDE, else MSYS2 UCRT64's."""
    if os.environ.get('VMA_INCLUDE'):
        return os.environ['VMA_INCLUDE']
    return str(MSYS_BIN.parent / 'include' / 'vma')


def main():
    if os.name != 'nt':
        print('astc_trim_check: Windows only (the check uses SRW locks and Windows threads); skipped')
        return 0
    work = ROOT / 'build' / 'test' / 'astc_trim'
    work.mkdir(parents=True, exist_ok=True)
    stubs = work / 'stubs.c'
    stubs.write_text('// Stand-ins written by tests/astc_trim_check.py: each returns 0.\n' +
                     ''.join(f'long long {s}() {{ return 0; }}\n' for s in STUBS))
    for name in SHADERS:   # only used when runtime/<name>_spv.h is missing: the runtime folder is searched first
        (work / f'{name}_spv.h').write_text('// A stand-in written by tests/astc_trim_check.py.\n#pragma once\n'
                                            f'#include <stdint.h>\nstatic const uint32_t {name}_spv[] = {{0}};\n')
    exe = work / 'astc_trim_check.exe'
    env = dict(os.environ)
    if MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    rt = ROOT / 'runtime'
    subprocess.check_call([compiler(), '-std=gnu11', '-O1', '-g', '-w', '-fno-strict-aliasing', '-march=x86-64-v3',
                           '-I', str(rt), '-I', str(work), '-I', vma_include(),
                           str(ROOT / 'tests' / 'astc_trim_check.c'), str(rt / 'platform' / 'common.c'), str(stubs),
                           '-o', str(exe), '-lws2_32'], env=env)
    try:
        return subprocess.run([str(exe)], env=env, timeout=60).returncode
    except subprocess.TimeoutExpired:
        print('astc_trim_check: FAIL: the check did not end within 60 s')
        return 1


if __name__ == '__main__':
    sys.exit(main())
