"""Exercise production compute creation with a fake driver: reuse must never suppress a required live pipeline."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/vk_pipeline.c').read_text()
start = source.index('typedef struct {\n    uint8_t code[32];\n    VkPipelineLayout layout;')
end = source.index('// The layout of a compute shader in *B;', start)
production = source[start:end]
harness = r'''
#include <windows.h>
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "sha256.h"
typedef SRWLOCK PlatMutex;
#define PLAT_MUTEX_INIT SRWLOCK_INIT
#define FNV0 0xcbf29ce484222325ull
#define FB_COMPUTE 0
typedef struct { uint32_t code[8]; } GfxShader;
typedef struct { VkPipelineLayout layout; } Layout;
static struct { int f_full_subgroups, feedback; VkDevice device; VkPipelineCache cache; } P;
static struct { int sg_size; } vk_caps;
static int cc_hit, pb_last, calls, modules, fail_next, disabled, override_word;
static uint64_t mm_hash;
static VkPipelineCreateFlags test_flags;
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    for (size_t i=0;i<n;i++) h=(h^((const unsigned char *)p)[i])*0x100000001b3ull;
    return h;
}
static void plat_mutex_lock(PlatMutex *m) { AcquireSRWLockExclusive(m); }
static void plat_mutex_unlock(PlatMutex *m) { ReleaseSRWLockExclusive(m); }
static void rt_sha256(const void *p, size_t n, uint8_t out[32]) { sha256(p,n,out); }
static int pins;   // gfx_shader_spirv_pin minus gfx_shader_spirv_unpin: every exit of compute_create unpins
static const uint32_t *gfx_shader_spirv_pin(const GfxShader *s, uint64_t *n) { pins++;*n=8;return s->code; }
static void gfx_shader_spirv_unpin(const GfxShader *s) { pins--; }
static uint32_t *spv_override(const uint32_t *code, uint64_t n, uint64_t *out) {
    if (!override_word) return NULL;
    uint32_t *p=malloc(n*4);memcpy(p,code,n*4);p[7]=override_word;*out=n;return p;
}
static uint32_t *spirv_fog_keep_froxels(const uint32_t *code, uint64_t n) { return NULL; }
static VkShaderModule make_module(const uint32_t *code, uint64_t n, int stage) {
    modules++;return (VkShaderModule)(uintptr_t)modules;
}
VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice d,VkShaderModule m,const VkAllocationCallbacks *a) { modules--; }
static VkPipelineCreateFlags noopt_flag(void) { return test_flags; }
static void pin_subgroup32(VkPipelineShaderStageCreateInfo *ci, VkPipelineShaderStageRequiredSubgroupSizeCreateInfo *r,
                          const uint32_t *w,uint64_t n) {}
static uint32_t local_size_x(const uint32_t *w,uint64_t n) { return 32; }
static int uses_subgroup_ops(const uint32_t *w,uint64_t n) { return 1; }
static const char *cfg_get(const char *k) { return disabled ? "0" : NULL; }
static void *fb_chain(VkPipelineCreationFeedbackCreateInfo *c,VkPipelineCreationFeedback *f,const void *next) {
    memset(f,0,sizeof *f);return NULL;
}
static void fb_note(int kind, const VkPipelineCreationFeedback *f) {}
static void crash_note(const char *w,const GfxShader *s,const GfxShader *b,const GfxShader *c) {}
static void plat_crash_context(const char *c) {}
static void rt_log(const char *fmt,...) {}
static VkResult pipe_create(int kind, void *info, VkPipeline *out) {
    calls++;
    if (fail_next) { fail_next=0;*out=VK_NULL_HANDLE;return VK_ERROR_UNKNOWN; }
    *out=(VkPipeline)(uintptr_t)calls;return VK_SUCCESS;
}
'''
harness += production
harness += r'''
int main(void) {
    GfxShader a={{0x07230203,1,2,3,4,5,6,7}}, b=a;
    Layout l={(VkPipelineLayout)(uintptr_t)1}, other={(VkPipelineLayout)(uintptr_t)2};
    int reused=-1;
    assert(compute_create(&a,&l,0,&reused) && !reused && calls==1);
    assert(!compute_create(&b,&l,0,&reused) && reused && calls==1); // distinct shader objects, same request
    assert(compute_create(&b,&l,0,NULL) && calls==2); // real dispatch still obtains a handle
    assert(compute_create(&b,&other,0,&reused) && !reused && calls==3);
    test_flags=VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
    assert(compute_create(&b,&l,0,&reused) && !reused && calls==4);
    test_flags=0;b.code[7]++;
    fail_next=1;
    assert(!compute_create(&b,&l,0,&reused) && !reused && calls==5);
    assert(compute_create(&b,&l,0,&reused) && !reused && calls==6); // failure is not cached
    assert(!compute_create(&b,&l,0,&reused) && reused && calls==6);
    assert(compute_create(&b,&l,1,&reused) && !reused && calls==7); // cache-only creation flags differ
    override_word=19;
    assert(compute_create(&a,&l,0,&reused) && !reused && calls==8); // final code, not original code
    override_word=20;
    assert(compute_create(&a,&l,0,&reused) && !reused && calls==9);
    override_word=19;
    assert(!compute_create(&a,&l,0,&reused) && reused && calls==9);
    override_word=0;P.f_full_subgroups=1;vk_caps.sg_size=32;
    assert(compute_create(&a,&l,0,&reused) && !reused && calls==10);
    disabled=1;
    assert(compute_create(&a,&l,0,&reused) && !reused && calls==11);
    disabled=0;
    assert(!compute_create(&a,&l,0,&reused) && reused && calls==11);
    compute_warm_clear();
    assert(compute_create(&a,&l,0,&reused) && !reused && calls==12);
    compute_warm_clear();
    assert(modules==0 && pins==0);
    puts("compute warm reuse: equivalent requests, live handles, layout/flags/code separation, failures and cleanup passed");
    return 0;
}
'''
out = root / 'build/test/compute_warm_reuse'
out.mkdir(parents=True, exist_ok=True)
c = out / 'check.c'
c.write_text(harness)
clang = Path(sys.argv[1]) if len(sys.argv) > 1 else Path('C:/msys64/ucrt64/bin/clang.exe')
exe = out / 'check.exe'
subprocess.run([str(clang), '-std=gnu11', '-O1', '-I', str(root / 'runtime'), str(c),
                str(root / 'runtime/sha256.c'), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
