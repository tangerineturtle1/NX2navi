"""The log file's cap and the end of the run (runtime/main.c log_sink): past the cap ordinary lines are dropped, with
one line that says so; once the process is ending (rt_log_ending, which a fatal error and the crash report call before
rt_log_sync) its lines are still written, after one line that says the end follows, up to a reserve of their own,
however much was dropped before. rt_log_sync alone (a bug capture in play, runtime/bugcap.c write_log_tail) does not
open the reserve. Builds log_sink with a small cap and reserve against stand-ins for the file and stderr, and checks
statically that every end that syncs the log marks it as ending first. No game files.

Usage: python tests/log_cap_check.py [path to clang]"""
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
source = (ROOT / 'runtime' / 'main.c').read_text(encoding='utf-8')


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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int PlatMutex;
#define PLAT_MUTEX_INIT 0
typedef struct PlatAppend PlatAppend;
static void plat_mutex_lock(PlatMutex *m) { (void)m; }
static int plat_mutex_trylock(PlatMutex *m) { (void)m; return 1; }
static void plat_mutex_unlock(PlatMutex *m) { (void)m; }
static char file[1 << 16];
static size_t file_n;
static int plat_append_write(PlatAppend *f, const void *p, size_t n) {
    (void)f;
    if (file_n + n <= sizeof file) memcpy(file + file_n, p, n);
    file_n += n;
    return 1;
}
static int plat_stderr_write(const void *p, size_t n) { (void)p, (void)n; return 1; }
static void logq_sync(void) {}
#define LOG_CAP 4096ull
#define LOG_END_RESERVE 2048ull
enum { LOG_EARLY, LOG_FILE, LOG_OFF };
static int g_log_state = LOG_FILE;
static PlatAppend *g_log_file;
static uint64_t g_log_bytes;
'''

MAIN = r'''
static int count(const char *what) {
    int n = 0;
    for (const char *p = file; (p = strstr(p, what)) != NULL && p < file + file_n; p++) n++;
    return n;
}
static int line(const char *tag, int i) {   // returns the line's length
    char b[64];
    int n = snprintf(b, sizeof b, "%s %04d ........................\n", tag, i);
    log_sink(b, (size_t)n);
    return n;
}
int main(int argc, char **argv) {
    // ending: rt_log_ending (an orderly end); fatal: rt_log_ending, then rt_log_sync (a fatal error, the crash report);
    // capture: rt_log_sync in play (a bug capture), more play, then rt_log_ending
    const char *how = argc > 1 ? argv[1] : "ending";
    int fails = 0, play_n = 0, end_n = 0;
    for (int i = 0; i < 1000; i++) play_n = line("play", i);   // 35,000 bytes, many times the cap
    int play = count("play "), cut = count("reached 64 MiB");
    if (!strcmp(how, "capture")) {
        rt_log_sync();
        for (int i = 0; i < 1000; i++) line("more", i);
        int more = count("more "), early = count("the end of the run follows");
        printf("capture: after rt_log_sync in play %d of 1000 play lines in the file, the end's line %d time(s)\n", more,
               early);
        if (more || early) printf("FAIL: rt_log_sync alone opened the end's reserve\n"), fails++;
    }
    size_t before_end = file_n;
    if (strcmp(how, "fatal")) {
        rt_log_ending();
    } else {
        rt_log_ending();
        rt_log_sync();
    }
    for (int i = 0; i < 100; i++) end_n = line("last", i);   // 3,400 bytes, more than the reserve
    int end = count("last "), follows = count("the end of the run follows");
    printf("%s: %d play lines in the file, the cut line %d time(s); %d end lines, the line before them %d time(s); the "
           "file %zu bytes (cap %llu, reserve %llu)\n", how, play, cut, end, follows, file_n, LOG_CAP, LOG_END_RESERVE);
    if (play != (int)(LOG_CAP / play_n) || cut != 1) printf("FAIL: lines past the cap\n"), fails++;
    if (follows != 1 || end != (int)(LOG_END_RESERVE / end_n)) printf("FAIL: the end's lines\n"), fails++;
    if (file_n - before_end > LOG_END_RESERVE + 200) printf("FAIL: past the reserve\n"), fails++;
    return fails ? 1 : 0;
}
'''


def static_checks():
    """Every end that syncs the log marks the process as ending first, so its lines reach the file past the cap."""
    bad = 0
    rt = ROOT / 'runtime'
    ends = [('main.c', 'static void crash_handler(const PlatCrash *x)'),
            ('main.c', 'static void log_sync_ending(void)'),
            ('dispatch.c', 'static void fatal_end(int code, const char *fmt, va_list ap) {'),
            ('vk_device.c', 'static void lost_wait_exit(void *arg)')]
    for file, sig in ends:
        text = (rt / file).read_text(encoding='utf-8')
        if sig not in text:
            print(f'static: {file}: {sig} is missing: FAIL', flush=True)
            bad += 1
            continue
        start = text.index(sig)
        brace = text.index('{', start)
        depth, end = 1, brace + 1
        while depth:
            depth += (text[end] == '{') - (text[end] == '}')
            end += 1
        body = re.sub(r'//[^\n]*', '', text[start:end])
        e, s = body.find('rt_log_ending()'), body.find('rt_log_sync()')
        ok = 0 <= e < s
        print(f'static: {file} {sig.split("(")[0].split()[-1].lstrip("*")} calls rt_log_ending before rt_log_sync: '
              f'{"ok" if ok else "FAIL"}', flush=True)
        bad += not ok
    ok = 'plat_exit_hook = log_sync_ending;' in source
    print(f'static: main.c plat_exit_hook is log_sync_ending: {"ok" if ok else "FAIL"}', flush=True)
    return bad + (not ok)


def main():
    parts = [HARNESS]
    parts.append(re.search(r'static PlatMutex g_err_lock = PLAT_MUTEX_INIT;\n(.*?\n)\nvoid rt_log_ending', source, re.S)
                 .group(0).rsplit('\n', 1)[0])
    parts.append(function('void rt_log_ending(void)'))
    parts.append(function('static void log_sink(const char *p, size_t n)'))
    parts.append(function('void rt_log_sync(void)'))
    parts.append(MAIN)
    work = ROOT / 'build' / 'test' / 'log_cap'
    work.mkdir(parents=True, exist_ok=True)
    src = work / 'log_cap_check.c'
    src.write_text('\n'.join(parts), encoding='utf-8')
    exe = work / ('log_cap_check.exe' if os.name == 'nt' else 'log_cap_check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', str(src), '-o', str(exe)], env=env)
    bad = static_checks()
    for how in ('ending', 'fatal', 'capture'):
        bad += subprocess.run([str(exe), how], env=env, timeout=60).returncode != 0
    print(f'log_cap_check: {"FAILED" if bad else "ok"}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
