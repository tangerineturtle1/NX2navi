// A stack overflow's crash report (runtime/platform/win32.c plat_stack_guarantee) with the platform layer linked: a
// thread made by plat_thread_spawn, or a fiber made by plat_fiber_new, recurses until its stack runs out, and the crash
// handler then writes its lines as main.c crash_handler does (2 KiB log buffers, a file write first, as during the
// boot's builds, and a frame walk). Built and run by tests/crash_stack_check.py, which wants every line and code 71.
//   crash_stack_check thread <file>   the overflow on a thread
//   crash_stack_check fiber <file>    the overflow on a fiber
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "platform/platform.h"

void rt_log(const char *fmt, ...) { (void)fmt; }

static void out(const char *s) {
    DWORD w;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, (DWORD)strlen(s), &w, NULL);
}

// main.c rt_log: a 2 KiB buffer on the stack and vsnprintf
__attribute__((noinline)) static void log2k(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    out(buf);
}

static const char *file;

static void crash(const PlatCrash *x) {
    // main.c crash_handler's first call while the boot builds pipelines: vk_pipeline_crash_note writes a file
    static const char text[] = "the process faulted while the boot built pipelines\n";
    int wrote = plat_file_write(file, text, sizeof text - 1);
    log2k("report line 1: %s (%#x) on thread %s, the file written: %d\n", x->what, x->code,
          plat_thread_name() ? plat_thread_name() : "(unnamed)", wrote);
    log2k("report line 2: %d host frames\n", x->nframes);
    void *f[62];
    USHORT n = RtlCaptureStackBackTrace(0, 62, f, NULL);
    log2k("report line 3: %u frames captured; the end\n", (unsigned)n);
    TerminateProcess(GetCurrentProcess(), 71);
}

static volatile int never;   // keeps the recursion from looking endless to the compiler
__attribute__((noinline)) static int recurse(int d) {
    if (never) return 0;
    volatile char pad[1024];
    pad[0] = (char)d;
    return recurse(d + 1) + pad[0];
}

static void overflower(void *arg) {
    (void)arg;
    recurse(0);
}

static PlatFiber *main_fiber;
static void fiber_body(void *arg) {
    (void)arg;
    recurse(0);
    plat_fiber_switch(main_fiber);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    file = argv[2];
    plat_crash_hook(crash);
    if (!strcmp(argv[1], "thread")) {
        plat_thread_spawn(overflower, NULL, "deep", 0);
    } else {
        main_fiber = plat_fiber_self();
        PlatFiber *f = plat_fiber_new(1 << 20, fiber_body, NULL);
        if (!main_fiber || !f) return 3;
        plat_fiber_switch(f);
    }
    Sleep(10000);
    out("main: no crash ended the process in 10 s\n");
    return 0;
}
