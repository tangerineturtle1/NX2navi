"""Check how pipeline-binaries.bin keeps its whole records (runtime/vk_pipeline.c pb_index and pb_keep,
NX2NAVI_PIPE_BINARIES=1). pb_init keeps the whole records of a file that ends inside one (the process ended during an
append, or a disk filled) or holds a malformed one, and cuts the file back to them; the walk must say where the last
whole record ends, and pb_keep must cut the file there. The check cuts the file's record layout and pb_hash, pb_find,
pb_insert, pb_index and pb_keep out of vk_pipeline.c, writes files of whole records, one with a torn last record and
one with a malformed record in the middle, and checks the offset and the index the walk gives, the length pb_keep
leaves on the disk, that a second walk reaches the end of the cut file, and that a record appended after the cut is
read back. plat_file_truncate is a stand-in that cuts the file with the C library.

No GPU, no game files, any Python. Needs the Vulkan headers of MSYS2 UCRT64 (vulkan/vulkan.h).

Usage: pb_torn_check.py
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


HARNESS = r'''
#include <vulkan/vulkan.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform/platform.h"
#ifdef _WIN32
#define pb_seek _fseeki64
#define pb_tell _ftelli64
#else
#define pb_seek fseeko
#define pb_tell ftello
#endif
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
#define FNV0 0xcbf29ce484222325ull
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif
static int logs, truncate_fails;
static char last_log[512];
void rt_log(const char *fmt, ...) {
    logs++;
    snprintf(last_log, sizeof last_log, "%s", fmt);
}
int plat_file_truncate(const char *path, uint64_t size) {   /* platform.h's, with the C library */
    if (truncate_fails) return 0;
#ifdef _WIN32
    int fd = _open(path, _O_RDWR | _O_BINARY);
    if (fd < 0) return 0;
    int ok = _chsize_s(fd, (__int64)size) == 0;
    _close(fd);
    return ok;
#else
    return truncate(path, (off_t)size) == 0;
#endif
}
'''

MAIN = r'''
static void put(FILE *f, uint32_t type, uint8_t key0, uint32_t payload) {
    uint32_t size = PB_HEAD + payload, ks = 16;
    uint8_t key[PB_KEY] = {key0, 1, 2, 3};
    fwrite(&size, 4, 1, f), fwrite(&type, 4, 1, f), fwrite(&ks, 4, 1, f), fwrite(key, PB_KEY, 1, f);
    for (uint32_t i = 0; i < payload; i++) fputc((int)(i * 7 + key0), f);
}

static void reset(void) {
    free(PB.slot);
    PB.slot = NULL, PB.cap = PB.n = PB.pipes = PB.blobs = 0;
}

static uint64_t walk(const char *path, uint64_t *len) {
    FILE *f = fopen(path, "rb");
    assert(f);
    pb_seek(f, 0, SEEK_END);
    *len = (uint64_t)pb_tell(f);
    reset();
    uint64_t good = pb_index(f, *len);
    fclose(f);
    return good;
}

int main(int argc, char **argv) {
    const char *path = argv[1];
    PbHeader hd = {{'N', 'X', '2', 'N', 'P', 'B', 'I', 'N'}, 2, 16, {0}};
    uint64_t len, whole;

    /* Three whole records: the walk reaches the end. */
    FILE *f = fopen(path, "wb");
    fwrite(&hd, sizeof hd, 1, f);
    put(f, PB_BLOB, 1, 1000), put(f, PB_BLOB, 2, 3), put(f, PB_PIPE, 3, 4 + 16 + PB_KEY);
    fclose(f);
    assert(walk(path, &len) == len && PB.n == 3 && PB.blobs == 2 && PB.pipes == 1);
    whole = len;
    assert(pb_find(pb_hash(PB_BLOB, (const uint8_t[PB_KEY]){1, 1, 2, 3}, 16))->off == sizeof hd);

    /* A fourth record cut short, at every length from its size field to one byte short of whole: the three stay and
       the walk ends where the fourth begins. */
    uint32_t rec4 = 4 + PB_HEAD + 500;
    for (uint32_t cut = 1; cut < rec4; cut += cut < 16 ? 1 : 37) {
        f = fopen(path, "wb");
        fwrite(&hd, sizeof hd, 1, f);
        put(f, PB_BLOB, 1, 1000), put(f, PB_BLOB, 2, 3), put(f, PB_PIPE, 3, 4 + 16 + PB_KEY);
        fclose(f);
        f = fopen(path, "ab");
        uint8_t *rec = calloc(1, rec4);
        uint32_t size = PB_HEAD + 500, type = PB_BLOB, ks = 16;
        memcpy(rec, &size, 4), memcpy(rec + 4, &type, 4), memcpy(rec + 8, &ks, 4), rec[12] = 4;
        fwrite(rec, cut, 1, f);
        free(rec);
        fclose(f);
        uint64_t good = walk(path, &len);
        assert(len == whole + cut && good == whole && PB.n == 3);
    }

    /* A malformed record in the middle: the records before it stay, the walk stops at it. */
    f = fopen(path, "wb");
    fwrite(&hd, sizeof hd, 1, f);
    put(f, PB_BLOB, 1, 1000);
    long at = ftell(f);
    put(f, 7, 2, 3);   /* no such type */
    put(f, PB_PIPE, 3, 4 + 16 + PB_KEY);
    fclose(f);
    assert(walk(path, &len) == (uint64_t)at && len > (uint64_t)at && PB.n == 1);

    /* pb_keep on a torn file: cut back to its whole records and logged; a second walk reaches the end of what is left,
       and a record appended there is read back with the three. */
    f = fopen(path, "wb");
    fwrite(&hd, sizeof hd, 1, f);
    put(f, PB_BLOB, 1, 1000), put(f, PB_BLOB, 2, 3), put(f, PB_PIPE, 3, 4 + 16 + PB_KEY);
    put(f, PB_BLOB, 4, 500);
    fclose(f);
    uint64_t torn = whole + 4 + PB_HEAD + 500 - 77;
    assert(plat_file_truncate(path, torn));
    uint64_t good = walk(path, &len);
    assert(len == torn && good == whole);
    logs = 0;
    assert(pb_keep(path, good, len) == whole && logs == 1 && strstr(last_log, "ends inside a record"));
    assert(walk(path, &len) == len && len == whole && PB.n == 3);
    f = fopen(path, "ab");
    put(f, PB_BLOB, 5, 64);
    fclose(f);
    assert(walk(path, &len) == len && len == whole + 4 + PB_HEAD + 64 && PB.n == 4);

    /* pb_keep on a malformed record in the middle: cut back to the record before it; the walk then reaches the end. */
    f = fopen(path, "wb");
    fwrite(&hd, sizeof hd, 1, f);
    put(f, PB_BLOB, 1, 1000);
    at = ftell(f);
    put(f, 7, 2, 3);
    put(f, PB_PIPE, 3, 4 + 16 + PB_KEY);
    fclose(f);
    good = walk(path, &len);
    logs = 0;
    assert(good == (uint64_t)at && pb_keep(path, good, len) == (uint64_t)at && logs == 1);
    assert(walk(path, &len) == len && len == (uint64_t)at && PB.n == 1);

    /* pb_keep on whole records: the file as it is, nothing logged. When the file cannot be cut: 0 (start empty), the
       file untouched. */
    f = fopen(path, "ab");
    put(f, PB_BLOB, 2, 3);
    fclose(f);
    good = walk(path, &len);
    logs = 0;
    assert(good == len && pb_keep(path, good, len) == len && logs == 0);
    f = fopen(path, "ab");
    fputc(1, f);
    fclose(f);
    good = walk(path, &len);
    truncate_fails = 1;
    assert(good == len - 1 && pb_keep(path, good, len) == 0 && logs == 0);
    truncate_fails = 0;
    uint64_t len2;
    assert(walk(path, &len2) == good && len2 == len);

    /* The header alone: nothing indexed, the walk ends at the header. */
    f = fopen(path, "wb");
    fwrite(&hd, sizeof hd, 1, f);
    fclose(f);
    assert(walk(path, &len) == sizeof hd && len == sizeof hd && PB.n == 0);

    reset();
    printf("pipeline binaries index: whole records kept, a torn last record and a malformed one end the walk there, "
           "and the file is cut back to its whole records\n");
    return 0;
}
'''


def cut(source, start, end):
    i = source.index(start)
    return source[i:source.index(end, i)]


def main():
    src = (ROOT / 'runtime' / 'vk_pipeline.c').read_text(encoding='utf-8')
    production = cut(src, '#define PB_MAX_BIN 8', '// Opens pipeline-binaries.bin')
    work = ROOT / 'build' / 'test' / 'pb_torn'
    work.mkdir(parents=True, exist_ok=True)
    c = work / 'check.c'
    c.write_text(HARNESS + production + MAIN, encoding='utf-8')
    exe = work / ('check.exe' if os.name == 'nt' else 'check')
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.check_call([compiler(), '-O1', '-g', '-Wall', '-Wno-unused-function', '-Wno-unused-variable',
                           '-march=x86-64-v3', '-I', str(ROOT / 'runtime'), str(c), '-o', str(exe)], env=env)
    return subprocess.run([str(exe), str(work / 'pipeline-binaries.bin')], env=env, timeout=60).returncode


if __name__ == '__main__':
    sys.exit(main())
