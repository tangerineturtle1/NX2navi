"""Check how a translator child that stops answering is handled (runtime/platform/win32.c plat_child_read,
runtime/gfx_shader.c child_exchange_raw and svc_request). The check cuts the child functions out of win32.c and the
request functions out of gfx_shader.c, builds them with PLAT_CHILD_READ_MS at 300 ms (platform.h lets a build set it)
and runs three children through them:

- a child that keeps its pipe open and never writes: plat_child_read gives -2 after the timeout, and svc_request
  gives -1 having started one child, so a stuck translator holds its caller one timeout, not two;
- a child that exits at once: the read fails with a broken pipe (0 or -1) at once, and svc_request restarts it once
  and gives 0;
- a child that answers one line: svc_request gives 1 and the line without its line end.

Windows only (the child functions are win32.c's). No GPU, no game files, any Python.

Usage: child_timeout_check.py
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def compiler():
    """$CC, else MSYS2 UCRT64's clang, else clang on PATH."""
    if os.environ.get('CC'):
        return os.environ['CC']
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def extract(src, head, tail=''):
    """The text from head through its brace-balanced body (plus tail)."""
    i = src.index(head)
    j = src.index('{', i)
    depth = 0
    for k in range(j, len(src)):
        if src[k] == '{':
            depth += 1
        elif src[k] == '}':
            depth -= 1
            if depth == 0:
                return src[i:k + 1] + tail + '\n'
    raise SystemExit(f'unbalanced: {head}')


HARNESS = r'''
#include <windows.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#define PLAT_CHILD_READ_MS 300
#include "platform/platform.h"
#define WPATH 2048
#define LINE_MAX_LEN 4096
static int widen(const char *s, wchar_t *w, int cap) { return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cap) > 0; }
static volatile int64_t n_requests;
static int starts;
static const char *child_arg;
'''

STUBS = r'''
static Svc one;
static int child_start(Svc *v) {
    uint32_t err = 0;
    starts++;
    v->child = plat_child_start("C:\\Windows\\System32\\cmd.exe", child_arg, &err);
    v->in_len = 0;
    return v->child != NULL;
}
static Svc *svc_get(void) { return &one; }
static void svc_put(Svc *v) { (void)v; }
static int child_exchange(Svc *v, const char *line, char *reply, size_t reply_size) {
    return child_exchange_raw(v, line, reply, reply_size);
}
'''

MAIN = r'''
static const char *SILENT = "/c ping -n 30 127.0.0.1 >NUL", *GONE = "/c exit 0", *ANSWER = "/c echo ok 7";

static int request(const char *arg, char *reply, size_t n, ULONGLONG *ms) {
    child_arg = arg, starts = 0;
    ULONGLONG t0 = GetTickCount64();
    int r = svc_request("job", reply, n);
    *ms = GetTickCount64() - t0;
    if (one.child) child_stop(&one);
    return r;
}

int main(void) {
    char buf[64], reply[256];
    ULONGLONG ms;

    /* plat_child_read alone */
    PlatChild *c = plat_child_start("C:\\Windows\\System32\\cmd.exe", SILENT, NULL);
    assert(c);
    ULONGLONG t0 = GetTickCount64();
    long got = plat_child_read(c, buf, sizeof buf);
    ms = GetTickCount64() - t0;
    printf("silent child: plat_child_read %ld after %llu ms\n", got, (unsigned long long)ms);
    assert(got == -2 && ms >= PLAT_CHILD_READ_MS && ms < 1000);
    plat_child_stop(c);
    c = plat_child_start("C:\\Windows\\System32\\cmd.exe", GONE, NULL);
    assert(c);
    got = plat_child_read(c, buf, sizeof buf);
    printf("exited child: plat_child_read %ld\n", got);
    assert(got == 0 || got == -1);
    plat_child_stop(c);

    /* svc_request */
    int r = request(SILENT, reply, sizeof reply, &ms);
    printf("silent child: svc_request %d, %d children, %llu ms\n", r, starts, (unsigned long long)ms);
    assert(r == -1 && starts == 1 && ms < 2 * PLAT_CHILD_READ_MS);
    r = request(GONE, reply, sizeof reply, &ms);
    printf("exited child: svc_request %d, %d children, %llu ms\n", r, starts, (unsigned long long)ms);
    assert(r == 0 && starts == 2);
    r = request(ANSWER, reply, sizeof reply, &ms);
    printf("answering child: svc_request %d, %d children, reply '%s'\n", r, starts, reply);
    assert(r == 1 && starts == 1 && !strcmp(reply, "ok 7"));

    printf("child timeout check: a silent child costs one timeout and is not asked again; a dead one is restarted once\n");
    return 0;
}
'''


def main():
    if os.name != 'nt':
        print('child timeout check: Windows only (win32.c); skipped')
        return 0
    win = (ROOT / 'runtime' / 'platform' / 'win32.c').read_text(encoding='utf-8')
    gfx = (ROOT / 'runtime' / 'gfx_shader.c').read_text(encoding='utf-8')
    parts = [
        HARNESS,
        extract(win, 'struct PlatChild {', ';'),
        extract(win, 'PlatChild *plat_child_start('),
        extract(win, 'void plat_child_stop('),
        extract(win, 'int plat_child_write('),
        extract(win, 'long plat_child_read('),
        extract(gfx, 'typedef struct {                                           // one translator child', ' Svc;'),
        'static int child_start(Svc *v);\n',
        extract(gfx, 'static void child_stop('),
        extract(gfx, 'static int child_exchange_raw('),
        STUBS,
        extract(gfx, 'static int svc_request('),
        MAIN,
    ]
    work = ROOT / 'build' / 'test' / 'child_timeout'
    work.mkdir(parents=True, exist_ok=True)
    c = work / 'check.c'
    c.write_text(''.join(parts), encoding='utf-8')
    exe = work / 'check.exe'
    env = dict(os.environ)
    if MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', '-march=x86-64-v3',
                           '-I', str(ROOT / 'runtime'), str(c), '-o', str(exe)], env=env)
    return subprocess.run([str(exe)], env=env, timeout=60).returncode


if __name__ == '__main__':
    sys.exit(main())
