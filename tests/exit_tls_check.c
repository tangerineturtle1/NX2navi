// The note of an end nobody of ours asked for (runtime/platform/win32.c exit_note_tls, plat_exit_note_setup) with the
// platform layer and runtime/logq.c linked: the process logs a line through the log queue, which the log thread is
// still gathering, and ends. Built and run by tests/exit_tls_check.py, which reads the log file each mode leaves.
//   exit_tls_check <mode> <log>, mode one of:
//   exitprocess   ExitProcess from a function of this program: the queued line, then the note naming this program
//   exit          exit(): the C runtime's ExitProcess, the same note
//   terminate     TerminateProcess: no note
//   platexit      plat_exit: the queued line (its hook writes the queue), no note
//   return        main returns after plat_exit_orderly, as main.c does after a tool mode: no note
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "logq.h"
#include "platform/platform.h"

void rt_log(const char *fmt, ...) { (void)fmt; }   // logq.c's line about slow writes

static PlatAppend *file;
static void sink(const char *p, size_t n) { plat_append_write(file, p, n); }

__attribute__((noinline)) static void end_here(void) {
    ExitProcess(0);
}

int main(int argc, char **argv) {
    if (argc < 3 || !(file = plat_append_open(argv[2]))) return 2;
    logq_init(sink);
    if (!logq_async()) return 3;
    plat_exit_hook = logq_sync;
    plat_exit_note_setup(file, logq_peek);
    plat_exit_note_modules();
    logq_write("queued line\n", 12);   // the log thread gathers it for LOGQ_GATHER_NS: still queued below
    const char *m = argv[1];
    if (!strcmp(m, "exitprocess")) end_here();
    if (!strcmp(m, "exit")) exit(0);
    if (!strcmp(m, "terminate")) TerminateProcess(GetCurrentProcess(), 0);
    if (!strcmp(m, "platexit")) plat_exit(0);
    if (!strcmp(m, "return")) {
        plat_exit_orderly();
        return 0;
    }
    return 2;
}
