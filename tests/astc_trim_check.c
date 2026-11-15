// runtime/vk_astc.c vk_astc_scratch_trim against the upload path's lock order. Built and run by
// tests/astc_trim_check.py; prints each failed check and exits 1 on any, 2 when two threads stay stuck.
// The upload path takes the upload lock first (vk_memory.c vk_upload_begin: cmd_lock) and the decoder's lock inside it
// (vk_astc_record: g_lock shared for the pipelines, then scratch_get: g_lock). The trim runs at every present and holds
// g_lock, so it may only try cmd_lock (vk_defer_buffer_try): waiting for it there deadlocks with an upload.
// The platform's mutexes are SRW locks as in runtime/platform/win32.c; the recursive cmd_lock is
// runtime/platform/common.c's, linked in. vk_defer_buffer and vk_defer_buffer_try stand in for vk_memory.c's, which
// take cmd_lock as its defer does.
#include "vk_astc.c"

#include <windows.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// ---- the platform: SRW locks and condition variables (runtime/platform/win32.c), thread ids for common.c
void plat_mutex_init(PlatMutex *m) { InitializeSRWLock((PSRWLOCK)m); }
void plat_mutex_lock(PlatMutex *m) { AcquireSRWLockExclusive((PSRWLOCK)m); }
void plat_mutex_lock_at(PlatMutex *m, void *site) { (void)site, AcquireSRWLockExclusive((PSRWLOCK)m); }
int plat_mutex_trylock(PlatMutex *m) { return TryAcquireSRWLockExclusive((PSRWLOCK)m) != 0; }
void plat_mutex_unlock(PlatMutex *m) { ReleaseSRWLockExclusive((PSRWLOCK)m); }
void plat_mutex_lock_shared(PlatMutex *m) { AcquireSRWLockShared((PSRWLOCK)m); }
void plat_mutex_unlock_shared(PlatMutex *m) { ReleaseSRWLockShared((PSRWLOCK)m); }
void plat_cond_init(PlatCond *c) { InitializeConditionVariable((PCONDITION_VARIABLE)c); }
void plat_cond_wait(PlatCond *c, PlatMutex *m) {
    SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, INFINITE, 0);
}
int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    return SleepConditionVariableSRW((PCONDITION_VARIABLE)c, (PSRWLOCK)m, (DWORD)(ns / 1000000), 0) != 0;
}
void plat_cond_broadcast(PlatCond *c) { WakeAllConditionVariable((PCONDITION_VARIABLE)c); }
void plat_cond_signal(PlatCond *c) { WakeConditionVariable((PCONDITION_VARIABLE)c); }
uint32_t plat_thread_id(void) { return (uint32_t)GetCurrentThreadId(); }

// ---- the rest of the runtime vk_astc.c names on this path
static uint64_t fake_now;
uint64_t kern_now_ns(void) { return __atomic_load_n(&fake_now, __ATOMIC_ACQUIRE); }
int cfg_bool(const char *k) { (void)k; return 1; }   // NX2NAVI_ASTC_SCRATCH_KEEP=1, the default
int rt_knob(const char *k, const char *def) { (void)k; return atoi(def); }
void rt_log(const char *fmt, ...) { (void)fmt; }
void vk_texstat_time(int kind, uint64_t t0) { (void)kind, (void)t0; }
static VkCtx ctx;
VkCtx *vk_ctx(void) { return &ctx; }
static uintptr_t next_handle = 0x1000;
VkResult vk_vma_create_buffer(VmaAllocator a, const VkBufferCreateInfo *bi, const VmaAllocationCreateInfo *ai,
                              VkBuffer *b, VmaAllocation *al, VmaAllocationInfo *info, const char *file) {
    (void)a, (void)bi, (void)ai, (void)info, (void)file;
    *b = (VkBuffer)(next_handle += 0x10), *al = (VmaAllocation)(next_handle += 0x10);
    return VK_SUCCESS;
}

// ---- vk_memory.c's deferral: defer appends under cmd_lock (recursive)
static PlatRMutex cmd_lock = PLAT_RMUTEX_INIT;
static volatile LONG deferred;
static void defer_locked(void) {
    plat_rmutex_lock(&cmd_lock);
    InterlockedIncrement(&deferred);
    plat_rmutex_unlock(&cmd_lock);
}
void vk_defer_buffer(VkBuffer buf, VmaAllocation alloc) {
    (void)alloc;
    if (buf) defer_locked();
}
int vk_defer_buffer_try(VkBuffer buf, VmaAllocation alloc) {
    (void)alloc;
    if (!plat_rmutex_trylock(&cmd_lock)) return 0;
    if (buf) defer_locked();
    plat_rmutex_unlock(&cmd_lock);
    return 1;
}

// A large texture grows the scratch (under cmd_lock, as an upload asks for it), then 6 s pass without one.
static void grow(void) {
    int reused;
    plat_rmutex_lock(&cmd_lock);
    scratch_get(&ctx, (VkCommandBuffer)0x2222, 80ull << 20, &reused);
    plat_rmutex_unlock(&cmd_lock);
    __atomic_add_fetch(&fake_now, 6000000000ull, __ATOMIC_ACQ_REL);
}

// ---- (a) another thread holds cmd_lock: the trim returns at once and leaves the scratch
static volatile LONG holding, release_hold, trim_done;
static DWORD WINAPI holder_thread(void *arg) {
    (void)arg;
    plat_rmutex_lock(&cmd_lock);
    InterlockedExchange(&holding, 1);
    while (!release_hold) Sleep(1);
    plat_rmutex_unlock(&cmd_lock);
    return 0;
}
static DWORD WINAPI trim_thread(void *arg) {
    (void)arg;
    vk_astc_scratch_trim();
    InterlockedExchange(&trim_done, 1);
    return 0;
}

// ---- (c) the upload holds cmd_lock, the present's trim comes, then the upload asks for the scratch
static volatile LONG upload_holds, trim_returned, upload_done;
static DWORD WINAPI upload_thread(void *arg) {
    (void)arg;
    plat_rmutex_lock(&cmd_lock);   // vk_upload_begin
    InterlockedExchange(&upload_holds, 1);
    for (int i = 0; i < 200 && !trim_returned; i++) Sleep(1);   // the staging copy: the present's trim comes meanwhile
    Sleep(20);                                                  // a blocking trim is now waiting for cmd_lock
    plat_mutex_lock_shared(&g_lock);                            // vk_astc_record: the pipelines
    plat_mutex_unlock_shared(&g_lock);
    int reused;
    scratch_get(&ctx, (VkCommandBuffer)0x2222, 4u << 20, &reused);
    plat_rmutex_unlock(&cmd_lock);   // vk_upload_end
    InterlockedExchange(&upload_done, 1);
    return 0;
}
static DWORD WINAPI present_thread(void *arg) {
    (void)arg;
    while (!upload_holds) Sleep(1);
    vk_astc_scratch_trim();
    InterlockedExchange(&trim_returned, 1);
    return 0;
}

int main(void) {
    ctx.device = (VkDevice)0x1111;
    __atomic_store_n(&fake_now, 100000000000ull, __ATOMIC_RELEASE);

    // (a)
    grow();
    CHECK(S.size == 96ull << 20, "the large texture left a scratch of %llu MiB, not 96",
          (unsigned long long)(S.size >> 20));
    LONG d0 = deferred;
    HANDLE h = CreateThread(NULL, 0, holder_thread, NULL, 0, NULL);
    while (!holding) Sleep(1);
    HANDLE t = CreateThread(NULL, 0, trim_thread, NULL, 0, NULL);
    DWORD w = WaitForSingleObject(t, 100);
    CHECK(w == WAIT_OBJECT_0, "the trim waited for the upload lock another thread holds");
    InterlockedExchange(&release_hold, 1);
    WaitForSingleObject(t, 3000);
    WaitForSingleObject(h, 3000);
    if (w == WAIT_OBJECT_0) {
        CHECK(S.size == 96ull << 20 && S.buf, "the trim that gave way still dropped the scratch (%llu MiB)",
              (unsigned long long)(S.size >> 20));
        CHECK(n_scratch_trim == 0 && deferred == d0, "the trim that gave way counted %llu trims, %ld deferrals",
              (unsigned long long)n_scratch_trim, deferred - d0);
    }

    // (b) cmd_lock free: the next present's trim goes through
    uint64_t trims = n_scratch_trim;
    d0 = deferred;
    vk_astc_scratch_trim();
    CHECK(S.size == 0 && !S.buf, "with the upload lock free the scratch stayed (%llu MiB)",
          (unsigned long long)(S.size >> 20));
    CHECK(n_scratch_trim == trims + 1 && deferred == d0 + 1, "with the upload lock free: %llu trims, %ld deferrals",
          (unsigned long long)(n_scratch_trim - trims), deferred - d0);

    // (c) the interleaving that deadlocked: both threads end
    grow();
    HANDLE th[2] = {CreateThread(NULL, 0, upload_thread, NULL, 0, NULL),
                    CreateThread(NULL, 0, present_thread, NULL, 0, NULL)};
    if (WaitForMultipleObjects(2, th, TRUE, 3000) == WAIT_TIMEOUT) {
        printf("FAIL: deadlock: after 3 s the upload has not got its scratch (done %ld) and the trim has not ended "
               "(returned %ld)\n", upload_done, trim_returned);
        fflush(stdout);
        ExitProcess(2);
    }
    CHECK(S.buf && S.size == 32ull << 20, "after the upload the scratch is %llu MiB, not the usual 32",
          (unsigned long long)(S.size >> 20));

    if (fails) return printf("astc_trim_check: %d checks failed\n", fails), 1;
    printf("astc_trim_check: all checks passed\n");
    return 0;
}
