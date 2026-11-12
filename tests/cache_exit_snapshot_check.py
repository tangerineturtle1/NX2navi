"""Windows: the pipeline cache save at exit holds new builds back, waits for the running ones, and copies the cache only
while no build runs; the autosave never holds a build back. Builds runtime/vk_pipeline.c's cache_build_enter and
cache_save_one into a test program against stand-ins for the driver and the file system.
Usage: python tests/cache_exit_snapshot_check.py [path to clang]"""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/vk_pipeline.c').read_text()


def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


harness = r'''
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
typedef SRWLOCK PlatMutex;
typedef void *VkPipelineCache;
typedef int VkResult;
#define VK_SUCCESS 0
static size_t mock_size = 4;
static int query_fail;
static PlatMutex cache_rw = SRWLOCK_INIT;
static volatile int32_t cache_saving_at_exit;
static volatile LONG active, snapshots;
static HANDLE old_in, release_old, new_in, file_in, release_file;
static int block_file, fail_file, writes, save_result;
static struct {
    VkPipelineCache cache, play;
    int device, fold, feedback;
    const char *cache_path, *play_path;
    uint64_t saved[2], cache_loaded, play_loaded;
} P;
static void plat_mutex_lock_shared(PlatMutex *m) { AcquireSRWLockShared(m); }
static void plat_mutex_unlock_shared(PlatMutex *m) { ReleaseSRWLockShared(m); }
static int plat_mutex_trylock(PlatMutex *m) { return TryAcquireSRWLockExclusive(m); }
static void plat_mutex_unlock(PlatMutex *m) { ReleaseSRWLockExclusive(m); }
static void plat_sleep_ms(uint32_t ms) { Sleep(ms); }
static uint64_t plat_now_ms(void) { return GetTickCount64(); }
static void rt_log(const char *fmt, ...) { (void)fmt; }
static void fb_log(const char *when) { (void)when; assert(!cache_saving_at_exit); }
static void plat_file_delete(const char *path) { (void)path; }
static int plat_file_write_atomic(const char *path, const void *data, size_t size) {
    (void)path;
    assert(!cache_saving_at_exit);   // builds go on before the file write
    assert(size == mock_size && !memcmp(data, "test", 4));
    writes++;
    if (block_file) {
        SetEvent(file_in);
        assert(WaitForSingleObject(release_file, 2000) == WAIT_OBJECT_0);
    }
    return !fail_file;
}
static int vkGetPipelineCacheData(int device, VkPipelineCache cache, size_t *size, void *data) {
    (void)device; (void)cache;
    assert(!active);   // never a copy while a build runs, on any driver
    InterlockedIncrement(&snapshots);
    if (!data && query_fail) return -1;
    if (data) { assert(*size >= mock_size); memcpy(data, "test", 4); }
    *size = mock_size;
    return VK_SUCCESS;
}
'''
harness += function('static void cache_build_enter(void)') + '\n'
harness += function('static int cache_save_one(int which, uint32_t wait_ms, int exiting)') + '\n'
harness += r'''
static DWORD WINAPI builder(void *old) {
    cache_build_enter();
    InterlockedIncrement(&active);
    SetEvent(old ? old_in : new_in);
    if (old) assert(WaitForSingleObject(release_old, 2000) == WAIT_OBJECT_0);
    InterlockedDecrement(&active);
    plat_mutex_unlock_shared(&cache_rw);
    return 0;
}
static DWORD WINAPI saver(void *unused) {
    (void)unused;
    save_result = cache_save_one(0, 2000, 1);
    return 0;
}
static void join(HANDLE t) { assert(WaitForSingleObject(t, 2000) == WAIT_OBJECT_0); CloseHandle(t); }
int main(void) {
    P.cache = (void *)1; P.cache_path = "unused"; P.play_path = "unused-play"; P.feedback = 1;
    old_in = CreateEventW(NULL, TRUE, FALSE, NULL);
    release_old = CreateEventW(NULL, TRUE, FALSE, NULL);
    new_in = CreateEventW(NULL, TRUE, FALSE, NULL);
    file_in = CreateEventW(NULL, TRUE, FALSE, NULL);
    release_file = CreateEventW(NULL, TRUE, FALSE, NULL);

    // The save at exit: a build is running; a build that comes while the save waits is held back until the copy is
    // made, and goes on before the file is written.
    HANDLE old = CreateThread(NULL, 0, builder, (void *)1, 0, NULL);
    assert(WaitForSingleObject(old_in, 2000) == WAIT_OBJECT_0);
    block_file = 1;
    HANDLE saving = CreateThread(NULL, 0, saver, NULL, 0, NULL);
    uint64_t until = GetTickCount64() + 2000;
    while (!__atomic_load_n(&cache_saving_at_exit, __ATOMIC_ACQUIRE)) {
        assert(GetTickCount64() < until); Sleep(1);
    }
    HANDLE next = CreateThread(NULL, 0, builder, NULL, 0, NULL);
    assert(WaitForSingleObject(new_in, 20) == WAIT_TIMEOUT);
    SetEvent(release_old);
    assert(WaitForSingleObject(file_in, 2000) == WAIT_OBJECT_0);
    assert(WaitForSingleObject(new_in, 2000) == WAIT_OBJECT_0);
    SetEvent(release_file);
    join(old); join(next); join(saving);
    assert(save_result == 1 && snapshots == 2 && writes == 1 && !cache_saving_at_exit);

    // The autosave does not wait for a running build and holds none back.
    cache_build_enter();
    assert(cache_save_one(0, 0, 0) == 0 && !cache_saving_at_exit);
    // The save at exit gives up on a build that outlasts its wait, and lets builds in again.
    assert(cache_save_one(0, 1, 1) == 0 && !cache_saving_at_exit);
    plat_mutex_unlock_shared(&cache_rw);
    cache_build_enter(); plat_mutex_unlock_shared(&cache_rw);

    // Data of the size already saved is not written again.
    block_file = 0;
    assert(cache_save_one(0, 100, 1) == 1 && writes == 1);
    // A failed write or size query keeps the file and the size it was saved at.
    P.saved[0] = 0; fail_file = 1;
    assert(cache_save_one(0, 100, 1) == -1 && !cache_saving_at_exit && P.saved[0] == 0);
    fail_file = 0; P.saved[0] = mock_size; query_fail = 1;
    int before = writes;
    assert(cache_save_one(0, 100, 1) == -1 && writes == before && P.saved[0] == mock_size);
    query_fail = 0;

    // A merge holds the cache exclusively: the save at exit waits for it like for a build.
    assert(plat_mutex_trylock(&cache_rw));
    assert(cache_save_one(0, 0, 1) == 0 && writes == before && !cache_saving_at_exit);
    plat_mutex_unlock(&cache_rw);
    puts("cache save at exit: builds held back and waited for, autosave unchanged, timeout, failures and merges passed");
    return 0;
}
'''
out = root / 'build/test/cache-exit-snapshot'
out.mkdir(parents=True, exist_ok=True)
(out / 'check.c').write_text(harness)
cc = sys.argv[1] if len(sys.argv) > 1 else 'clang'
subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', str(out/'check.c'),
                '-o', str(out/'check.exe')], check=True)
subprocess.run([str(out/'check.exe')], check=True, timeout=15)
