"""A save file's handle and a CloseFile on another thread (runtime/fs_host.c file_of, file_release, close_file): a
call under way keeps its host file open, the close happens once when the last call is out, the guest's handle value
is not handed out again before that, and a call after the close gets FS_R_INVALID_ARGUMENT. A call with no close
pending takes no exclusive lock, so it ends while another thread holds the lock shared (a Commit's fsync loop). The
host files here are
stand-ins that hand a closed file's value to the next one opened, as Windows does with a HANDLE: a read that went on
through a closed handle would read the other file. Builds fs_host.c's file functions into a test program. No game
files.

Usage: python tests/fs_file_use_check.py [path to clang]"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
source = (ROOT / 'runtime' / 'fs_host.c').read_text(encoding='utf-8')


def compiler():
    if len(sys.argv) > 1:
        return sys.argv[1]
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


HARNESS = r'''
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef SRWLOCK PlatMutex;
#define PLAT_MUTEX_INIT SRWLOCK_INIT
static volatile LONG exclusive_takes;   // exclusive acquisitions of fs_host.c's lock
static void plat_mutex_lock(PlatMutex *m) { InterlockedIncrement(&exclusive_takes); AcquireSRWLockExclusive(m); }
static void plat_mutex_unlock(PlatMutex *m) { ReleaseSRWLockExclusive(m); }
static void plat_mutex_lock_shared(PlatMutex *m) { AcquireSRWLockShared(m); }
static void plat_mutex_unlock_shared(PlatMutex *m) { ReleaseSRWLockShared(m); }
static void plat_inc32(volatile int32_t *v) { InterlockedIncrement((volatile LONG *)v); }
static void plat_dec32(volatile int32_t *v) { InterlockedDecrement((volatile LONG *)v); }
static void rt_log(const char *fmt, ...) { (void)fmt; }
__attribute__((noreturn)) static void rt_fatal(const char *fmt, ...) { printf("FATAL: %s\n", fmt); fflush(stdout); ExitProcess(99); }
enum { FS_OPEN_READ = 1, FS_OPEN_WRITE = 2, FS_OPEN_APPEND = 4 };
enum { FS_R_INVALID_ARGUMENT = 6001, FS_R_OPEN_MODE = 6002, FS_R_PATH_NOT_FOUND = 6003, FS_R_UNSUPPORTED_OPEN = 6004 };
enum { M_FREE, M_SAVE, M_TEMP, M_CACHE, M_SAVE_RO };
enum { PLAT_FS_FILE = 1 };
#define FILE_BASE 0x1000
#define NFILES 8
#define PATH_CAP 64
static int romfs_owns(const char *p) { (void)p; return 0; }
static uint32_t romfs_open(uint64_t *o, const char *p, int m) { (void)o, (void)p, (void)m; return 1; }
static int romfs_handle(uint64_t h) { (void)h; return 0; }
static uint32_t romfs_read(uint64_t *o, uint64_t h, int64_t off, void *b, uint64_t n) { (void)o, (void)h, (void)off, (void)b, (void)n; return 1; }
static void romfs_close(uint64_t h) { (void)h; }
static void fs_trace(const char *w, const char *p) { (void)w, (void)p; }
static uint32_t fs_result(int e) { return e ? 6099u : 0; }
static void mem_thaw(uintptr_t a, uint64_t n) { (void)a, (void)n; }
static uint32_t host_path_kind(const char *path, char *p, size_t n, int *kind) {
    snprintf(p, n, "%s", path);
    *kind = M_SAVE;
    return 0;
}
static int plat_fs_kind(const char *p) { (void)p; return PLAT_FS_FILE; }
static volatile int32_t save_writers;
static void save_touched(void) {}

// The host's files: four kernel objects; a close frees one, and the next open takes the lowest free one, so a closed
// handle's value comes back for another file at once.
typedef struct PlatFile { volatile LONG open, closes; char content; } PlatFile;
static PlatFile kfiles[4];
static SRWLOCK klock = SRWLOCK_INIT;
static HANDLE hold_read, in_read;   // hold_read set: a read waits for it (and says so through in_read)
static PlatFile *plat_fs_open(const char *p, int write, int *err) {
    (void)write;
    AcquireSRWLockExclusive(&klock);
    for (int i = 0; i < 4; i++)
        if (!kfiles[i].open) {
            kfiles[i].open = 1, kfiles[i].content = p[0];
            ReleaseSRWLockExclusive(&klock);
            return &kfiles[i];
        }
    ReleaseSRWLockExclusive(&klock);
    *err = 1;
    return NULL;
}
static void plat_file_close(PlatFile *f) {
    AcquireSRWLockExclusive(&klock);
    if (!f->open) printf("FAIL: a host file closed twice\n");
    f->open = 0, f->closes++;
    ReleaseSRWLockExclusive(&klock);
}
static int64_t plat_file_pread(PlatFile *f, void *buf, uint32_t n, uint64_t off) {
    (void)off;
    if (hold_read) { SetEvent(in_read); WaitForSingleObject(hold_read, INFINITE); }
    for (volatile int spin = 0; spin < 200; spin++) {}   // the window in which another thread may close and open
    if (!f->open) return -1;
    memset(buf, f->content, n);
    return n;
}
'''

MAIN = r'''
static uint64_t h_read;
static char got;
static uint32_t got_r;
static DWORD WINAPI reader(void *arg) {
    (void)arg;
    uint64_t n = 0;
    char b[4] = {0};
    got_r = read_file(&n, h_read, 0, b, 4);
    got = b[0];
    return 0;
}

static volatile LONG stop, foreign, invalid, reads;
static volatile uint64_t current;   // the handle the looping reader reads: a file named "a"
static DWORD WINAPI loop_reader(void *arg) {
    (void)arg;
    while (!stop) {
        uint64_t n = 0;
        char b[4] = {0};
        uint32_t r = read_file(&n, current, 0, b, 4);
        if (!r && b[0] != 'a') InterlockedIncrement(&foreign);
        if (r == FS_R_INVALID_ARGUMENT) InterlockedIncrement(&invalid);
        InterlockedIncrement(&reads);
    }
    return 0;
}

int main(void) {
    int fails = 0;
    uint64_t ha, hb, hc;
    // 1. A read under way when CloseFile comes: it finishes in its own file; the close happens when it is out, once.
    if (open_file(&ha, "a", FS_OPEN_READ | FS_OPEN_WRITE)) return printf("FAIL: open\n"), 1;
    PlatFile *ka = files[ha - FILE_BASE].h;
    hold_read = CreateEventW(NULL, TRUE, FALSE, NULL), in_read = CreateEventW(NULL, FALSE, FALSE, NULL);
    h_read = ha;
    HANDLE t = CreateThread(NULL, 0, reader, NULL, 0, NULL);
    WaitForSingleObject(in_read, INFINITE);
    close_file(ha);
    int closed_early = ka->closes;
    uint64_t n = 0;
    char b[4];
    HANDLE hr = hold_read;
    hold_read = NULL;   // the calls below do not wait
    uint32_t after = read_file(&n, ha, 0, b, 4);
    open_file(&hb, "b", FS_OPEN_READ);
    int reused = hb == ha || files[hb - FILE_BASE].h == ka;
    SetEvent(hr);
    WaitForSingleObject(t, INFINITE);
    printf("close during a read: closed %s, a read after the close %s, the next open %s, the read got '%c' (%s), the "
           "file closed %ld time(s)\n", closed_early ? "AT ONCE (FAIL)" : "after the read",
           after == FS_R_INVALID_ARGUMENT ? "refused" : "ALLOWED (FAIL)", reused ? "TOOK ITS HANDLE (FAIL)" : "got another",
           got, got_r || got != 'a' ? "FAIL" : "ok", (long)ka->closes);
    if (closed_early || after != FS_R_INVALID_ARGUMENT || reused || got != 'a' || got_r || ka->closes != 1) fails++;
    open_file(&hc, "c", FS_OPEN_READ);
    if (hc != ha) printf("FAIL: the closed slot was not free again (%#llx)\n", (unsigned long long)hc), fails++;
    close_file(hb), close_file(hc);

    // 2. Calls with no close pending take no exclusive lock: 1000 reads add none, and a read ends while another thread
    //    holds the lock shared (fs_host.c commit's fsync loop runs so).
    open_file(&ha, "a", FS_OPEN_READ);
    LONG before = exclusive_takes;
    int bad_reads = 0;
    for (int i = 0; i < 1000; i++) bad_reads += read_file(&n, ha, 0, b, 4) != 0 || b[0] != 'a';
    LONG taken = exclusive_takes - before;
    AcquireSRWLockShared(&lock);
    h_read = ha, got = 0;
    t = CreateThread(NULL, 0, reader, NULL, 0, NULL);
    int ended = WaitForSingleObject(t, 2000) == WAIT_OBJECT_0;
    ReleaseSRWLockShared(&lock);
    WaitForSingleObject(t, INFINITE);
    printf("reads with no close pending: 1000 reads took the exclusive lock %ld time(s)%s, a read beside a shared holder "
           "%s\n", (long)taken, taken ? " (FAIL)" : "", ended ? "ended" : "WAITED FOR IT (FAIL)");
    if (taken || bad_reads || !ended || got != 'a') fails++;
    close_file(ha);

    // 3. A reader loops over a file "a" while another thread closes it, opens "b" (which may take a closed handle's
    //    value) and opens "a" again: no read returns b's bytes.
    open_file(&ha, "a", FS_OPEN_READ);
    current = ha;
    HANDLE r = CreateThread(NULL, 0, loop_reader, NULL, 0, NULL);
    for (int i = 0; i < 20000; i++) {
        close_file(current);
        open_file(&hb, "b", FS_OPEN_READ);
        open_file(&ha, "a", FS_OPEN_READ);
        current = ha;
        close_file(hb);
    }
    stop = 1;
    WaitForSingleObject(r, INFINITE);
    close_file(current);
    printf("a reader beside 20000 closes and opens: %ld reads, %ld refused after a close, %ld read another file's "
           "bytes\n", (long)reads, (long)invalid, (long)foreign);
    if (foreign) fails++;
    int open_left = 0;
    for (int i = 0; i < 4; i++) open_left += kfiles[i].open;
    for (int i = 0; i < NFILES; i++) open_left += files[i].h != NULL;
    if (open_left) printf("FAIL: %d files left open\n", open_left), fails++;
    printf("fs_file_use_check: %s\n", fails ? "FAILED" : "ok");
    return fails ? 1 : 0;
}
'''


def main():
    if os.name != 'nt':
        print('fs_file_use_check: Windows only')
        return 0
    file_t = re.search(r'typedef struct \{ PlatFile \*h;[^\n]*\} File;', source).group(0)
    parts = [HARNESS, file_t, 'static File files[NFILES];', 'static PlatMutex lock = PLAT_MUTEX_INIT;']
    for sig in ('static File *file_of(', 'static void file_closed(', 'static void file_clear(', 'static void file_release(',
                'static int64_t host_read_at(', 'static uint32_t open_file(', 'static uint32_t read_file(',
                'static void close_file('):
        parts.append(function(sig))
    parts.append(MAIN)
    work = ROOT / 'build' / 'test' / 'fs_file_use'
    work.mkdir(parents=True, exist_ok=True)
    (work / 'fs_file_use_check.c').write_text('\n'.join(parts), encoding='utf-8')
    exe = work / 'fs_file_use_check.exe'
    env = dict(os.environ)
    if MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', str(work / 'fs_file_use_check.c'),
                           '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env, timeout=300).returncode


if __name__ == '__main__':
    sys.exit(main())
