// The Win32 calls AMD's Vulkan frame interpolation swapchain (sdk/src/backends/vk/FrameInterpolationSwapchain, FidelityFX
// SDK v1.1.4) makes, on POSIX threads, for the Linux build of libamd_fidelityfx_vk.so. AMD's own note in
// FrameInterpolationSwapchainVK.h: "this needs to be ported to standard C++ or other platform if necessary". This folder
// is on the include path of the Linux build only, in the place of the Windows SDK's headers, so AMD's sources stay as
// they are. Only what the swapchain uses is here:
//   - critical sections: recursive mutexes, as Win32's are;
//   - events (auto- or manual-reset) and threads behind one HANDLE type, waited on with WaitForSingleObject;
//   - QueryPerformanceCounter in nanoseconds of CLOCK_MONOTONIC (QueryPerformanceFrequency 1e9);
//   - thread priorities are left to the system; thread names go to pthread_setname_np.
#pragma once

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

typedef uint32_t DWORD;
typedef uint64_t UINT64;
typedef int BOOL;
typedef void* LPVOID;
typedef void* HANDLE;
typedef const wchar_t* PCWSTR;
typedef DWORD(*LPTHREAD_START_ROUTINE)(LPVOID);

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL 0
#endif
#define WINAPI
#define TEXT(s) s
#define INFINITE 0xFFFFFFFFu
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15

typedef union _LARGE_INTEGER {
    struct {
        uint32_t LowPart;
        int32_t  HighPart;
    };
    int64_t QuadPart;
} LARGE_INTEGER;

inline BOOL QueryPerformanceCounter(LARGE_INTEGER* count)
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    count->QuadPart = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    return TRUE;
}

inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency)
{
    frequency->QuadPart = 1000000000;
    return TRUE;
}

inline void Sleep(DWORD ms)
{
    timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

inline BOOL SwitchToThread() { return sched_yield() == 0; }

// Critical sections
typedef struct _CRITICAL_SECTION {
    pthread_mutex_t mutex;
} CRITICAL_SECTION;

inline void InitializeCriticalSection(CRITICAL_SECTION* cs)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}
inline void DeleteCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_destroy(&cs->mutex); }
inline void EnterCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_lock(&cs->mutex); }
inline void LeaveCriticalSection(CRITICAL_SECTION* cs) { pthread_mutex_unlock(&cs->mutex); }
inline BOOL TryEnterCriticalSection(CRITICAL_SECTION* cs) { return pthread_mutex_trylock(&cs->mutex) == 0; }

// Events and threads share HANDLE, as on Windows: the object says which it is.
struct Win32ShimObject {
    enum Kind { Event, Thread } kind;
    // event
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
    bool            manualReset;
    bool            signaled;
    // thread
    pthread_t              thread;
    LPTHREAD_START_ROUTINE start;
    LPVOID                 param;
    bool                   joined;
};

inline HANDLE CreateEvent(void*, BOOL manualReset, BOOL initialState, const char*)
{
    Win32ShimObject* e = new Win32ShimObject();
    e->kind = Win32ShimObject::Event;
    pthread_mutex_init(&e->mutex, nullptr);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&e->cond, &attr);
    pthread_condattr_destroy(&attr);
    e->manualReset = manualReset != FALSE;
    e->signaled    = initialState != FALSE;
    return e;
}
#define CreateEventA CreateEvent

inline BOOL SetEvent(HANDLE h)
{
    Win32ShimObject* e = (Win32ShimObject*)h;
    if (!e || e->kind != Win32ShimObject::Event)
        return FALSE;
    pthread_mutex_lock(&e->mutex);
    e->signaled = true;
    if (e->manualReset)
        pthread_cond_broadcast(&e->cond);
    else
        pthread_cond_signal(&e->cond);
    pthread_mutex_unlock(&e->mutex);
    return TRUE;
}

inline BOOL ResetEvent(HANDLE h)
{
    Win32ShimObject* e = (Win32ShimObject*)h;
    if (!e || e->kind != Win32ShimObject::Event)
        return FALSE;
    pthread_mutex_lock(&e->mutex);
    e->signaled = false;
    pthread_mutex_unlock(&e->mutex);
    return TRUE;
}

inline void* Win32ShimThreadStart(void* p)
{
    Win32ShimObject* t = (Win32ShimObject*)p;
    return (void*)(uintptr_t)t->start(t->param);
}

inline HANDLE CreateThread(void*, size_t, LPTHREAD_START_ROUTINE start, LPVOID param, DWORD, DWORD* threadId)
{
    Win32ShimObject* t = new Win32ShimObject();
    t->kind   = Win32ShimObject::Thread;
    t->start  = start;
    t->param  = param;
    t->joined = false;
    if (pthread_create(&t->thread, nullptr, Win32ShimThreadStart, t) != 0)
    {
        delete t;
        return NULL;
    }
    if (threadId)
        *threadId = 0;
    return t;
}

inline DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    Win32ShimObject* o = (Win32ShimObject*)h;
    if (!o)
        return WAIT_FAILED;
    if (o->kind == Win32ShimObject::Thread)
    {
        // the swapchain only waits for its threads to end, with INFINITE
        if (!o->joined)
        {
            pthread_join(o->thread, nullptr);
            o->joined = true;
        }
        return WAIT_OBJECT_0;
    }
    pthread_mutex_lock(&o->mutex);
    if (ms == INFINITE)
    {
        while (!o->signaled)
            pthread_cond_wait(&o->cond, &o->mutex);
    }
    else
    {
        timespec until;
        clock_gettime(CLOCK_MONOTONIC, &until);
        until.tv_sec += ms / 1000;
        until.tv_nsec += (long)(ms % 1000) * 1000000;
        if (until.tv_nsec >= 1000000000)
            until.tv_sec++, until.tv_nsec -= 1000000000;
        while (!o->signaled)
            if (pthread_cond_timedwait(&o->cond, &o->mutex, &until) == ETIMEDOUT)
                break;
    }
    DWORD result = o->signaled ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
    if (o->signaled && !o->manualReset)
        o->signaled = false;
    pthread_mutex_unlock(&o->mutex);
    return result;
}

inline BOOL CloseHandle(HANDLE h)
{
    Win32ShimObject* o = (Win32ShimObject*)h;
    if (!o)
        return FALSE;
    if (o->kind == Win32ShimObject::Thread)
    {
        if (!o->joined)
            pthread_detach(o->thread);
    }
    else
    {
        pthread_cond_destroy(&o->cond);
        pthread_mutex_destroy(&o->mutex);
    }
    delete o;
    return TRUE;
}

// Raising a thread above the others needs privileges on Linux (SCHED_FIFO or a negative nice); the system schedules them.
inline BOOL SetThreadPriority(HANDLE, int) { return TRUE; }

inline long SetThreadDescription(HANDLE h, PCWSTR name)
{
    Win32ShimObject* t = (Win32ShimObject*)h;
    if (!t || t->kind != Win32ShimObject::Thread || !name)
        return 0;
    char s[16];   // pthread names hold 15 characters
    size_t n = 0;
    for (; name[n] && n < sizeof s - 1; n++)
        s[n] = name[n] < 128 ? (char)name[n] : '?';
    s[n] = 0;
    pthread_setname_np(t->thread, s);
    return 0;
}
