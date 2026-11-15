"""Stress the production recent-first prebuild queue: changing concurrency must not omit or repeat any job."""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
source = (root / 'runtime/vk_pipeline.c').read_text()
start = source.index('static size_t lb_next(int worker)')
brace = source.index('{', start)
depth, end = 1, brace + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
harness = r'''
#include <windows.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
static struct {
    volatile int64_t next;
    volatile int32_t stop;
    size_t n, n_keep;
    int idle, tail_one, bg_threads;
} LB;
static int bf_wait_now, bg_threads = 8;   // bf_waiting(): the start waits for the whole warm
static int lb_idle(void) { return LB.idle && !bf_wait_now; }
static volatile LONG seen[2000];
static HANDLE start_event;
'''
harness += source[start:end]
harness += r'''
static DWORD WINAPI worker(void *arg) {
    int id=(int)(intptr_t)arg;
    assert(WaitForSingleObject(start_event,2000)==WAIT_OBJECT_0);
    for (;;) {
        size_t i=lb_next(id);
        if (i>=LB.n) break;
        assert(InterlockedIncrement(&seen[i])==1);
        if (LB.idle && !bf_wait_now && LB.tail_one && i>=LB.n_keep) assert(id==0);
        if (LB.idle && !bf_wait_now) assert(id<LB.bg_threads);
        if (i%7==0) SwitchToThread();
    }
    return 0;
}
static void run(size_t prefix, int automatic, int idle) {
    LB.next=0;LB.n=2000;LB.n_keep=prefix;LB.stop=0;LB.tail_one=automatic;LB.idle=idle;LB.bg_threads=bg_threads;
    for (int i=0;i<2000;i++) seen[i]=0;
    ResetEvent(start_event);
    HANDLE threads[8];
    for (int i=0;i<8;i++) { threads[i]=CreateThread(NULL,0,worker,(void *)(intptr_t)i,0,NULL);assert(threads[i]); }
    SetEvent(start_event);
    assert(WaitForMultipleObjects(8,threads,TRUE,5000)==WAIT_OBJECT_0);
    for (int i=0;i<8;i++) CloseHandle(threads[i]);
    assert(LB.next==2000);
    for (int i=0;i<2000;i++) assert(seen[i]==1);
}
int main(void) {
    start_event=CreateEventW(NULL,TRUE,FALSE,NULL);assert(start_event);
    for (int repeat=0;repeat<20;repeat++) {
        run(289,1,1);run(0,1,1);run(2000,1,1);run(1,1,1);
        run(289,0,1);run(289,1,0);
    }
    // The boot's builders (idle) beside the game: only the first bg_threads workers go on; while the start waits
    // for the whole warm (bf_waiting) every worker does.
    bg_threads=2;run(289,1,1);run(0,0,1);bf_wait_now=1;run(289,1,1);bf_wait_now=0;bg_threads=8;
    LB.next=289;LB.n_keep=289;LB.tail_one=1;LB.idle=1;LB.bg_threads=8;
    assert(lb_next(1)==LB.n && LB.next==289);
    assert(lb_next(0)==289);
    LB.stop=1;
    assert(lb_next(0)==LB.n && LB.next==290);
    LB.stop=0;LB.tail_one=0;
    assert(lb_next(1)==290);
    LB.bg_threads=2;LB.idle=1;
    assert(lb_next(2)==LB.n && LB.next==291);   // worker 2 of 2 stops beside the game
    assert(lb_next(1)==291);
    LB.idle=0;
    assert(lb_next(2)==292);                    // and goes on in a preparation (idle 0)
    CloseHandle(start_event);
    puts("Prebuild queue: concurrent exactly-once coverage, recent/tail boundary, explicit override and stop checks passed");
    return 0;
}
'''
out = root / 'build/test/compute-prebuild-queue'
out.mkdir(parents=True, exist_ok=True)
c = out / 'check.c'
c.write_text(harness)
cc = sys.argv[1] if len(sys.argv) > 1 else 'C:/msys64/ucrt64/bin/clang.exe'
exe = out / 'check.exe'
subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True, timeout=15)
