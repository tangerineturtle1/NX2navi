"""Check the translated shaders an install's shader cache holds, offline: every <key>.spv with its <key>.res.json, from
<cache>/shader-cache.pack and from the loose files (a loose file stands for its packed copy, as the game reads them).

The game checks little of a cached shader when it loads one (runtime/gfx_shader.c disk_load_from: the reflection is one
JSON object that names its stage, the SPIR-V has its magic and whole instructions) and hands the module to the driver
as it is. A module the driver then compiles wrongly, or crashes on, shows as a missing draw or a crash in play, far
from its cause. This tool puts the question to the whole cache at once:

  1. the game's own checks, so the count says how many entries the game would translate again;
  2. spirv-val on each module that passes them (--target-env vulkan1.3, as tools/shader_census.py validates fresh
     translations), unless --no-val is given or spirv-val is not found.

The modules are the translator's output; the pipeline code rewrites a copy of each before the driver sees it
(runtime/vk_pipeline.c make_module), and that copy is not what is checked here. tools/shader_census.py --translate
validates translations made now from the game files; this validates what a folder already holds, which is also what
earlier translators wrote and what an interrupted write left.

    python tools\\validate_shader_cache.py <install folder>\\shader-cache [--every N] [--jobs N] [--no-val] [--out FILE]

--every N checks every N-th shader (by key order), for a quick look at a large cache; --out writes one line per failed
shader (key, origin, what failed). Exit code 0 when every checked entry passed, 1 otherwise, 2 for a folder that is not
a shader cache. The game may be running: nothing is written to the cache.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pack_shader_cache as psc  # noqa: E402

SPV, RES = psc.KINDS['.spv'], psc.KINDS['.res.json']
SPIRV_MAGIC = 0x07230203
STAGES = (b'"vs"', b'"ps"', b'"cs"', b'"ts"', b'"ms"')   # gfx_shader.c reflection_valid
TARGET_ENV = 'vulkan1.3'


def find_spirv_val():
    """The path of spirv-val, or None: MSYS2 UCRT64's on Windows, else the one on PATH."""
    msys = Path(r'C:\msys64\ucrt64\bin\spirv-val.exe')
    if os.name == 'nt' and msys.is_file():
        return str(msys)
    return shutil.which('spirv-val')


def spirv_problem(data):
    """What gfx_shader.c disk_load_from refuses in a module ('' when it takes it): size, magic, and every instruction
    after the five header words having a word count that ends inside the module (spirv_complete)."""
    if data is None:
        return 'no .spv'
    if len(data) < 20 or len(data) % 4:
        return 'the .spv is %d bytes: not a whole SPIR-V module' % len(data)
    words = len(data) // 4
    if struct.unpack_from('<I', data, 0)[0] != SPIRV_MAGIC:
        return 'the .spv has another magic number'
    i = 5
    while i < words:
        n = struct.unpack_from('<I', data, 4 * i)[0] >> 16
        if not n:
            return 'an instruction of zero words at word %d (a zero-filled stretch)' % i
        i += n
    return '' if i == words else 'the last instruction runs past the end of the module'


def reflection_problem(data):
    """What gfx_shader.c reflection_valid refuses in a .res.json ('' when it takes it)."""
    if data is None:
        return 'no .res.json'
    if b'\0' in data:
        return 'the .res.json holds NUL bytes (an interrupted write)'
    body = data.strip()
    if len(body) < 2 or body[:1] != b'{' or body[-1:] != b'}':
        return 'the .res.json is not one JSON object'
    at = body.find(b'"stage"')
    quote = body.find(b'"', at + 7) if at >= 0 else -1
    if quote < 0 or not body.startswith(STAGES, quote):
        return 'the .res.json names no stage the game knows'
    return ''


class Cache:
    """The .spv and .res.json entries of a shader cache folder: loose files, then the pack."""

    def __init__(self, folder):
        self.folder = Path(folder)
        self.loose = {k: p for k, p in psc.loose_files(self.folder).items() if k[0] in (SPV, RES)}
        self.pack_path = self.folder / psc.PACK
        self.packed = {k: v for k, v in psc.read_pack(self.pack_path).items() if k[0] in (SPV, RES)}

    def keys(self):
        """Every key with a .spv or a .res.json, sorted."""
        return sorted({name for _, name in self.loose} | {name for _, name in self.packed})

    def read(self, kind, name, pack_file):
        """(data or None, 'loose' | 'pack' | '') of one entry."""
        path = self.loose.get((kind, name))
        if path is not None:
            return psc.read_loose(path), 'loose'
        at = self.packed.get((kind, name))
        if at is None or pack_file is None:
            return None, ''
        return os.pread(pack_file, at[1], at[0]) if hasattr(os, 'pread') else pread(pack_file, at[1], at[0]), 'pack'


def pread(fd, size, offset):
    """os.pread where the platform has none (Windows): callers hold one descriptor per thread."""
    os.lseek(fd, offset, os.SEEK_SET)
    return os.read(fd, size)


def check_one(cache, name, val, pack_file):
    """(key hex, origin, problem): problem '' when the entry passed every check asked for."""
    spv, origin = cache.read(SPV, name, pack_file)
    res, res_origin = cache.read(RES, name, pack_file)
    origin = origin or res_origin
    problem = reflection_problem(res) or spirv_problem(spv)
    if not problem and val:
        r = subprocess.run([val, '--target-env', TARGET_ENV, '-'], input=spv, capture_output=True)
        if r.returncode:
            text = (r.stderr or r.stdout).decode('utf-8', 'replace').strip().splitlines()
            problem = 'spirv-val: ' + (text[0] if text else 'exit code %d' % r.returncode)
    return name.hex(), origin, problem


def group_of(problem):
    """A problem with its numbers and ids taken out, so the same fault in many shaders counts as one group."""
    return re.sub(r"'\d+\[%[^\]]*\]'|\b\d+\b", 'N', re.sub(r'^spirv-val: error: line \d+: ', 'spirv-val: ', problem))


def validate(folder, every=1, jobs=0, val=None, out=None, say=print):
    """Checks the cache at folder; returns (checked, failures) with failures a list of (key, origin, problem)."""
    cache = Cache(folder)
    names = cache.keys()[::max(1, every)]
    jobs = jobs or min(32, (os.cpu_count() or 4))
    failures, done, t0, last = [], 0, time.monotonic(), time.monotonic()

    def work(chunk):
        # one descriptor of the pack per task: positioned reads without a shared file position
        fd = os.open(cache.pack_path, os.O_RDONLY | getattr(os, 'O_BINARY', 0)) if cache.packed else None
        try:
            return [check_one(cache, n, val, fd) for n in chunk]
        finally:
            if fd is not None:
                os.close(fd)

    chunks = [names[i:i + 64] for i in range(0, len(names), 64)]
    with ThreadPoolExecutor(max_workers=jobs) as ex:
        for results in ex.map(work, chunks):
            done += len(results)
            failures += [r for r in results if r[2]]
            if time.monotonic() - last >= 10:
                last = time.monotonic()
                say(f'  {done} of {len(names)} shaders checked, {len(failures)} failed, {last - t0:.0f} s')
    if out:
        Path(out).write_text(''.join(f'{k} {o or "-"} {p}\n' for k, o, p in failures), encoding='utf-8')
    return len(names), failures


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('cache', help='the shader cache folder (<install folder>/shader-cache)')
    ap.add_argument('--every', type=int, default=1, help='check every N-th shader only')
    ap.add_argument('--jobs', type=int, default=0, help='threads (default: the CPUs, at most 32)')
    ap.add_argument('--no-val', action='store_true', help="the game's own checks only, without spirv-val")
    ap.add_argument('--out', help='write one line per failed shader to this file')
    args = ap.parse_args()
    folder = Path(args.cache)
    if not folder.is_dir():
        print(f'{folder}: not a folder', file=sys.stderr)
        return 2
    val = None if args.no_val else find_spirv_val()
    if not args.no_val and not val:
        print("spirv-val not found (MSYS2: pacman -S mingw-w64-ucrt-x86_64-spirv-tools): the game's own checks only")
    t0 = time.monotonic()
    checked, failures = validate(folder, args.every, args.jobs, val, args.out)
    if not checked:
        print(f'{folder}: no translated shaders (no {psc.PACK} and no loose .spv files)', file=sys.stderr)
        return 2
    refused = sum(1 for f in failures if not f[2].startswith('spirv-val: '))
    print(f'{checked} shaders checked in {time.monotonic() - t0:.0f} s: {checked - len(failures)} passed, {refused} '
          f"the game would translate again (its own checks), {len(failures) - refused} refused by spirv-val"
          + ('' if val else ' (not run)'))
    groups = Counter(group_of(f[2]) for f in failures)
    for text, n in groups.most_common(20):
        first = next(f for f in failures if group_of(f[2]) == text)
        print(f'  {n:6d}  {text}   (first: {first[0][:16]}, {first[1] or "no file"})')
    if len(groups) > 20:
        print(f'  ... and {len(groups) - 20} more kinds; --out FILE lists every failed shader')
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
