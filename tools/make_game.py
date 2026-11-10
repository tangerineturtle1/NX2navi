"""Build Ocarina of Time (Switch 2, game 1.1.1) for Windows on the player's PC, from the player's own game files.

    python -I tools/make_game.py --nup "C:\\path\\game.nup" --out "C:\\Games\\Ocarina"
    python -I tools/make_game.py --exefs "C:\\path\\exefs" --romfs "C:\\path\\romfs" --out "C:\\Games\\Ocarina"
    python -I tools/make_game.py --nup "C:\\path\\game.nup" --check          # only checks the game's version
    python -I tools/make_game.py --rebuild --out "C:\\Games\\Ocarina"         # a newer kit, the files of the last build
    python -I tools/make_game.py --rebuild --out "C:\\Games\\Ocarina" --debug # a debug build with symbols beside it

--debug builds <out>/ocarina-debug.exe (ocarina-debug on Linux) beside the release one, which it leaves alone: the
runtime at -O0 with full debug info, the lifted code still at -O1 (at -O0 it is far too slow to play and its huge
functions overflow the stack) with line tables only, frame pointers kept in both. On Windows with lld the debug info
is CodeView in <out>/ocarina-debug.pdb (Visual Studio, WinDbg, RemedyBG, RAD Debugger); without lld it is DWARF
inside the exe (gdb, lldb). On Linux it is DWARF inside the binary. Its objects go to build-cache/obj-debug, the
prebuilt runtime library is not used (it carries no debug info), and its compile and shaders steps are recorded
apart from the release build's, so switching between the two rebuilds neither from scratch.

Steps, each skipped when a run before finished it for the same inputs (state in <out>/build-cache/state.json):
  1. check    the title (0400486024DEC000) and main's build id (game 1.1.1)
  2. exefs    the five ExeFS files, every module's segments unpacked (ZBIC through the player's own `zbic` package)
              and stored plainly, as the runtime's loader reads them -> <out>/exefs
  3. romfs    every RomFS file, checked against the container's sha256, into one image -> <out>/romfs/romfs.bin and
              index.json (16-byte aligned, in the byte order of the paths). When the lift and the compile follow, it
              runs on a thread of its own beside them: only the RomFS index and romfs.c wait for the image
  4. lift     recomp/gen.py with recomp/patches-v111.txt -> <out>/build-cache/lift, plus the two SDK bring-up fixes
  5. compile  the lifted C and runtime/ with MSYS2's clang, linked into <out>/ocarina.exe; the MSYS2 DLLs it loads,
              the shader translator and romfs-index.bin beside it
  6. shaders  every shader in the game files (tools/prepare_shaders.py, default bind state) translated, and each one's
              GPU driver build done once into the driver's own cache, so play stops for them far less often
              (`ocarina.exe --prepare-shaders`); variants met in play with another bind state are still translated
              then, and the driver cache is per executable and is lost when the driver clears it
  7. prompts  only with button art in the kit (<kit>/prompt-art, tools/make_launcher.py and make_kit.py
              --prompt-art): tools/pad_prompts.py draws the Xbox, PlayStation and keyboard and mouse button prompts
              from it into <out>/romfs-patches-* (the Controls menu's Button prompts loads them), the keys as the
              bind_ lines of <out>/nx2navi.ini give them. Its key names everything it reads (the RomFS, the art, the
              tools, those lines), so it runs again only when one of them changed or a folder is gone, and running
              it makes no other step run again. It is last: a failure here leaves the game and its shaders complete.
              Without art the step is skipped and the game shows its own prompts, as before

The exefs and romfs steps depend only on the game files; lift and compile depend on this kit's code too. A newer kit
therefore lifts and compiles again but keeps the unpacked game files, and --rebuild needs no game files at all. The
compile step keeps every object with the dependency list clang wrote for it (build-cache/obj/objects.json) and
compiles a file again only when it, a header it includes, the flags or the compiler changed; the kit's own folder
is not part of that key, so a kit in another folder (the launcher keeps each version in its own) reuses the objects.
A kit with lib/libnx2navirt.a built by --make-runtime-lib for the same runtime/ and the same compiler links the
runtime from it and compiles only the runtime files that include headers written per install. The new ocarina.exe
replaces the old one only once it links (with lld when the toolchain has it), so a failed build leaves the last good
one in place. Compiles run on every logical processor, the largest files first. The lifted code is built for this
PC's x86-64 level (cpu_level), with MSYS2 CLANG64's clang when it sits beside the UCRT64 folder and reports the same
version (fast_clang: the same objects, about a fifth faster).

    python tools/make_game.py --make-runtime-lib <kit>/lib --msys <ucrt64>   # the release's prebuilt runtime

On Linux the same steps make <out>/ocarina with the clang, clang++ and lld on PATH and the system's libraries
(Vulkan, SDL3, Opus, libvpx, dav1d, zstd; libstdc++ static, rpath $ORIGIN), as tools/build_game.py's Linux
build does: the runtime's *_sdl.* and platform/linux.c take the place of its *_win.*, platform/win32.c and
nx2navi.rc, and --msys is not used. README-BuildKit.md, "Linux", lists the packages.

Nothing is written outside --out (but for --make-runtime-lib DIR, which writes DIR). The game folder then holds
only what this PC built from the player's files: never
share it. Needs Python 3.10+ with numpy, lz4, zstandard and zbic (pip), with fonttools and pillow too when the kit has
button art, and MSYS2 UCRT64's clang and libraries (README-BuildKit.md lists the commands). Delete <out>/build-cache
afterwards to free about 5 GB; the next build then compiles everything again.

--events adds one machine-readable line per step and progress change to stdout, each starting with "@@" and followed
by a JSON object (launcher/build.py reads them): {"ev": "step", "step": ..., "state": "start" | "done" | "skip"},
{"ev": "progress", "step": ..., "done": n, "total": n, "unit": ...}, {"ev": "phase", ...}, {"ev": "error", "message":
...} and {"ev": "end", ...}.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

KIT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(KIT / 'tools'))
import nup  # noqa: E402
import prompt_art  # noqa: E402

TITLE_ID = 0x0400486024DEC000
MAIN_BUILD_ID = 'e0f3ccfaf50065ae4ea01eb5aeb13b1a'   # main of game 1.1.1
EXEFS_FILES = ('main', 'main.npdm', 'sdk', 'subsdk0', 'rtld')
MODULES = ('main', 'sdk', 'subsdk0', 'rtld')
ALIGN = 16
STATE_VERSION = 2
STEPS = ('exefs', 'romfs', 'lift', 'compile', 'shaders', 'prompts')
# Steps whose key names every input they read rather than the step before them: a step before them that runs does
# not make them run again, and they make no step after them run again.
OWN_KEY_STEPS = ('prompts',)
# The Python packages tools/pad_prompts.py imports (pip name: import name), checked before a build with button art.
PROMPT_PACKAGES = {'zstandard': 'zstandard', 'fonttools': 'fontTools', 'pillow': 'PIL'}
# The line tools/pad_prompts.py prints for each overlay folder it wrote.
PROMPTS_WRITTEN = re.compile(r'^.+: \d+ files, the font grew by -?\d+ bytes$')

# Bring-up fixes in the lifted SDK, each anchored on exact generated text that must occur once.
LIFT_FIXES = [
    # nn::oe::SetupGpuErrorHandler (sdk+0x59a430) registers a system worker the host does not provide: GPU error
    # reporting is not needed, so it returns at once.
    ('sdk', 'void f_700579a430(Cpu *c) {\n    FN_ENTER(0x700579a430ull);\n',
     'void f_700579a430(Cpu *c) {\n    c->x[0] = 0; return;   /* bring-up: GpuErrorHandler (make_game.py) */\n'
     '    FN_ENTER(0x700579a430ull);\n'),
    # The thread-start path loads TPIDR_EL0 from TLR+0x1F0 before the SDK stored its TLS there for a host-created
    # thread: a zero slot keeps the TLS pointer the host seeded.
    ('sdk', '    { uint64_t ea = c->x[19] + 0x1b8ull; ST64(ea, x8); }\n    x8 = (uint64_t)(c->tpidrro);\n'
     '    { uint64_t ea = x8 + 0x1f0ull; uint64_t v = LD64(ea); x8 = (uint64_t)(v); }\n    c->tpidr = x8;\n',
     '    { uint64_t ea = c->x[19] + 0x1b8ull; ST64(ea, x8); }\n    x8 = (uint64_t)(c->tpidrro);\n'
     '    { uint64_t ea = x8 + 0x1f0ull; uint64_t v = LD64(ea); x8 = (uint64_t)(v); }\n'
     '    if (x8) c->tpidr = x8;   /* zero slot: keep the host\'s TLS (make_game.py) */\n'),
]
WIN = os.name == 'nt'
if WIN:
    LINK_LIBS = ['-lvulkan-1', '-lSDL3', '-lopus', '-lvpx', '-l:libdav1d.a', '-lzstd', '-l:libatomic.a', '-lbcrypt',
                 '-limm32', '-luuid', '-lole32', '-luser32', '-lshell32', '-lgdi32', '-ladvapi32', '-lwinmm']
else:   # tools/build_game.py's Linux libraries, plus dav1d (the AV1 movies) and libatomic
    LINK_LIBS = ['-lvulkan', '-lSDL3', '-lopus', '-lvpx', '-ldav1d', '-lzstd', '-l:libstdc++.a', '-latomic',
                 '-lm', '-lpthread', '-ldl']
SYSTEM_DLLS_SKIP = {'vulkan-1.dll'}   # the graphics driver's loader, never a copy
EXE_NAME = 'ocarina.exe' if WIN else 'ocarina'
XLATE_NAME = 'nx2navi_xlate.exe' if WIN else 'nx2navi_xlate'
# Headers this build writes per install (gen.py, romfs_index.py): a runtime source that includes one, directly or
# through a runtime header, is compiled on the player's PC; every other runtime source can come prebuilt.
GENERATED_HEADERS = ('layout.h', 'patches.h', 'romfs_config.h')
RUNTIME_LIB = 'libnx2navirt.a'
RUNTIME_PATTERNS = ('*.c', '*.cpp', 'platform/*.c', 'ui/*.cpp', 'third_party/imgui/*.cpp',
                    'third_party/imgui/backends/*.cpp')
INCLUDE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]+)"', re.M)

# Runs a script the way `python script.py` does, with the script's folder first on sys.path. The launcher's
# embeddable Python leaves the script folder off sys.path (its ._pth file), so every script this build starts goes
# through this.
RUN_SCRIPT = ('import os, runpy, sys; s = sys.argv[1]; sys.argv = sys.argv[1:]; '
              'sys.path.insert(0, os.path.dirname(os.path.abspath(s))); runpy.run_path(s, run_name="__main__")')

EVENTS = False
DEBUG = False   # --debug
# The steps whose output differs in a --debug build: recorded under '<step>-debug' in state.json, so the release and
# the debug build each keep their own and switching between them rebuilds neither.
DEBUG_STEPS = ('compile', 'shaders')
OUT_LOCK = threading.Lock()   # the RomFS step prints from its own thread (main)


def say(msg):
    with OUT_LOCK:
        print(msg, flush=True)


def emit(ev, **fields):
    """One "@@{json}" line for the launcher (--events); nothing otherwise."""
    if EVENTS:
        line = '@@' + json.dumps({'ev': ev, **fields}, ensure_ascii=False)
        with OUT_LOCK:
            print(line, flush=True)


def fail(msg):
    emit('error', message=msg)
    with OUT_LOCK:
        print(f'\nerror: {msg}', file=sys.stderr, flush=True)
    sys.exit(1)


class Background:
    """One step on a thread of its own. join() waits for it and raises its failure (fail's SystemExit included) in
    the caller; failed() tells the caller early, so it can stop work that would be thrown away."""

    def __init__(self, fn):
        self.exc = None
        self.thread = threading.Thread(target=self._run, args=(fn,), name='romfs', daemon=True)
        self.thread.start()

    def _run(self, fn):
        try:
            fn()
        except BaseException as e:   # noqa: B036 - handed to join()
            self.exc = e

    def failed(self):
        return self.exc is not None

    def join(self):
        self.thread.join()
        if self.exc is not None:
            raise self.exc


def py_cmd(script, *args):
    """The command line that runs a Python script of this kit with this interpreter (see RUN_SCRIPT)."""
    flags = ['-I'] if sys.flags.isolated else []
    return [sys.executable, *flags, '-c', RUN_SCRIPT, str(script), *map(str, args)]


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while b := f.read(8 << 20):
            h.update(b)
    return h.hexdigest()


def tree_digest(root, patterns):
    """sha256 over the relative names and contents of the files under root matching the glob patterns."""
    h = hashlib.sha256()
    files = sorted({p for pat in patterns for p in Path(root).glob(pat) if p.is_file() and '__pycache__' not in p.parts})
    for p in files:
        h.update(p.relative_to(root).as_posix().encode() + b'\0')
        h.update(p.read_bytes())
        h.update(b'\0')
    return h.hexdigest()


# ---------------------------------------------------------------- input

class Source:
    """The game files, from a .nup container or from extracted exefs/romfs folders."""

    def __init__(self, a):
        self.n = None
        if a.nup:
            try:
                self.n = nup.Nup(a.nup)
            except (OSError, nup.NupError) as e:
                fail(f'cannot read {a.nup}: {e}')
            # The base title and its update (title id + 0x800), the update first and the newest of each kind first.
            # A module comes from the first of them that holds it whole: an update's ExeFS may leave a module out or
            # hold something else under its name.
            pkgs = [p for p in self.n.packages if p['title_id'] in (TITLE_ID, TITLE_ID | 0x800)]
            if not pkgs:
                ids = ', '.join(f"{p['title_id']:016X}" for p in self.n.packages)
                fail(f'this .nup holds {ids}, not Ocarina of Time (0400486024DEC000)')
            pkgs.sort(key=lambda p: (p['kind'] != 1, -p['version']))
            self.exefs = {}
            for name in EXEFS_FILES:
                for p in pkgs:
                    fi = dict(self.n.tree(p['exefs'])).get(name) if p['exefs'] < self.n.ndir else None
                    magic = b'META' if name.endswith('.npdm') else b'NSO0'
                    if fi is not None and self.n.size(fi) >= 0x100 and next(self.n.chunks(fi))[:4] == magic:
                        self.exefs[name] = fi
                        break
            self.romfs = []
            for p in pkgs:
                if p['romfs'] < self.n.ndir:
                    self.romfs = list(self.n.tree(p['romfs']))
                if self.romfs:
                    break
            st = os.stat(a.nup)
            self.identity = f'nup {os.path.abspath(a.nup)} {st.st_size} {int(st.st_mtime)}'
        else:
            self.exefs_dir, self.romfs_dir = Path(a.exefs), Path(a.romfs)
            for d in (self.exefs_dir, self.romfs_dir):
                if not d.is_dir():
                    fail(f'{d} is not a folder')
            self.exefs = {name: self.exefs_dir / name for name in EXEFS_FILES if (self.exefs_dir / name).is_file()}
            self.romfs = [(p.relative_to(self.romfs_dir).as_posix(), p) for p in self.romfs_dir.rglob('*') if p.is_file()]
            # every file's size and modification time, not only their count: a RomFS extracted again (or edited)
            # with the same number of files, or a replaced sdk, kept the old unpacked files ("done before")
            h = hashlib.sha256()
            for rel, f in sorted([(n, self.exefs[n]) for n in self.exefs] + self.romfs):
                st = f.stat()
                h.update(f'{rel}\0{st.st_size}\0{st.st_mtime_ns}\n'.encode('utf-8', 'surrogateescape'))
            self.identity = (f'folders {self.exefs_dir.resolve()} {self.romfs_dir.resolve()} {len(self.romfs)} '
                             f'{h.hexdigest()[:32]}')
        missing = [x for x in EXEFS_FILES if x not in self.exefs]
        if missing:
            fail(f'the ExeFS lacks {", ".join(missing)} (or holds something else under that name)')
        if not self.romfs:
            fail('the RomFS is empty')

    def read_exefs(self, name):
        return self.n.read(self.exefs[name]) if self.n else self.exefs[name].read_bytes()

    def romfs_size(self, ref):
        return self.n.size(ref) if self.n else ref.stat().st_size

    def romfs_chunks(self, ref):
        if self.n:
            yield from self.n.chunks(ref)
            return
        with open(ref, 'rb') as f:
            while b := f.read(nup.CHUNK):
                yield b


class BuiltSource:
    """--rebuild: the ExeFS and RomFS an earlier run unpacked into the game folder, under that run's input identity.
    The ExeFS modules are stored plainly there but keep their headers, so check() reads the same build id."""

    def __init__(self, out, state):
        self.out = out
        if state.get('version') != STATE_VERSION or not state.get('input'):
            fail(f'{out} holds no finished build of this kit to rebuild from: give --nup, or --exefs and --romfs')
        for step in ('exefs', 'romfs'):
            if step not in state.get('steps', {}):
                fail(f'the last build in {out} did not finish its {step} step: give the game files again')
        missing = [n for n in EXEFS_FILES if not (out / 'exefs' / n).is_file()]
        missing += [f'romfs/{n}' for n in ('romfs.bin', 'index.json') if not (out / 'romfs' / n).is_file()]
        if missing:
            fail(f'{out} lacks {", ".join(missing)}: give the game files again')
        self.identity = state['input']

    def read_exefs(self, name):
        return (self.out / 'exefs' / name).read_bytes()


def check(src):
    main = src.read_exefs('main')
    if main[:4] != b'NSO0':
        fail('main is not an NSO module')
    build = main[0x40:0x50].hex()
    if build != MAIN_BUILD_ID:
        fail(f'this is not game version 1.1.1: main has build {build[:8]}, the recomp supports build {MAIN_BUILD_ID[:8]}. '
             'Install the 1.1.1 update and dump the game again.')
    return main


# ---------------------------------------------------------------- exefs

def flat_nso(raw, name):
    """An NSO with its segments stored uncompressed (the runtime's loader reads only those); segment hashes kept."""
    flags = struct.unpack_from('<I', raw, 0xC)[0]
    out = bytearray(raw[:0x100])
    for i, seg in enumerate(('text', 'ro', 'data')):
        file_off, mem_off, size = struct.unpack_from('<III', raw, 0x10 + i * 0x10)
        stored = struct.unpack_from('<I', raw, 0x60 + i * 4)[0]
        data = raw[file_off:file_off + (stored if flags >> i & 1 else size)]
        if flags >> i & 1:
            if flags & 0x80:
                try:
                    import zbic
                except ImportError:
                    fail('the game\'s modules need the zbic package: pip install zbic')
                data = zbic.decompress(data)
            else:
                import lz4.block
                data = lz4.block.decompress(data, uncompressed_size=size)
        if len(data) != size:
            fail(f'{name}: {seg} unpacks to {len(data):#x} bytes, its header says {size:#x}')
        if flags >> (3 + i) & 1 and hashlib.sha256(data).digest() != raw[0xA0 + 0x20 * i:0xC0 + 0x20 * i]:
            fail(f'{name}: the {seg} segment is damaged (sha256 mismatch)')
        struct.pack_into('<III', out, 0x10 + i * 0x10, len(out), mem_off, size)
        struct.pack_into('<I', out, 0x60 + i * 4, size)
        out += data
    struct.pack_into('<I', out, 0xC, flags & ~0x87)   # segments stored plainly, no ZBIC
    return bytes(out)


def step_exefs(src, out):
    dst = out / 'exefs'
    dst.mkdir(parents=True, exist_ok=True)
    for i, name in enumerate(EXEFS_FILES):
        raw = src.read_exefs(name)
        data = flat_nso(raw, name) if name in MODULES else raw
        (dst / name).write_bytes(data)
        say(f'  {name:9} {len(raw) / 1e6:6.1f} MB -> {len(data) / 1e6:6.1f} MB')
        emit('progress', step='exefs', done=i + 1, total=len(EXEFS_FILES), unit='files')


def exefs_digest(out):
    return tree_digest(out / 'exefs', EXEFS_FILES)


# ---------------------------------------------------------------- romfs

def step_romfs(src, out, jobs):
    dst = out / 'romfs'
    dst.mkdir(parents=True, exist_ok=True)
    files = sorted(src.romfs, key=lambda e: e[0].encode())
    total = sum(src.romfs_size(ref) for _, ref in files)
    free = shutil.disk_usage(dst).free
    if free < total + (1 << 30):
        fail(f'{dst} needs {total / 1e9:.1f} GB free for the RomFS image, it has {free / 1e9:.1f} GB')
    say(f'  {len(files)} files, {total / 1e9:.1f} GB')
    entries, h, pos, done, t0 = [], hashlib.sha256(), 0, 0, time.time()
    part = dst / 'romfs.bin.part'
    emit('progress', step='romfs', done=0, total=total, unit='bytes')

    def load(e):   # small files are read whole on a worker; big ones are streamed by the writer
        rel, ref = e
        return None if src.romfs_size(ref) > (64 << 20) else b''.join(src.romfs_chunks(ref))

    with open(part, 'wb', buffering=16 << 20) as img, concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        batch = 256
        for start in range(0, len(files), batch):
            group = files[start:start + batch]
            for (rel, ref), data in zip(group, ex.map(load, group)):
                pad = -pos % ALIGN
                if pad:
                    img.write(b'\0' * pad)
                    h.update(b'\0' * pad)
                    pos += pad
                size = src.romfs_size(ref)
                entries.append({'path': rel, 'offset': pos, 'size': size})
                for chunk in ((data,) if data is not None else src.romfs_chunks(ref)):
                    img.write(chunk)
                    h.update(chunk)
                pos += size
                done += size
            emit('progress', step='romfs', done=done, total=total, unit='bytes')
            if start // batch % 40 == 0:
                say(f'  RomFS {done / 1e9:5.1f} of {total / 1e9:.1f} GB, {time.time() - t0:.0f} s')
    part.replace(dst / 'romfs.bin')
    index = {'status': 'pass', 'romfs_size': pos, 'romfs_sha256': h.hexdigest(), 'files': entries}
    (dst / 'index.json').write_text(json.dumps(index), encoding='utf-8')
    say(f'  RomFS image {pos / 1e9:.2f} GB in {time.time() - t0:.0f} s')


# ---------------------------------------------------------------- lift

EMITTED = re.compile(r'emitted (\d+)/(\d+) chunks')


def lift_digest():
    """The lifter's code, the patch table and the bring-up fixes: what the lifted C depends on besides the ExeFS."""
    h = hashlib.sha256(tree_digest(KIT / 'recomp', ('*.py', 'patches-v111.txt')).encode())
    h.update(repr(LIFT_FIXES).encode())
    return h.hexdigest()


def step_lift(out, cache):
    """gen.py into build-cache/lift. The folder is kept: gen.py rewrites only the chunks whose text changed and
    removes the ones it no longer writes."""
    lift = cache / 'lift'
    cmd = py_cmd(KIT / 'recomp' / 'gen.py', '--exefs', out / 'exefs', '--out', lift,
                 '--patches', KIT / 'recomp' / 'patches-v111.txt')
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                         errors='replace', bufsize=1)
    for line in p.stdout:
        line = line.rstrip('\n')
        say(f'  {line}')
        m = EMITTED.search(line)
        if m:
            emit('progress', step='lift', done=int(m.group(1)), total=int(m.group(2)), unit='chunks')
    if p.wait():
        fail('the code generator failed (see above)')
    for mod, old, new in LIFT_FIXES:
        hits = [p for p in sorted((lift / mod).glob('*.c')) if old in p.read_text(encoding='utf-8')]
        if len(hits) != 1 or hits[0].read_text(encoding='utf-8').count(old) != 1:
            fail(f'a bring-up fix found its anchor {len(hits)} times in the lifted {mod}, not once')
        p = hits[0]
        p.write_text(p.read_text(encoding='utf-8').replace(old, new), encoding='utf-8', newline='\n')


# ---------------------------------------------------------------- compile

def modules_c(lift, out, gen):
    layout = json.loads((lift / 'layout.json').read_text(encoding='utf-8'))
    L = ['#include "dispatch.h"', '#include "host.h"', '#include "layout.h"', '']
    for m in layout:
        n = m['name']
        L += [f'extern const uint64_t {n}_fn_addrs[];', f'extern const GuestFn {n}_fn_ptrs[];',
              f'extern const uint64_t {n}_fn_count;']
    L += ['', 'const RtLayoutEntry rt_layout[] = {']
    L += [f'    {{"{m["name"]}", MOD_{m["name"].upper()}_BASE, MOD_{m["name"].upper()}_SIZE}},' for m in layout]
    L += ['};', f'const int rt_layout_count = {len(layout)};', '', 'int rt_linked_modules(RtModule *out, int max) {',
          '    int n = 0;']
    for m in layout:
        n, u = m['name'], m['name'].upper()
        L.append(f'    if (n < max) out[n++] = (RtModule){{"{n}", MOD_{u}_BASE, MOD_{u}_SIZE, {n}_fn_addrs, {n}_fn_ptrs, '
                 f'{n}_fn_count}};')
    L += ['    return n;', '}', '', 'const RtFileHash rt_exefs_sha256[] = {']
    L += [f'    {{"{name}", "{sha256_file(out / "exefs" / name)}"}},' for name in EXEFS_FILES]
    L += ['};', f'const int rt_exefs_sha256_count = {len(EXEFS_FILES)};', '',
          f'const char rt_default_exefs[] = {json.dumps(str(out / "exefs"))};',
          f'const char rt_default_romfs[] = {json.dumps(str(out / "romfs"))};',
          f'const char rt_default_profile[] = {json.dumps(str(out / "profile"))};',
          'const int rt_game_version = 111;',
          'const unsigned long long rt_app_pool_bytes = 9809ull << 20;   /* the Switch 2 application pool */',
          'const int rt_nvn_api_version[2] = {231, 40};   /* NVN2: main asks for major 231, minor 40 or later */',
          f'const char rt_port_version[] = {json.dumps(kit_version())};']
    p = gen / 'modules.c'
    write_if_changed(p, '\n'.join(L) + '\n')
    if WIN:   # on Linux ui/menu_sdl.cpp defines it
        write_if_changed(gen / 'shim.c', '/* Defined by the SDL window path only; this build uses the Win32 one. */\n'
                                         'void menu_sdl_event(void *e) { (void)e; }\n')
    return p


def write_if_changed(path, text):
    if not path.is_file() or path.read_text(encoding='utf-8') != text:
        path.write_text(text, encoding='utf-8')


def kit_version():
    v = KIT / 'VERSION'
    return v.read_text(encoding='utf-8').strip() if v.is_file() else '0.0.1-ocarina'


def pe_imports(path):
    """The DLL names a PE file imports (its import directory), lower case."""
    d = Path(path).read_bytes()
    pe = struct.unpack_from('<I', d, 0x3C)[0]
    nsec, opt = struct.unpack_from('<H', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
    sec = pe + 24 + opt
    rva = struct.unpack_from('<I', d, pe + 24 + 120)[0]   # PE32+ data directory 1: imports

    def off(r):
        for i in range(nsec):
            va, vs, raw = (struct.unpack_from('<I', d, sec + 40 * i + k)[0] for k in (12, 8, 20))
            if va <= r < va + max(vs, 1):
                return r - va + raw
        return None

    names, at = [], off(rva) if rva else None
    while at is not None:
        name_rva = struct.unpack_from('<I', d, at + 12)[0]
        if not name_rva:
            break
        o = off(name_rva)
        names.append(d[o:d.index(b'\0', o)].decode('ascii').lower())
        at += 20
    return names


def copy_if_changed(src, dst):
    """Copy src over dst unless dst already has its size and modification time. A file the running game holds open
    cannot be replaced on Windows: that stops the build with a message instead of a traceback."""
    s = os.stat(src)
    try:
        d = os.stat(dst)
        if d.st_size == s.st_size and int(d.st_mtime) == int(s.st_mtime):
            return False
    except FileNotFoundError:
        pass
    try:
        shutil.copy2(src, dst)
    except PermissionError:
        fail(f'{dst} is in use: close the game and try again')
    return True


DLSS_DLL, DLSS_NOTICE = 'nvngx_dlss.dll', 'nvngx_dlss-NOTICE.txt'   # tools/dlss_pin.py, in the kit's bin/
# DLSS Frame Generation (runtime/vk_dlssg.c) loads these from beside the game: nx2navi_dlssg.dll (runtime/dlssg/,
# tools/build_dlssg.py), NVIDIA Streamline's interposer and the plugins it uses, and NGX's frame generation library.
# A release carries them with their licence texts in bin/dlssg/ (tools/make_launcher.py --dlssg); without the whole set
# the game leaves DLSS Frame Generation off.
DLSSG_DIR = 'dlssg'
DLSSG_FILES = ('nx2navi_dlssg.dll', 'sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.pcl.dll', 'sl.reflex.dll',
               'nvngx_dlssg.dll')


def notice_sha256(path):
    """The sha256 line of a DLSS notice: the DLL a kit placed beside the game."""
    try:
        for line in path.read_text(encoding='utf-8').splitlines():
            if line.startswith('sha256 '):
                return line.split()[1].lower()
    except (OSError, UnicodeDecodeError):
        pass
    return None


def place_dlss(out):
    """NVIDIA's DLSS library from the kit's bin/ beside the game, where NGX loads it (runtime/vk_dlaa.c), with its
    notice. A different nvngx_dlss.dll the player put there (another DLSS version) is kept; the one an earlier build
    placed, named by the notice beside it, is replaced. Returns a line for the build's log when it kept the player's."""
    src, dst, note = KIT / 'bin' / DLSS_DLL, out / DLSS_DLL, KIT / 'bin' / DLSS_NOTICE
    if not src.is_file():
        return None
    if dst.is_file():
        s, d = os.stat(src), os.stat(dst)
        if d.st_size != s.st_size or int(d.st_mtime) != int(s.st_mtime):
            have = sha256_file(dst)
            if have != notice_sha256(out / DLSS_NOTICE) and have != sha256_file(src):
                return f"{DLSS_DLL} kept: the one already beside the game is not the kit's (another DLSS version)"
    copy_if_changed(src, dst)
    if note.is_file() and (not (out / DLSS_NOTICE).is_file() or (out / DLSS_NOTICE).read_bytes() != note.read_bytes()):
        shutil.copy2(note, out / DLSS_NOTICE)   # by content: two versions' notices can share size and second
    return None


def place_dlssg(out):
    """DLSS Frame Generation's libraries and their licence texts, the kit's bin/dlssg/, beside the game. Returns how
    many files it holds, 0 when the kit has no complete set (nothing is placed then)."""
    src = KIT / 'bin' / DLSSG_DIR
    if not all((src / n).is_file() for n in DLSSG_FILES):
        return 0
    files = sorted(p for p in src.iterdir() if p.is_file())
    for p in files:
        copy_if_changed(p, out / p.name)
    return len(files)


def copy_dlls(exe, msys_bin, out):
    seen, todo, copied = set(), [exe], []
    while todo:
        for name in pe_imports(todo.pop()):
            src = msys_bin / name
            if name in seen or name in SYSTEM_DLLS_SKIP or not src.is_file():
                continue
            seen.add(name)
            copy_if_changed(src, out / src.name)
            copied.append(src.name)
            todo.append(src)
    return copied


def parse_deps(text):
    """The prerequisites of a make-style dependency file as clang -MMD writes it (the target dropped)."""
    text = text.replace('\\\r\n', ' ').replace('\\\n', ' ')
    tokens, cur, i = [], '', 0
    while i < len(text):
        c = text[i]
        if c == '\\' and i + 1 < len(text) and text[i + 1] in ' #':
            cur += text[i + 1]
            i += 2
            continue
        if c == '$' and text[i + 1:i + 2] == '$':
            cur += '$'
            i += 2
            continue
        if c.isspace():
            if cur:
                tokens.append(cur)
            cur = ''
        else:
            cur += c
        i += 1
    if cur:
        tokens.append(cur)
    for k, t in enumerate(tokens):   # the target ends with ':' (a drive letter's colon is followed by a separator)
        if t.endswith(':'):
            return tokens[k + 1:]
    return tokens


class ObjectCache:
    """Which objects are current: per object the key of its command and source, and the sha256 (with size and
    modification time, to skip hashing an untouched file) of every file its dependency list names
    (build-cache/obj/objects.json). Paths under a root are kept relative to it ({'<kit>': KIT}), so the same files
    in a kit at another place still count as the same."""

    def __init__(self, path, roots=None):
        self.path = path
        self.roots = {k: os.path.normcase(os.path.abspath(v)) for k, v in (roots or {}).items()}
        try:
            self.entries = json.loads(path.read_text(encoding='utf-8')) if path.is_file() else {}
        except (OSError, ValueError):
            self.entries = {}
        self._sha = {}

    def norm(self, p):
        a = os.path.normcase(os.path.abspath(p))
        for name, root in self.roots.items():
            if a == root or a.startswith(root + os.sep):
                return name + a[len(root):].replace(os.sep, '/')
        return a

    def real(self, p):
        for name, root in self.roots.items():
            if p.startswith(name):
                return root + p[len(name):].replace('/', os.sep)
        return p

    def file_sha(self, p):
        p = os.path.normcase(os.path.abspath(p))
        if p not in self._sha:
            try:
                self._sha[p] = sha256_file(p)
            except OSError:
                self._sha[p] = None
        return self._sha[p]

    def _same(self, p, rec):
        path = self.real(p)
        if isinstance(rec, str):   # a cache without sizes and times
            return self.file_sha(path) == rec
        digest, size, mtime = rec
        try:
            st = os.stat(path)
        except OSError:
            return False
        if st.st_size == size and st.st_mtime_ns == mtime:
            return True
        return st.st_size == size and self.file_sha(path) == digest

    def current(self, obj, key):
        e = self.entries.get(obj.name)
        return (e is not None and e.get('key') == key and obj.is_file()
                and all(self._same(p, rec) for p, rec in e.get('deps', {}).items()))

    def record(self, obj, key, dep_file, src):
        try:
            deps = parse_deps(Path(dep_file).read_text(encoding='utf-8', errors='replace'))
        except OSError:
            deps = []
        out = {}
        for p in deps or [str(src)]:
            try:
                st = os.stat(p)
                out[self.norm(p)] = [self.file_sha(p), st.st_size, st.st_mtime_ns]
            except OSError:
                out[self.norm(p)] = [None, -1, -1]
        self.entries[obj.name] = {'key': key, 'deps': out}

    def forget(self, obj):
        self.entries.pop(obj.name, None)

    def adopt(self, obj, key, old_keys, old_kits=()):
        """Records obj under key when its entry has one of old_keys, the keys an earlier version gave the same compile,
        its object is there and every file it was compiled from reads as it did. A file in an earlier kit's folder
        (old_kits, app/<version>) is read at the same place in this kit, as the object must match this kit's headers:
        that folder may be gone. 1 when taken over."""
        e = self.entries.get(obj.name)
        if not e or e.get('key') not in old_keys or not obj.is_file():
            return 0
        kit = self.roots.get('<kit>')
        kits = [os.path.normcase(os.path.abspath(k)) for k in old_kits]
        deps = {}
        for p, rec in e.get('deps', {}).items():
            path = self.real(p)
            a = os.path.normcase(os.path.abspath(path))
            for k in kits:
                if kit and a.startswith(k + os.sep):
                    path = kit + a[len(k):]
                    break
            digest = rec if isinstance(rec, str) else rec[0]
            if not digest or self.file_sha(path) != digest:
                return 0
            st = os.stat(path)
            deps[self.norm(path)] = [digest, st.st_size, st.st_mtime_ns]
        self.entries[obj.name] = {'key': key, 'deps': deps}
        return 1

    def save(self):
        tmp = self.path.with_suffix('.tmp')
        tmp.write_text(json.dumps(self.entries), encoding='utf-8')
        tmp.replace(self.path)


def compiler_ids(clang, env):
    """(id, id up to 0.6.0): what names the compiler in the object cache's keys. The id is its --version text and its
    size. The keys up to 0.6.0 held the file's modification time too, so a tool update that put the same compiler back
    made every object stale, a full compile of the game for nothing; that form is still made, to take those objects
    over (object_alts)."""
    p = subprocess.run([str(clang), '--version'], capture_output=True, text=True, env=env)
    st = os.stat(clang)
    return (sha256_bytes(f'{p.stdout}\n{st.st_size}'.encode()),
            sha256_bytes(f'{p.stdout}\n{st.st_size}\n{int(st.st_mtime)}'.encode()))


def compiler_id(clang, env):
    return compiler_ids(clang, env)[0]


def object_key(cmd, src, dst, cache, salt, sysroot=False):
    """The cache key of compiling src into dst with cmd: the compiler's id (salt), the command's flags with the cache's
    roots standing for their paths, and the source's sha256. --sysroot is left out: only fast_clang's compiler gets it,
    and its objects are the UCRT64 compiler's byte for byte, so the object of either serves (sysroot=True keeps it: the
    keys up to 0.6.0)."""
    flags = [c for c in cmd[1:] if c not in (str(src), str(dst)) and (sysroot or not c.startswith('--sysroot='))]
    for name, root in cache.roots.items():
        flags = [name + os.path.normcase(f)[len(root):].replace(os.sep, '/') if os.path.normcase(f).startswith(root)
                 else f for f in flags]
    return sha256_bytes(json.dumps([salt, [os.path.basename(cmd[0])] + flags, cache.file_sha(src)]).encode())


def legacy_key(src, dst, cache, salt, clang, clangxx, kit, lift, gen, vma):
    """The key 0.5.0 and earlier gave compiling src into dst: their whole command with its paths as they were, the
    kit's own folder (app/<version>) among them, and the compiler's id up to 0.6.0 (salt)."""
    rt = Path(kit) / 'runtime'
    cpp = src.suffix in ('.cpp', '.cxx')
    guest = dst.name.startswith(('main_', 'sdk_', 'subsdk0_'))
    cmd = [str(clangxx if cpp else clang), '-c', '-std=gnu++17' if cpp else '-std=gnu11', '-O1' if guest else '-O2',
           '-fno-strict-aliasing', '-w', '-I', str(rt), '-I', str(lift), '-I', str(gen),
           '-I', str(rt / 'third_party' / 'imgui'), '-I', str(vma), '-o', str(dst), str(src)]
    flags = [c for c in cmd if c not in (str(src), str(dst))]
    return sha256_bytes(json.dumps([salt, flags, cache.file_sha(src)]).encode())


def earlier_kits():
    """The folders of the launcher's earlier versions beside this kit's (app/<version>), by path, whether there or not:
    the keys of their objects name them, and the launcher keeps only the previous one (app/previous.txt)."""
    app = KIT.parent
    if app.name != 'app':
        return []
    kits = {p for p in app.iterdir() if p.is_dir() and p != KIT and (p / 'runtime').is_dir()}
    try:
        prev = (app / 'previous.txt').read_text(encoding='utf-8').strip()
        if prev and app / prev != KIT:
            kits.add(app / prev)
    except OSError:
        pass
    return sorted(kits)


def compile_all(srcs, cmd_for, cache, jobs, env, progress=None, salt='', stop=None, alts=None):
    """Compile each (source, object) whose object is not current in cache. cmd_for(src, dst) -> argv without the
    dependency-file options; salt identifies the compiler. Returns (compiled, reused, failures) with failures as
    (src, stderr tail). The cache's roots stand for their paths in the flags too. The largest sources start first:
    compile time grows with size, and a big file that started last would run on alone after the others finished.
    stop() true makes the compiles not yet started end at once, unrecorded (a step beside this one failed).
    alts(src, dst) -> (keys, kits): the keys earlier versions gave the same compile, and the folders of their kits; an
    object recorded under one of them is taken over (ObjectCache.adopt) instead of compiled again."""
    todo, keys = [], {}
    adopted = 0
    for src, dst in srcs:
        keys[dst] = object_key(cmd_for(src, dst), src, dst, cache, salt)
        if cache.current(dst, keys[dst]):
            continue
        if alts and cache.adopt(dst, keys[dst], *alts(src, dst)):
            adopted += 1
            continue
        todo.append((src, dst))
    reused = len(srcs) - len(todo)
    if adopted:
        say(f'  {adopted} objects an earlier version compiled are kept: the same compile of the same sources')
    todo.sort(key=lambda job: job[0].stat().st_size, reverse=True)

    def cc(job):
        src, dst = job
        dep = dst.with_suffix('.d')
        if stop and stop():
            return src, dst, dep, None, ''
        cmd = cmd_for(src, dst)
        cmd[1:1] = ['-MMD', '-MF', str(dep)]
        p = subprocess.run(cmd, capture_output=True, text=True, env=env, errors='replace')
        return src, dst, dep, p.returncode, p.stderr[-1500:]

    done, bad = reused, []
    if progress:
        progress(done, len(srcs))
    with concurrent.futures.ThreadPoolExecutor(max(1, jobs)) as ex:
        for src, dst, dep, rc, err in ex.map(cc, todo):
            done += 1
            if rc is None:   # not started (stop)
                continue
            if rc:
                bad.append((src, err))
                cache.forget(dst)
            else:
                cache.record(dst, keys[dst], dep, src)
            if progress:
                progress(done, len(srcs))
    cache.save()
    return len(todo), reused, bad


def runtime_sources():
    """(source, object name) of every runtime file this OS's build compiles: the Win32 files on Windows, the SDL
    window's (*_sdl.*) and platform/linux.c elsewhere (tools/build_game.py's foreign_os)."""
    rt = KIT / 'runtime'
    out = []
    for pat in RUNTIME_PATTERNS:
        for p in sorted(rt.glob(pat)):
            if (p.stem.endswith('_sdl') or p.name == 'linux.c') if WIN else (p.stem.endswith('_win') or p.name == 'win32.c'):
                continue
            out.append((p, 'rt_' + p.relative_to(rt).as_posix().replace('/', '_').rsplit('.', 1)[0] + '.o'))
    return out


def per_install_names(generated=GENERATED_HEADERS):
    """The basenames of the generated headers and of every runtime header that includes one, at any depth."""
    rt = KIT / 'runtime'
    headers = {}
    for p in list(rt.rglob('*.h')) + list(rt.rglob('*.inc')):
        headers[p.name] = {os.path.basename(i) for i in INCLUDE.findall(p.read_text(encoding='utf-8', errors='replace'))}
    tainted = set(generated)
    grew = True
    while grew:
        grew = False
        for name, incs in headers.items():
            if name not in tainted and incs & tainted:
                tainted.add(name)
                grew = True
    return tainted


def per_install(src, tainted):
    incs = {os.path.basename(i) for i in INCLUDE.findall(Path(src).read_text(encoding='utf-8', errors='replace'))}
    return bool(incs & tainted)


def clang_version(clang, env):
    p = subprocess.run([str(clang), '--version'], capture_output=True, text=True, env=env)
    return (p.stdout.splitlines() or [''])[0].strip()


def runtime_digest():
    return tree_digest(KIT / 'runtime', ('**/*',))


# The x86-64 levels the lifted code is built and tested for (docs/building.md, "Checks"), as the macros clang defines
# for a CPU that has them.
CPU_V2 = {'__SSE3__', '__SSSE3__', '__SSE4_1__', '__SSE4_2__', '__POPCNT__', '__LAHF_SAHF__',
          '__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16'}
CPU_V3 = CPU_V2 | {'__AVX__', '__AVX2__', '__BMI__', '__BMI2__', '__F16C__', '__FMA__', '__LZCNT__', '__MOVBE__',
                   '__XSAVE__'}


def cpu_level(clang, env):
    """This PC's x86-64 level for the lifted code: 3 (AVX2, FMA, BMI2), 2 (SSE4.2, POPCNT) or 0 when clang cannot
    tell. The game runs on the PC that builds it, so it takes this CPU's level, as tools/build_game.py --cpu does by
    hand. Without one the lifted code would be generic x86-64: every fused multiply-add of the game a call to
    runtime/fma.c's software fma, and a level the differential tests do not cover."""
    p = subprocess.run([str(clang), '-march=native', '-E', '-dM', '-x', 'c', os.devnull], capture_output=True,
                       text=True, env=env, errors='replace')
    have = set(re.findall(r'^#define (__\w+) ', p.stdout, re.M)) if p.returncode == 0 else set()
    return 3 if CPU_V3 <= have else 2 if CPU_V2 <= have else 0


def clang_ident(clang, env):
    """(version, target) a clang reports, or None when it does not run."""
    try:
        p = subprocess.run([str(clang), '--version'], capture_output=True, text=True, env=env, errors='replace')
    except OSError:
        return None
    v, t = re.search(r'clang version (\S+)', p.stdout), re.search(r'^Target: (\S+)', p.stdout, re.M)
    return (v.group(1), t.group(1)) if p.returncode == 0 and v and t else None


def fast_clang(msys, clang, env):
    """MSYS2's CLANG64 clang beside the UCRT64 one (msys64/clang64, which the launcher installs): the same LLVM release
    built with profile-guided optimisation, about a fifth faster on the lifted code. With UCRT64's headers
    (--sysroot) it writes the same objects as UCRT64's clang, byte for byte but the COFF timestamp; it is used only
    when it reports the same version and target. None otherwise."""
    if not WIN:
        return None
    fast = Path(msys).parent / 'clang64' / 'bin' / 'clang.exe'
    if not fast.is_file():
        return None
    mine = clang_ident(clang, env)
    return fast if mine and clang_ident(fast, env) == mine else None


def debug_flags(guest, codeview):
    """--debug's compile flags: line tables for the lifted code (full info for millions of generated lines makes
    the compile and the PDB enormous for little gain), full info for the runtime; CodeView for a PDB when lld links
    on Windows, else DWARF."""
    return (['-gline-tables-only' if guest else '-g', '-fno-omit-frame-pointer']
            + (['-gcodeview'] if codeview else []))


def runtime_cmd(clang, clangxx, inc, level=0, fast=None, msys=None, debug=False, codeview=False):
    """fast: fast_clang's compiler for the lifted code, with msys as its sysroot. debug: --debug's flags (the
    runtime at -O0 with debug info, the lifted code at -O1 with line tables); codeview: CodeView rather than DWARF."""
    def cmd_for(src, dst):
        cpp = src.suffix in ('.cpp', '.cxx')
        # the lifted code at -O1 (its size makes -O2 slow to build), the runtime at -O2; neither may assume strict
        # aliasing: the lifted code reaches guest memory through casts of one address to several types
        # (tools/build_game.py's flags). The lifted code is built for this PC's level (cpu_level); the runtime
        # stays generic, so the prebuilt one serves every PC (its vectors are 16 bytes, passed alike at every level).
        guest = dst.name.startswith(('main_', 'sdk_', 'subsdk0_'))
        march = [f'-march=x86-64-v{level}'] if guest and level else []
        cc = [str(fast), f'--sysroot={msys}'] if guest and fast else [str(clangxx if cpp else clang)]
        # -fdeclspec: third_party/ffx_api/ffx_api.h declares its entry points __declspec(dllexport) unconditionally
        declspec = ['-fdeclspec'] if cpp and not WIN else []
        opt = '-O1' if guest else ('-O0' if debug else '-O2')
        dbg = debug_flags(guest, codeview) if debug else []
        return [*cc, '-c', '-std=gnu++17' if cpp else '-std=gnu11', opt, *dbg,
                *march, '-fno-strict-aliasing', '-w', *declspec, *inc, '-o', str(dst), str(src)]
    return cmd_for


def make_runtime_lib(dest, msys, jobs, obj=None):
    """--make-runtime-lib: compile every runtime source that does not include a per-install header into
    <dest>/libnx2navirt.a, with runtime-lib.json naming the runtime/ tree and the compiler it was built from. The
    objects stay in obj (default <dest>/obj), which a release keeps out of what it ships."""
    dest = Path(dest).resolve()
    dest.mkdir(parents=True, exist_ok=True)
    obj = Path(obj).resolve() if obj else dest / 'obj'
    obj.mkdir(parents=True, exist_ok=True)
    clang, clangxx = msys / 'bin' / 'clang.exe', msys / 'bin' / 'clang++.exe'
    env = dict(os.environ, PATH=str(msys / 'bin') + os.pathsep + os.environ.get('PATH', ''))
    rt = KIT / 'runtime'
    tainted = per_install_names()
    srcs = [(p, obj / name) for p, name in runtime_sources() if not per_install(p, tainted)]
    inc = ['-I', str(rt), '-I', str(rt / 'third_party' / 'imgui'), '-I', str(msys / 'include' / 'vma')]
    cache = ObjectCache(obj / 'objects.json', {'<kit>': KIT})
    t0 = time.time()
    compiled, reused, bad = compile_all(srcs, runtime_cmd(clang, clangxx, inc), cache, jobs, env,
                                        lambda d, t: (d % 20 == 0 or d == t) and say(f'  {d} of {t}'),
                                        salt=compiler_id(clang, env))
    if bad:
        for src, err in bad[:3]:
            print(f'--- {src}\n{err}', file=sys.stderr)
        fail(f'{len(bad)} runtime files did not compile')
    lib = dest / RUNTIME_LIB
    lib.unlink(missing_ok=True)
    rsp = dest / 'ar.rsp'
    rsp.write_text('\n'.join('"%s"' % o.as_posix() for _, o in srcs), encoding='utf-8')
    ar = os.environ.get('NX2NAVI_AR') or str(msys / 'bin' / 'ar.exe')
    if subprocess.run([ar, 'rcs', str(lib), '@' + str(rsp)], env=env).returncode:
        fail('the runtime library could not be archived')
    meta = {'runtime': runtime_digest(), 'compiler': clang_version(clang, env),
            'objects': sorted(o.name for _, o in srcs)}
    (dest / 'runtime-lib.json').write_text(json.dumps(meta, indent=1), encoding='utf-8')
    rsp.unlink(missing_ok=True)
    say(f'{lib}: {len(srcs)} runtime files ({compiled} compiled, {reused} kept), {lib.stat().st_size / 1e6:.1f} MB, '
        f'{time.time() - t0:.0f} s')


def prebuilt_runtime(clang, env):
    """(library, object names it holds) when the kit's lib/ matches this runtime/ and this compiler, else None."""
    lib, meta = KIT / 'lib' / RUNTIME_LIB, KIT / 'lib' / 'runtime-lib.json'
    try:
        m = json.loads(meta.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return None
    if not lib.is_file() or m.get('runtime') != runtime_digest() or m.get('compiler') != clang_version(clang, env):
        say('  the prebuilt runtime does not match this kit or compiler: compiling runtime/')
        return None
    return lib, set(m.get('objects', []))


def romfs_index(out, gen):
    if subprocess.run(py_cmd(KIT / 'tools' / 'romfs_index.py', '--romfs', out / 'romfs', '--out', gen)).returncode:
        fail('the RomFS index could not be written')


def windres_command(msys, rco):
    """MSYS2's windres on nx2navi.rc, run with runtime/ as the working directory (compile_resources says why)."""
    pp = ('-E', '-xc', '-DRC_INVOKED')   # windres's own preprocessor flags, which --preprocessor replaces
    return [str(msys / 'bin' / 'windres.exe'), '--use-temp-file', f'--preprocessor={msys / "bin" / "gcc.exe"}',
            *(f'--preprocessor-arg={a}' for a in pp), '-O', 'coff', '-i', 'nx2navi.rc', '-o', str(rco)]


RC_LINE = re.compile(r'(\d+)\s+(24|ICON)\s+"([^"]+)"')


def resource_file(rt):
    """runtime/nx2navi.rc as a .res file (the format Windows' resource compiler writes): the manifest as resource 24,
    the icon's images as RT_ICON 1..n and their directory as RT_GROUP_ICON, US English as windres gives them. lld
    takes a .res among its inputs and builds the exe's resource section from it.

    Python writes it, so no tool opens a path by the system's ANSI code page: windres does (binutils), and a build
    folder or a user folder whose name holds a character outside that code page (C:\\Users\\Víctor on a Cyrillic or a
    Japanese Windows, D:\\Games\\Zelda：Ocarina) failed with "Invalid argument" or "Cannot create temporary file"."""
    def resource(typ, name, data, flags):
        head = struct.pack('<HHHHIHHII', 0xffff, typ, 0xffff, name, 0, flags, 0x0409, 0, 0)
        return struct.pack('<II', len(data), 8 + len(head)) + head + data + b'\0' * (-len(data) % 4)

    out = bytearray(struct.pack('<II', 0, 32) + struct.pack('<HHHHIHHII', 0xffff, 0, 0xffff, 0, 0, 0, 0, 0, 0))
    for line in (rt / 'nx2navi.rc').read_text(encoding='utf-8').splitlines():
        m = RC_LINE.fullmatch(line.strip())
        if not m:
            continue
        name, kind, data = int(m.group(1)), m.group(2), (rt / m.group(3)).read_bytes()
        if kind == '24':
            out += resource(24, name, data, 0x1030)
            continue
        reserved, ico_type, n = struct.unpack_from('<HHH', data, 0)
        if reserved or ico_type != 1 or not n:
            fail(f'runtime/{m.group(3)} is not an icon file')
        group = struct.pack('<HHH', 0, 1, n)
        for i in range(n):
            w, h, colors, res0, planes, bits, size, off = struct.unpack_from('<BBBBHHII', data, 6 + 16 * i)
            out += resource(3, i + 1, data[off:off + size], 0x1010)
            group += struct.pack('<BBBBHHIH', w, h, colors, res0, planes, bits, size, i + 1)
        out += resource(14, name, group, 0x1030)
    return bytes(out)


def compile_resources(msys, rt, obj, env, lld=False):
    """With lld (lld=True) obj/nx2navi.res written by resource_file, which lld links in itself; else windres below.

    runtime/nx2navi.rc into obj/nx2navi_rc.o: the manifest of the game's exe (per-monitor DPI awareness, so the
    picture keeps the monitor's real size rather than one Windows stretches) and its icon.

    windres preprocesses the .rc with gcc through one command line it writes itself, quoting each path that holds a
    space. By default it hands that line to cmd.exe /c, which drops the first and the last double quote of a line
    holding more than two: gcc's quoted path loses its opening quote, and under a user folder with a space in its
    name (C:\\Users\\Ali Sayed) cmd.exe stops at that space ("'C:\\Users\\Ali' is not recognized").

    With --use-temp-file windres starts gcc itself instead, no shell involved: it splits the line into arguments and
    passes each one to gcc intact. Its splitter reads a quoted first word right, but it cuts a quoted -I path at its
    spaces and reads past the end of the line after a quoted last word, so the line holds no path but gcc's, its
    first word: the .rc goes by its bare name from runtime/ as the working directory, where windres also finds the
    manifest and the icon, and there is no -I. windres's temporary files go to a folder of the build cache, deleted
    afterwards (it leaves two of them behind on every run).

    The object is written under another name and replaces the old one only when its bytes differ, so an unchanged
    resource keeps its time and does not make the link key change (a rebuild with nothing new does not link again)."""
    if lld:
        res = obj / 'nx2navi.res'
        data = resource_file(rt)
        if not res.is_file() or res.read_bytes() != data:   # same bytes: keep the time, so the link key holds
            res.write_bytes(data)
        return res
    rco, new = obj / 'nx2navi_rc.o', obj / 'nx2navi_rc.o.new'
    tmp = obj / 'windres-tmp'
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir()
    try:
        r = subprocess.run(windres_command(msys, new), cwd=rt, env=dict(env, TMP=str(tmp), TEMP=str(tmp)),
                           capture_output=True, text=True, errors='replace')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if r.returncode:
        new.unlink(missing_ok=True)
        fail(f'windres failed on runtime/nx2navi.rc: {r.stderr.strip()[:400]}')
    if rco.is_file() and rco.read_bytes() == new.read_bytes():
        new.unlink()
    else:
        os.replace(new, rco)
    return rco


def toolchain(msys):
    """(clang, clang++, environment, the folder holding vk_mem_alloc.h): MSYS2 UCRT64's on Windows, the ones on PATH
    elsewhere, with tools/build_game.py's VMA lookup (the system's header, else the pinned one fetched into
    toolchain/vma)."""
    if WIN:
        return (msys / 'bin' / 'clang.exe', msys / 'bin' / 'clang++.exe',
                dict(os.environ, PATH=str(msys / 'bin') + os.pathsep + os.environ.get('PATH', '')), msys / 'include' / 'vma')
    from build_game import vma_include
    return Path(shutil.which('clang')), Path(shutil.which('clang++')), dict(os.environ), vma_include()


def xlate_dir():
    """The folder of the shader translator: the kit's bin/, else a source checkout's build/xlate/
    (tools/build_xlate.py); None when neither has it. The compile and shaders steps and the shaders step's key all
    look it up here, so they agree on which translator a build uses."""
    for d in (KIT / 'bin', KIT / 'build' / 'xlate'):
        if (d / XLATE_NAME).is_file():
            return d
    return None


def xlate_source():
    """xlate_dir(), or stop the build when there is no translator."""
    d = xlate_dir()
    if d is None:
        fail(f'{XLATE_NAME} is missing: build it with tools/build_xlate.py')
    return d


# The x86-64 level the lifted code was compiled for, beside the objects (lifted_level).
LIFTED_BUILD = 'lifted-build.json'


def lifted_level(obj, now, update):
    """The x86-64 level to compile the lifted code for. A build from the game files takes this processor's (now). An
    update keeps the level the objects were compiled for, so that it compiles only what changed: the one recorded
    (LIFTED_BUILD), else 0 (generic x86-64) when the cache is one of 0.5.0 or earlier, which compiled for no level and
    whose entries name their files by sha256 alone. Never above this processor's: a game folder moved to an older PC is
    compiled again for it."""
    if not update:
        return now
    try:
        rec = json.loads((obj / LIFTED_BUILD).read_text(encoding='utf-8'))
        return min(int(rec['level']), now)
    except (OSError, ValueError, KeyError, TypeError):
        pass
    try:
        entries = json.loads((obj / 'objects.json').read_text(encoding='utf-8'))
        old = any(isinstance(rec, str) for e in entries.values() for rec in e.get('deps', {}).values())
    except (OSError, ValueError, AttributeError):
        old = False
    return 0 if old else now


def step_compile(out, cache_dir, msys, jobs, romfs_job=None, update=False, debug=False):
    """romfs_job: the RomFS step still running beside this one (a first build). Everything that does not include
    romfs_config.h compiles meanwhile; the RomFS index and the few files that include it follow once it is done.
    update: a rebuild after a kit update (lifted_level). debug: --debug, into build-cache/obj-debug and
    <out>/ocarina-debug.exe with its symbols (see the docstring)."""
    lift, gen, obj = cache_dir / 'lift', cache_dir / 'gen', cache_dir / ('obj-debug' if debug else 'obj')
    gen.mkdir(parents=True, exist_ok=True)
    obj.mkdir(parents=True, exist_ok=True)
    rt = KIT / 'runtime'
    clang, clangxx, env, vma = toolchain(msys)
    lld = (msys / 'bin' / 'ld.lld.exe').is_file() if WIN else bool(shutil.which('ld.lld'))
    codeview = debug and WIN and lld   # a PDB needs lld; GNU ld keeps DWARF in the exe
    if not romfs_job:
        romfs_index(out, gen)
    srcs = [(p, obj / f'{mod}_{p.stem}.o') for mod in ('main', 'sdk', 'subsdk0') for p in sorted((lift / mod).glob('*.c'))]
    prebuilt = None if debug else prebuilt_runtime(clang, env)   # the prebuilt runtime has no debug info
    if debug:
        say('  debug build: the runtime at -O0 with full debug info, the lifted code at -O1 with line tables; '
            + ('CodeView symbols into a PDB' if codeview else 'DWARF symbols in the executable'))
    runtime = [(p, obj / name) for p, name in runtime_sources()]
    if prebuilt:
        runtime = [(p, o) for p, o in runtime if o.name not in prebuilt[1]]
        say(f'  the runtime comes prebuilt; {len(runtime)} runtime files are compiled here')
    srcs += runtime
    srcs.append((modules_c(lift, out, gen), obj / 'modules.o'))
    if WIN:
        srcs.append((gen / 'shim.c', obj / 'shim.o'))
    inc = ['-I', str(rt), '-I', str(lift), '-I', str(gen), '-I', str(rt / 'third_party' / 'imgui'), '-I', str(vma)]
    cid, cid_old = compiler_ids(clang, env)   # fast_clang's compiler writes the same objects: not part of the key
    now = cpu_level(clang, env)
    level = lifted_level(obj, now, update)
    if level == now:
        say(f'  the lifted code is built for x86-64-v{level}, this processor\'s level' if level else
            '  the lifted code is built for generic x86-64: the processor\'s level could not be read')
    else:
        say(f'  the lifted code is built for {f"x86-64-v{level}" if level else "generic x86-64"}, as the objects kept '
            f'from before this update were (Rebuild the Game compiles it for this processor\'s level)')
    fast = fast_clang(msys, clang, env)
    if fast:
        say(f'  the lifted code compiles with {fast}')
    cmd_for = runtime_cmd(clang, clangxx, inc, level, fast, msys, debug, codeview)
    # The keys earlier versions gave the same compiles (compile_all alts): 0.6.0's, with the compiler's file time and,
    # with the fast compiler, its id and --sysroot; 0.5.0's and earlier's, with their kit's folder in the paths.
    plain = runtime_cmd(clang, clangxx, inc, level, None, msys, debug, codeview)
    fast_old = (cid_old + compiler_ids(fast, env)[1]) if fast else None
    kits = earlier_kits()

    def alts(src, dst):
        keys = [object_key(plain(src, dst), src, dst, cache, cid_old, sysroot=True)]
        if fast:
            keys.append(object_key(cmd_for(src, dst), src, dst, cache, fast_old, sysroot=True))
        keys += [legacy_key(src, dst, cache, cid_old, clang, clangxx, k, lift, gen, vma) for k in kits]
        return keys, kits

    t0 = time.time()
    last = [0.0]

    def progress(done, total):
        now = time.time()
        if done == total or now - last[0] >= 0.25:
            last[0] = now
            emit('progress', step='compile', done=done, total=total, unit='files')
        if done % 50 == 0 or done == total:
            say(f'  compiled {done} of {total}, {now - t0:.0f} s')

    say(f'  {len(srcs)} files on {jobs} threads')
    cache = ObjectCache(obj / 'objects.json', {'<kit>': KIT})
    late = []
    if romfs_job:   # the lifted chunks include only cpu.h and simd.h: only the runtime and generated files are read
        tainted = per_install_names(('romfs_config.h',))
        late = [(s, o) for s, o in runtime + srcs[-(2 if WIN else 1):] if per_install(s, tainted)]
        say(f'  {len(late)} files wait for the RomFS index: {", ".join(s.name for s, _ in late)}')
    early = [j for j in srcs if j not in late]
    compiled, reused, bad = compile_all(early, cmd_for, cache, jobs, env, lambda d, t: progress(d, len(srcs)),
                                        salt=cid, stop=romfs_job.failed if romfs_job else None, alts=alts)
    if romfs_job:
        if romfs_job.thread.is_alive():
            say('  waiting for the RomFS image')
        romfs_job.join()   # raises the RomFS step's failure; a failed compile still keeps the finished image
        if not bad:
            romfs_index(out, gen)
            c2, r2, bad = compile_all(late, cmd_for, cache, jobs, env,
                                      lambda d, t: progress(len(early) + d, len(srcs)), salt=cid, alts=alts)
            compiled, reused = compiled + c2, reused + r2
    say(f'  {compiled} compiled, {reused} unchanged since the last build')
    if bad:
        for src, err in bad[:3]:
            print(f'--- {src}\n{err}', file=sys.stderr)
        fail(f'{len(bad)} files did not compile')
    (obj / LIFTED_BUILD).write_text(json.dumps({'level': level}), encoding='utf-8')   # lifted_level reads it next update

    objs = [o for _, o in srcs] + ([compile_resources(msys, rt, obj, env, lld=lld)] if WIN else [])
    exe = out / EXE_NAME
    tag = '-debug' if debug else ''
    rsp = cache_dir / f'link{tag}.rsp'
    rsp.write_text('\n'.join('"%s"' % o.as_posix() for o in objs), encoding='utf-8')
    # lld links several times faster than GNU ld; the stack matches tools/build_game.py's build
    # 16 MiB for the main thread with either linker (GNU ld takes the same flag): the default 2 MiB is too little
    if WIN:
        ld = (['-fuse-ld=lld'] if lld else []) + ['-Wl,--stack,16777216']
    else:   # tools/build_game.py's Linux flags; the main thread's stack is the process's (ulimit -s), not the linker's
        ld = (['-fuse-ld=lld'] if lld else []) + ['-no-pie', '-Wl,-z,noexecstack', '-Wl,-rpath,$ORIGIN']
    # the link map feeds tools/prof_report.py (symbols for an NX2NAVI_PROFILE sampling profile); both linkers write one
    ld.append('-Wl,--Map=' + (cache_dir / f'ocarina{tag}.map').as_posix())
    pdb = out / 'ocarina-debug.pdb' if codeview else None
    if debug:   # keep the symbols: -g at the link, and with CodeView the PDB beside the exe (its path is in the exe)
        ld.append('-g')
        if pdb:
            ld.append('-Wl,--pdb=' + str(pdb))
    lib = ['-Wl,--whole-archive', str(prebuilt[0]), '-Wl,--no-whole-archive'] if prebuilt else []
    stamp = [[o.name, o.stat().st_size, o.stat().st_mtime_ns] for o in objs]
    if prebuilt:
        stamp.append([prebuilt[0].name, prebuilt[0].stat().st_size, sha256_file(prebuilt[0])])
    link_key = sha256_bytes(json.dumps([LINK_LIBS, ld, cid] + stamp).encode())
    state_link = cache_dir / f'link{tag}.key'
    if (exe.is_file() and state_link.is_file() and state_link.read_text(encoding='utf-8') == link_key
            and (pdb is None or pdb.is_file())):
        say(f'  {EXE_NAME} is current, not linked again')
    else:
        emit('phase', step='compile', phase='link')
        say('  linking')
        tmp = out / (EXE_NAME + '.new')
        lib_dir = ['-L', str(msys / 'lib')] if WIN else []
        p = subprocess.run([str(clangxx), *ld, '-o', str(tmp), '@' + str(rsp), *lib, *lib_dir, *LINK_LIBS],
                           capture_output=True, text=True, env=env, errors='replace')
        if p.returncode:
            print(p.stderr[-4000:], file=sys.stderr)
            tmp.unlink(missing_ok=True)
            fail('the link failed' + (f' (if {pdb.name} is open in a debugger, close it and try again)' if pdb else ''))
        try:
            os.replace(tmp, exe)
        except PermissionError:
            tmp.unlink(missing_ok=True)
            fail(f'{exe} is in use: close the game and try again')
        state_link.write_text(link_key, encoding='utf-8')
    copy_if_changed(gen / 'romfs-index.bin', out / 'romfs-index.bin')
    xlate = xlate_source()
    note_cache_translator(out)   # before the new translator replaces the one the shader cache was made with
    for name in (XLATE_NAME, 'nx2navi_xlate.id'):
        copy_if_changed(xlate / name, out / name)
    if (KIT / 'bin' / 'amd_fidelityfx_vk.dll').is_file():   # FSR 3.1 (runtime/vk_ffx.cpp)
        copy_if_changed(KIT / 'bin' / 'amd_fidelityfx_vk.dll', out / 'amd_fidelityfx_vk.dll')
    kept = place_dlss(out) if WIN else None   # NVIDIA DLSS (runtime/vk_dlaa.c): its library is a Windows DLL
    if kept:
        say(f'  {kept}')
    if WIN and place_dlssg(out):   # DLSS Frame Generation (runtime/vk_dlssg.c): Windows DLLs too
        say('  DLSS Frame Generation: nx2navi_dlssg.dll, Streamline and nvngx_dlssg.dll beside the game')
    beside = copy_dlls(exe, msys / 'bin', out) if WIN else []
    say(f'  {EXE_NAME} {exe.stat().st_size / 1e6:.0f} MB' + (f'; beside it: {", ".join(beside)}' if beside else ''))
    if pdb and pdb.is_file():
        say(f'  symbols: {pdb} {pdb.stat().st_size / 1e6:.0f} MB')
    (out / 'profile').mkdir(exist_ok=True)


# ---------------------------------------------------------------- main

SHADER_PROGRESS = re.compile(r'^prepare-shaders: progress (translate|build) (\d+) (\d+)\s*$')
SHADER_STALL_S = 180   # no count moved for this long: the launcher says so (the step still runs)
SHADER_BEAT_S = 10     # a progress event at least this often while the step runs


def shader_progress(phase, done, total, quiet_s, elapsed_s):
    """The shaders step's progress event fields: the translation fills the first half of the bar, the build into the
    driver's cache the second (nx2navi.exe prints both counts, runtime/gfx_shader.c prepare_files and
    runtime/vk_pipeline.c lb_warm_prepared); before either count, reading the game's shaders. quiet_s: seconds since a
    count last moved, said once it passes SHADER_STALL_S, so a slow step never looks frozen without a word. The
    detail ends without a full stop: the launcher adds one (launcher/ui/app.py)."""
    clock = f'{int(elapsed_s) // 60}:{int(elapsed_s) % 60:02d}'
    if phase is None:
        fraction, detail = 0.0, f'Reading the game\'s shaders ({clock} so far)'
    elif phase == 'translate':
        fraction, detail = 0.5 * done / max(total, 1), f'Translated {done:,} of {total:,} shaders'
    else:
        fraction, detail = 0.5 + 0.5 * done / max(total, 1), f'Built {done:,} of {total:,} shaders for your graphics card'
    if quiet_s >= SHADER_STALL_S:
        detail += (f'. No change for {int(quiet_s) // 60} min, but it is still running: one large shader can take '
                   'a while')
    return {'step': 'shaders', 'fraction': round(fraction, 4), 'detail': detail}


# In the game's shader-cache/: the translator identities its translations are of, newest first (runtime/gfx_shader.c,
# "Carry"): '#' comment lines, then one identity a line in 64 hex digits, the content of nx2navi_xlate.id.
SHADER_IDS = 'identities.txt'
HEX64 = re.compile(r'[0-9a-fA-F]{64}')


def cache_identities(out):
    """The translator identities the game's shader cache lists (SHADER_IDS), lower case; [] when it has none."""
    try:
        text = (out / 'shader-cache' / SHADER_IDS).read_text(encoding='ascii', errors='replace')
    except OSError:
        return []
    return [line.strip().lower() for line in text.splitlines() if HEX64.fullmatch(line.strip())]


def note_cache_translator(out):
    """Before a new translator goes beside the game: a shader cache that has no SHADER_IDS yet (made before the game
    carried translations over) gets one naming the translator that made it, the one still beside the game. The game
    then carries those translations over at this update too (runtime/gfx_shader.c ids_load), and the update need not
    prepare them all again (shader_carry). Nothing when there is no cache, the file is there, or the old id is no
    identity."""
    cache, ids = out / 'shader-cache', out / 'shader-cache' / SHADER_IDS
    if not cache.is_dir() or ids.exists():
        return
    try:
        old = (out / 'nx2navi_xlate.id').read_text(encoding='ascii', errors='replace').strip()[:64]
    except OSError:
        return
    if not HEX64.fullmatch(old):
        return
    tmp = ids.with_name(SHADER_IDS + '.tmp')
    tmp.write_text('# Translator identities this shader cache holds translations of, newest first\n' + old.lower() + '\n',
                   encoding='ascii', newline='\n')
    os.replace(tmp, ids)


def shader_carry(out):
    """Whether the game carries the shaders prepared before an update over (runtime/gfx_shader.c, NX2NAVI_SHADER_CARRY,
    on unless the environment or nx2navi.ini sets it to 0) and there are some: shader-prepared.bin is written by a
    finished preparation, and the cache must name the translator its translations are of (SHADER_IDS,
    note_cache_translator), else the game has nothing to carry them over by."""
    if os.environ.get('NX2NAVI_SHADER_CARRY', '').strip() == '0':
        return False
    try:
        for line in (out / 'nx2navi.ini').read_text(encoding='utf-8-sig', errors='replace').splitlines():
            k, eq, v = line.strip().partition('=')
            if eq and k.strip().upper().removeprefix('NX2NAVI_') == 'SHADER_CARRY' and v.strip().strip('"') == '0':
                return False
    except OSError:
        pass
    return (out / 'shader-prepared.bin').is_file() and bool(cache_identities(out))


def step_shaders(out, update=False):
    """Every shader of the game files into the game's shader cache and the driver's own cache, before the first start.
    Its output is read as it comes: the counts nx2navi.exe prints become progress events, the other lines go on to
    the log, and while nothing moves an event every few seconds keeps saying the step is running and for how long.
    update: after a kit update the shaders prepared before stay in use and the game translates them again in the
    background (shader_carry), so the update does not wait the minutes a full preparation takes."""
    # The game translates with the copy beside it. The compile step refreshes that copy, but a kit that changes only
    # the translator runs this step again alone (step_key), which would then prepare with the old one. Only a kit that
    # has one: the compile step, which always runs first, stops a build without it.
    d = xlate_dir()
    if d is not None:
        note_cache_translator(out)
        for name in (XLATE_NAME, 'nx2navi_xlate.id'):
            copy_if_changed(d / name, out / name)
    if update and shader_carry(out):
        say('  the shaders prepared before this update stay in use; the game translates them again in the background')
        return
    cmd = [sys.executable, '-u', str(KIT / 'tools' / 'prepare_shaders.py'), str(out), '--romfs', str(out / 'romfs'),
           '--exefs', str(out / 'exefs'), '--no-list', '--exe', EXE_NAME]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                            errors='replace', bufsize=1)
    t0 = time.monotonic()
    state = {'phase': None, 'done': 0, 'total': 0, 'moved': t0}
    lock = threading.Lock()
    stop = threading.Event()

    def send():
        now = time.monotonic()
        emit('progress', **shader_progress(state['phase'], state['done'], state['total'], now - state['moved'],
                                           now - t0))

    def heartbeat():
        while not stop.wait(SHADER_BEAT_S):
            with lock:
                send()

    beat = threading.Thread(target=heartbeat, daemon=True)
    beat.start()
    last_log = 0.0
    try:
        for line in proc.stdout:
            m = SHADER_PROGRESS.match(line)
            if not m:
                say(line.rstrip('\n'))
                continue
            phase, done, total = m.group(1), int(m.group(2)), int(m.group(3))
            with lock:
                if (phase, done) != (state['phase'], state['done']):
                    state['moved'] = time.monotonic()
                state.update(phase=phase, done=done, total=total)
                send()
            if time.monotonic() - last_log >= 30 or done == total:   # the log gets a count now and then, not each one
                last_log = time.monotonic()
                say(f'  shaders: {"translated" if phase == "translate" else "built"} {done} of {total}, '
                    f'{time.monotonic() - t0:.0f} s')
    finally:
        rc = proc.wait()
        proc.stdout.close()
        stop.set()
        beat.join()
    if rc:
        fail('preparing the shaders failed (the lines above say why); the game still runs, compiling shaders as it '
             'meets them. Run the build again to retry this step')
    for seed in ('seed-programs.bin', 'seed-answers.bin'):   # translated now: the shader cache holds them
        (out / seed).unlink(missing_ok=True)


def prompt_art_dir():
    """The kit's button art (<kit>/prompt-art, tools/prompt_art.py), or None when the kit has none."""
    art = KIT / prompt_art.FOLDER
    return art if art.is_dir() else None


def bind_lines(out):
    """The bind_ lines of the game folder's nx2navi.ini, comments cut off: the keys the keyboard and mouse prompts
    show (tools/pad_prompts.py pc_keys reads the same lines)."""
    try:
        text = (out / 'nx2navi.ini').read_text(encoding='utf-8', errors='replace')
    except OSError:
        return []
    lines = (ln.split('#')[0].split(';')[0].strip() for ln in text.splitlines())
    return [ln for ln in lines if ln.lower().startswith('bind_')]


def step_prompts(out):
    """The button prompts drawn from the kit's art into the game folder: tools/pad_prompts.py writes one overlay
    folder per style and layout (prompt_art.OVERLAYS), which runtime/romfs.c loads in place of the game's button font
    and prompt textures when the Controls menu's Button prompts is Xbox, PlayStation or Keyboard and mouse. Its lines
    go on to the log, each folder written is a progress event."""
    total = len(prompt_art.OVERLAYS)
    proc = subprocess.Popen(py_cmd(KIT / 'tools' / 'pad_prompts.py', '--art', prompt_art_dir(), '--game', out),
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                            errors='replace', bufsize=1)
    done = 0
    emit('progress', step='prompts', fraction=0.0, detail=f'Drawing {total} sets of button prompts')
    try:
        for line in proc.stdout:
            line = line.rstrip('\n')
            if line.startswith('{'):   # its closing report, one long JSON line
                continue
            say(f'  {line}')
            if PROMPTS_WRITTEN.match(line):
                done += 1
                emit('progress', step='prompts', fraction=round(done / total, 4),
                     detail=f'Drew {done} of {total} sets of button prompts')
    finally:
        rc = proc.wait()
        proc.stdout.close()
    if rc:
        fail('drawing the button prompts failed (the lines above say why); the game still runs. Run the build again '
             'to retry this step')


def keep_awake():
    """Hold off sleep for this process's life (Windows): the build runs for many minutes with no input, and a PC that
    sleeps in the middle stops the compiles. The hold ends with the process."""
    if os.name != 'nt':
        return
    try:
        import ctypes
        ES_CONTINUOUS, ES_SYSTEM_REQUIRED = 0x80000000, 0x00000001
        ctypes.windll.kernel32.SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)
    except (OSError, AttributeError):
        pass


def load_state(path):
    try:
        state = json.loads(path.read_text(encoding='utf-8')) if path.is_file() else {}
    except (OSError, ValueError):
        state = {}
    if state.get('version') == 1:   # the first kit: a list of finished steps, all tied to one kit version
        done = state.get('done', [])
        state = {'version': STATE_VERSION, 'input': state.get('input'), 'steps': {}}
        for step in ('exefs', 'romfs'):
            if step in done:
                state['steps'][step] = step_key(step, state['input'])
    return state


def cpu_tag():
    """This processor's features the lifted code's level follows (cpu_level), as Windows reports them; part of the
    compile key, so a game folder moved to a PC without them (or a new processor) is compiled again rather than kept
    with instructions the processor lacks. 'any' where it cannot be read."""
    if os.name != 'nt':
        return 'any'
    try:
        import ctypes
        present = ctypes.windll.kernel32.IsProcessorFeaturePresent
        # PF_SSE4_2_INSTRUCTIONS_AVAILABLE 38, PF_AVX2_INSTRUCTIONS_AVAILABLE 40, PF_AVX512F_INSTRUCTIONS_AVAILABLE 41
        return ''.join('1' if present(f) else '0' for f in (38, 40, 41))
    except (OSError, AttributeError):
        return 'any'


def slot(step):
    """The name a step is recorded under in state.json: its own, or '<step>-debug' for DEBUG_STEPS of --debug."""
    return f'{step}-debug' if DEBUG and step in DEBUG_STEPS else step


def step_key(step, identity, out=None):
    """What a step's output depends on. exefs and romfs: the game files only; lift: the unpacked ExeFS and the
    lifter; compile: the lift, the runtime sources, this script and the processor (cpu_tag); prompts: the game files,
    the kit's button art, the tools that draw it, the bind_ lines of nx2navi.ini, and whether every folder it writes
    is still there."""
    if step in ('exefs', 'romfs'):
        return f'{step}-1|{identity}'
    if step == 'prompts':
        tools = tree_digest(KIT / 'tools', ('pad_prompts.py', 'prompt_art.py', 'button_icons.py'))
        binds = sha256_bytes(json.dumps(bind_lines(out)).encode())
        written = all((out / f / 'manifest.tsv').is_file() for f in prompt_art.OVERLAYS)
        return (f'prompts-1|{identity}|{tree_digest(prompt_art_dir(), ("**/*.png",))}|{tools}|{binds}'
                f'|{"written" if written else "missing"}')
    lift = f'lift|{exefs_digest(out)}|{lift_digest()}'
    if step == 'lift':
        return lift
    compile_key = (f'compile|{lift}|{tree_digest(KIT / "runtime", ("**/*",))}|{sha256_file(Path(__file__))}|{kit_version()}'
                   f'|cpu {cpu_tag()}' + ('|debug' if DEBUG else ''))
    if step == 'compile':
        return compile_key
    # shaders: the built game and the translator that prepared them
    # the translator's id from where the compile step takes it (xlate_dir): a kit's bin/ or a checkout's build/xlate/
    xd = xlate_dir()
    xid = sha256_file(xd / 'nx2navi_xlate.id') if xd and (xd / 'nx2navi_xlate.id').is_file() else 'none'
    return f'shaders-1|{compile_key}|{sha256_file(KIT / "tools" / "prepare_shaders.py")}|{xid}'


def main():
    global EVENTS, DEBUG, EXE_NAME
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--nup', help='the game as one .nup file')
    ap.add_argument('--exefs', help='the game\'s exefs folder (with --romfs, instead of --nup)')
    ap.add_argument('--romfs', help='the game\'s romfs folder')
    ap.add_argument('--rebuild', action='store_true',
                    help='build again from the game files the last run unpacked into --out (after a kit update)')
    ap.add_argument('--retune', action='store_true',
                    help="with --rebuild: compile the lifted code for this processor's level even when the objects "
                         'kept from before an update were compiled for another (Rebuild the Game)')
    ap.add_argument('--out', help='the folder for the finished game')
    ap.add_argument('--check', action='store_true', help='only check the game\'s version; builds nothing')
    ap.add_argument('--msys', default=r'D:\msys64\ucrt64',
                    help='MSYS2 UCRT64 folder (default C:\\msys64\\ucrt64; Windows only: Linux takes clang and lld from PATH)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4, help='parallel compiles')
    ap.add_argument('--events', action='store_true', help='machine-readable progress lines on stdout ("@@" + JSON)')
    ap.add_argument('--debug', action='store_true',
                    help='build ocarina-debug.exe with debug symbols (a PDB on Windows) beside the release build')
    ap.add_argument('--make-runtime-lib', metavar='DIR', help='build the prebuilt runtime library of a release into DIR')
    ap.add_argument('--obj', help='with --make-runtime-lib: the folder for its objects')
    ap.add_argument('--until', choices=STEPS, help=argparse.SUPPRESS)   # tests: stop after this step
    a = ap.parse_args()
    EVENTS = a.events
    DEBUG = a.debug
    if DEBUG:
        EXE_NAME = 'ocarina-debug.exe' if WIN else 'ocarina-debug'
    if a.make_runtime_lib:
        make_runtime_lib(a.make_runtime_lib, Path(a.msys), a.jobs, a.obj)
        return
    given = bool(a.nup) + bool(a.exefs or a.romfs) + bool(a.rebuild)
    if given != 1 or (not a.nup and not a.rebuild and not (a.exefs and a.romfs)):
        ap.error('give --nup, both --exefs and --romfs, or --rebuild')
    if a.rebuild and not a.out:
        ap.error('--rebuild needs --out')
    out = Path(a.out).resolve() if a.out else None
    cache = out / 'build-cache' if out else None
    state_path = cache / 'state.json' if cache else None
    state = load_state(state_path) if state_path else {}

    emit('step', step='check', state='start')
    src = BuiltSource(out, state) if a.rebuild else Source(a)
    check(src)
    say('check: Ocarina of Time 1.1.1 (build e0f3ccfa)')
    emit('check', version='1.1.1', build=MAIN_BUILD_ID[:8])
    emit('step', step='check', state='done')
    if a.check:
        return
    if not out:
        ap.error('--out is needed to build')
    msys = Path(a.msys)
    if WIN:
        for need in ('bin/clang.exe', 'bin/clang++.exe', 'bin/SDL3.dll', 'lib/libSDL3.dll.a', 'include/vma'):
            if not (msys / need).exists():
                fail(f'{msys / need} is missing: install MSYS2 and the packages README-BuildKit.md lists')
    else:
        for tool in ('clang', 'clang++', 'ld.lld'):
            if not shutil.which(tool):
                fail(f'{tool} is not on PATH: install the packages README-BuildKit.md lists under "Linux"')
    packages = {'numpy': 'numpy', 'lz4': 'lz4', **({} if a.rebuild else {'zbic': 'zbic'}),
                **(PROMPT_PACKAGES if prompt_art_dir() else {})}
    for pip_name, mod in packages.items():
        try:
            __import__(mod)
        except ImportError as e:   # its own message too: an installed package can fail to load (a DLL, a clash)
            extra = ' fonttools pillow' if prompt_art_dir() else ''
            fail(f'the Python package {pip_name} is missing or does not load ({e}): '
                 f'pip install numpy lz4 zstandard zbic{extra}')
    cache.mkdir(parents=True, exist_ok=True)
    reset = state.get('version') != STATE_VERSION or state.get('input') != src.identity
    if reset:
        state = {'version': STATE_VERSION, 'input': src.identity, 'steps': {}}
    state['kit'] = kit_version()
    keep_awake()
    t0 = time.time()
    romfs_job = None
    runs = {'exefs': lambda: step_exefs(src, out), 'romfs': lambda: step_romfs(src, out, min(a.jobs, 8)),
            'lift': lambda: step_lift(out, cache),
            'compile': lambda: step_compile(out, cache, msys, a.jobs, romfs_job, a.rebuild and not a.retune, DEBUG),
            'shaders': lambda: step_shaders(out, a.rebuild), 'prompts': lambda: step_prompts(out)}
    state_lock = threading.Lock()   # the RomFS step records itself from its own thread

    def save():
        # whole or not at all: a cut write (a full disk, Cancel ending the process) would leave a state.json that
        # loads as {}, and the launcher would take the built game for none and ask for the dump again
        with state_lock:
            tmp = state_path.with_name(state_path.name + '.tmp')
            tmp.write_text(json.dumps(state, indent=1), encoding='utf-8')
            os.replace(tmp, state_path)

    if reset:
        # on disk at once: the exefs step rewrites <out>/exefs in place, and a run cut short there would leave the
        # old state.json saying the old game's steps are done, which --rebuild trusts
        save()

    def run(name):
        t = time.time()
        runs[name]()
        with state_lock:
            state['steps'][slot(name)] = step_key(name, src.identity, out)
        save()
        say(f'{name}: {time.time() - t:.0f} s')
        emit('step', step=name, state='done', seconds=round(time.time() - t, 1))

    try:
        for name in STEPS:
            if name == 'prompts' and not prompt_art_dir():
                say('prompts: none to draw, the kit has no button art')
                emit('step', step=name, state='skip')
            elif state['steps'].get(slot(name)) == step_key(name, src.identity, out):
                say(f'{name}: done before')
                emit('step', step=name, state='skip')
            else:
                # a step that runs makes every step after it run too: their keys hash its output, not its key (but
                # for OWN_KEY_STEPS, whose keys name their inputs)
                with state_lock:
                    for later in STEPS[STEPS.index(name):]:
                        if later == name or not {name, later} & set(OWN_KEY_STEPS):
                            state['steps'].pop(slot(later), None)
                            if later != name and later in DEBUG_STEPS:   # both builds hang off an earlier step
                                state['steps'].pop(f'{later}-debug', None)
                                state['steps'].pop(later, None)
                say(f'{name}:')
                emit('step', step=name, state='start')
                if name in ('shaders', 'prompts') and romfs_job:   # they read the RomFS image
                    romfs_job.join()
                if name == 'romfs' and a.until != 'romfs':
                    # The RomFS image is mostly disk work, the lift and the compile are processor work: they run side
                    # by side, and only the RomFS index and the files that include it wait for the image
                    # (step_compile). A first build takes about as long as the longer of the two, not their sum.
                    romfs_job = Background(lambda: run('romfs'))
                else:
                    run(name)
            if romfs_job and romfs_job.failed():
                romfs_job.join()
            if name == a.until:
                break
        if romfs_job:
            romfs_job.join()
    except (SystemExit, Exception):
        # a failed lift or compile, or an error of this script: the RomFS image still finishes and is kept, so the
        # next run starts after it (the thread is a daemon, and the process ending would cut its .part file short)
        if romfs_job and romfs_job.thread.is_alive():
            say('waiting for the RomFS image before stopping')
            romfs_job.thread.join()
        raise
    save()
    say(f'\nfinished in {(time.time() - t0) / 60:.1f} min: {out / EXE_NAME}')
    emit('end', exe=str(out / EXE_NAME), seconds=round(time.time() - t0, 1), kit=kit_version())


if __name__ == '__main__':
    main()
