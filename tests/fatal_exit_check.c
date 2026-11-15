// runtime/dispatch.c's end of a fatal error (fatal_end), with the platform layer linked and the rest of the runtime as
// stand-ins that write straight to stdout. Built and run by tests/fatal_exit_check.py, which reads what each mode
// printed and its exit code. The exe has a TLS callback that prints a line when the process's libraries get their
// detach call, which ExitProcess (and _Exit, which is ExitProcess in the C runtime) makes and TerminateProcess does not.
//
//   fatal_exit_check fatal         rt_fatal: exit code 70, the FATAL line, no detach line
//   fatal_exit_check crash_crash   two threads fault at once: one whole crash report, exit code 71
//   fatal_exit_check fatal_crash   a fault on another thread while rt_fatal reports: its FATAL line alone, exit code 70
//   fatal_exit_check crash_fatal   rt_fatal on another thread while a crash is reported: the whole report, exit code 71
//   fatal_exit_check fatal_rumble  rt_fatal while the HD rumble stop hangs (SDL's lock held): exit code 70 all the same
//   fatal_exit_check fatal_rtss    rt_fatal on RTSS's thread ("rtss"): exit code 70, through rtss_limit_crash too
//
// The crash handler here is a stand-in for main.c crash_handler with its first line (the static check in
// tests/fatal_exit_check.py holds main.c to it): it writes three report lines 100 ms apart and ends with plat_exit(71).
#include <stdio.h>
#include <windows.h>

#include "host.h"
#include "platform/platform.h"

static void say(const char *s) {
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)strlen(s), &w, NULL);
}

static void NTAPI tls_cb(PVOID h, DWORD reason, PVOID reserved) {
    (void)h, (void)reserved;
    if (reason == DLL_PROCESS_DETACH) say("detach: the libraries' detach calls ran\n");
}
__attribute__((section(".CRT$XLB"), used)) PIMAGE_TLS_CALLBACK p_tls_cb = tls_cb;
__thread int tls_var;   // with _tls_used below: the image gets its TLS directory, so the callback is called
extern const IMAGE_TLS_DIRECTORY _tls_used;
const void *volatile keep_tls = &_tls_used;

// ---- the runtime fatal_end reaches
void rt_log_ending(void) {}
void rt_log_sync(void) {}
void rt_log_raw(const char *p, size_t n) {
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), p, (DWORD)n, &w, NULL);
}
void rt_vlogf(const char *fmt, va_list ap) {
    char b[1024];
    int n = vsnprintf(b, sizeof b, fmt, ap);
    if (n > 0) rt_log_raw(b, (size_t)n < sizeof b ? (size_t)n : sizeof b - 1);
}
void rt_logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_vlogf(fmt, ap);
    va_end(ap);
}
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    rt_vlogf(fmt, ap);
    va_end(ap);
    rt_log_raw("\n", 1);
}
void kern_free_cores_note(void) {}
void prof_sampler_write(void) {}
void rtss_limit_crash(void) { say("rtss: left to the next start\n"); }   // rtss_limit.c: no call into RTSS
void kern_checkpoint(void) {}

// perf.c's summary at a fatal error, which dispatch.c calls through a weak symbol
void perf_summary_flush_try(void) { say("frametime: at a fatal error\n"); }

// input.c's stop, which dispatch.c calls through a weak symbol: says it ran, or hangs as when SDL's lock is held
static int rumble_hangs;
void input_rumble_hd_stop(void) {
    if (rumble_hangs) Sleep(INFINITE);
    say("rumble: stopped\n");
}

// ---- the crash report and the threads that fail beside it
static void crash(const PlatCrash *x) {
    if (!rt_ending_enter(71)) plat_exit(71);   // main.c crash_handler's first line
    char b[160];
    for (int i = 1; i <= 3; i++) {
        snprintf(b, sizeof b, "report line %d: %s on thread %lu\n", i, x->what, (unsigned long)GetCurrentThreadId());
        say(b);
        Sleep(100);
    }
    plat_exit(71);
}

static volatile LONG go;
static DWORD WINAPI fault(void *arg) {
    Sleep((DWORD)(uintptr_t)arg);
    while (!go) Sleep(0);
    *(volatile int *)(uintptr_t)16 = 1;
    return 0;
}
static DWORD WINAPI fatal_thread(void *arg) {
    Sleep((DWORD)(uintptr_t)arg);
    rt_fatal("the test's fatal error %d", 2);
}
static void rtss_thread(void *arg) {   // rtss_limit.c's worker, named as plat_thread_spawn names it there
    (void)arg;
    rt_fatal("the test's fatal error %d", 3);
}
static void slow_note(void) {   // a fatal note that takes a while, as the card's and the memory settings' may
    say("fatal note: start\n");
    Sleep(300);
    say("fatal note: end\n");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *mode = argc > 1 ? argv[1] : "";
    tls_var = 1;
    plat_crash_hook(crash);
    if (!strcmp(mode, "fatal")) rt_fatal("the test's fatal error %d", 1);
    if (!strcmp(mode, "fatal_rumble")) {
        rumble_hangs = 1;
        rt_fatal("the test's fatal error %d", 1);
    }
    if (!strcmp(mode, "crash_crash")) {
        CloseHandle(CreateThread(NULL, 0, fault, (void *)0, 0, NULL));
        CloseHandle(CreateThread(NULL, 0, fault, (void *)0, 0, NULL));
        Sleep(50);
        go = 1;
        Sleep(10000);
        return 3;
    }
    if (!strcmp(mode, "fatal_crash")) {
        rt_fatal_note_add(slow_note);
        go = 1;
        CloseHandle(CreateThread(NULL, 0, fault, (void *)100, 0, NULL));
        rt_fatal("the test's fatal error %d", 1);
    }
    if (!strcmp(mode, "crash_fatal")) {
        CloseHandle(CreateThread(NULL, 0, fatal_thread, (void *)100, 0, NULL));
        go = 1;
        fault(0);
        return 3;
    }
    if (!strcmp(mode, "fatal_rtss")) {
        plat_thread_spawn(rtss_thread, NULL, "rtss", 0);
        Sleep(10000);
        return 3;
    }
    printf("usage: fatal_exit_check fatal|crash_crash|fatal_crash|crash_fatal|fatal_rumble|fatal_rtss\n");
    return 2;
}
