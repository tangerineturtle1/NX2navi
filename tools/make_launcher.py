"""Build a release of the NX2navi launcher (docs/launcher.md, "Releases").

    python tools/make_launcher.py --xlate build/xlate [--ffx amd_fidelityfx_vk.dll] [--dlss nvngx_dlss.dll | --no-dlss]
                                  [--ffx4 FOLDER]
                                  [--prompt-art FOLDER [FOLDER ...]] [--out build/launcher-release]
                                  [--pgo build/pgo/pgo-v111.profdata] [--feed owner/name]
                                  [--notes notes.md] [--publish]

Needs Python 3.14 (tarfile and zstd for the MSYS2 packages), uv on PATH, network access on the first run, and
either Windows (msiexec, and the C compiler of MSYS2 it downloads itself) or Linux with clang, lld and llvm-windres
(it cross-compiles against the same MSYS2 packages) plus msitools. Downloads are cached in toolchain/launcher-dl/ and
checked against the sha256 in PINS.

The release folder (--out) holds:

    NX2navi/                      the launcher folder exactly as NX2navi-Setup.exe installs it:
      NX2navi.exe                 launcher/native/nx2navi.c with the Navi icon (launcher/ui/art.py)
      python/                     python.org's embeddable Python 3.14.9, tkinter and Tcl/Tk 9.0 from its tcltk.msi,
                                  the packages of tools/requirements-launcher.txt, runtime-id.txt
      app/current.txt, app/<version>/   launcher/ (without native/), runtime/, recomp/, the tools make_game.py uses,
                                  bin/nx2navi_xlate.exe and .id (and amd_fidelityfx_vk.dll with --ffx, AMD's two
                                  FSR 4 libraries with --ffx4),
                                  bin/nvngx_dlss.dll and its notice (NVIDIA DLSS at the version tools/dlss_pin.py
                                  pins, downloaded from NVIDIA's DLSS SDK repository and checked, or --dlss; not with
                                  --no-dlss),
                                  bin/dlssg/ with --dlssg (DLSS Frame Generation: nx2navi_dlssg.dll, Streamline's
                                  runtime DLLs and nvngx_dlssg.dll at the version tools/streamline_pin.py pins,
                                  downloaded from NVIDIA's Streamline SDK release and checked, or --streamline; their
                                  licences and a notice),
                                  lib/libnx2navirt.a (the runtime, prebuilt with the release's compiler),
                                  data/pgo-v111.profdata and .json with --pgo (the execution profile make_game.py
                                  compiles the lifted code and the runtime with, the prebuilt runtime included, and
                                  its record: tools/pgo.py merge),
                                  prompt-art/ with --prompt-art (the button art make_game.py draws the Xbox,
                                  PlayStation, Steam Deck and keyboard and mouse prompts from, tools/prompt_art.py art_files),
                                  toolchain.lock.json (the MSYS2 packages of today, pinned), feed.json, VERSION, the
                                  licence files
    NX2navi-Setup-<version>.exe   launcher/native/setup.c with the folder above appended as a zip: the one file
                                  players download
    NX2navi-app-<version>.zip     app/<version> alone: what the launcher downloads to update itself
    NX2navi-runtime-<id>.zip      python/ alone: downloaded only when a release changes the Python
    NX2navi-update.json           the manifest the launcher reads (launcher/feed.py)

--prompt-art names the button art: folders that hold XGamepad, P5Gamepad and Keyboard_Mouse, or those three folders
themselves. Only the pictures tools/pad_prompts.py reads are copied; the art stays out of the repository. Without it
the release has no prompt-art folder and builds the game with the game's own prompts only.

--pgo names the execution profile of a play session (docs/building.md, "Profile-guided build"). It is derived from
the game's code and stays out of the repository; its record must name this checkout's recomp/, or the release stops
(tools/pgo.py fitting). Without it the release builds the game as before, the lifted code at -O1. The profile is
part of NX2navi-app-<version>.zip, so an update brings it and the launcher's next build of the game compiles with it.

--publish uploads the four files as GitHub release v<version> of the --feed repository with the gh CLI (a draft
first is wiser: add --draft). check_content runs over everything before it is packed: the release holds no game data.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))
import build_shaders  # noqa: E402
import check_content  # noqa: E402
import dlss_pin  # noqa: E402
import pgo  # noqa: E402
import prompt_art  # noqa: E402
import streamline_pin  # noqa: E402
from launcher import toolchain  # noqa: E402
from launcher.paths import Layout  # noqa: E402

MAIN_ROOT = (ROOT / 'build').resolve().parent   # build/ and toolchain/ are junctions in a worktree
DL = MAIN_ROOT / 'toolchain' / 'launcher-dl'
WORK = MAIN_ROOT / 'toolchain' / 'launcher-work'
PY_VERSION = '3.14.9'
PINS = {
    'python-embed': (f'https://www.python.org/ftp/python/{PY_VERSION}/python-{PY_VERSION}-embed-amd64.zip',
                     '3c01d7cfc666f746a6f064f60fb2156ab0ec464d3d738ab796371a46a9bb24a2'),
    # the installer's own Tcl/Tk and tkinter component, published beside it
    'tcltk': (f'https://www.python.org/ftp/python/{PY_VERSION}/amd64/tcltk.msi',
              'ac87609db3f715404dd41a989c2f1706ea1512f3d86217f938d11c63af8eb505'),
}
TOOLS = ('make_game.py', 'nup.py', 'romfs_index.py', 'check_content.py', 'hostmem.py', 'prepare_shaders.py',
         'shader_census.py', 'shader_list.py', 'pipeline_list.py', 'zstd_compat.py', 'pad_prompts.py', 'prompt_art.py', 'button_icons.py', 'romfs_check.py',
         'pack_shader_cache.py', 'mod_files.py', 'cheats.py', 'exefs_patches.py')
TOP = ('LICENSE', 'LICENSE-ADDITIONS', 'THIRD_PARTY_NOTICES.md', 'nx2navi.ini.example')
SKIP_DIRS = {'__pycache__', '.pytest_cache', 'native'}
SKIP_SUFFIXES = {'.o', '.obj', '.pyc', '.exe', '.dll', '.a', '.lib'}
TCLTK_FILES = ('_tkinter.pyd', 'tcl90.dll', 'tcl9tk90.dll', 'zlib1.dll', 'libtommath.dll')
DEFAULT_FEED = 'tangerineturtle1/NX2navi'


def say(msg):
    print(msg, flush=True)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while b := f.read(1 << 20):
            h.update(b)
    return h.hexdigest()


def fetch(name):
    url, want = PINS[name]
    dest = DL / url.rsplit('/', 1)[1]
    if dest.is_file() and sha256_file(dest) == want:
        return dest
    from launcher import net
    say(f'download {url}')
    net.download(url, dest, sha256=want)
    return dest


def run(cmd, **kw):
    say('  ' + ' '.join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], check=True, **kw)


def copy_tree(src, dst, skip_dirs=SKIP_DIRS):
    for p in sorted(src.rglob('*')):
        rel = p.relative_to(src)
        if any(part in skip_dirs for part in rel.parts) or p.is_dir() or p.suffix.lower() in SKIP_SUFFIXES:
            continue
        (dst / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, dst / rel)


def version():
    v = (ROOT / 'VERSION').read_text(encoding='utf-8').strip()
    r = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--short', 'HEAD'], capture_output=True, text=True)
    return v, f'{v}+{r.stdout.strip()}' if r.returncode == 0 and r.stdout.strip() else v


# ---------------------------------------------------------------- python/

def tcltk_files():
    """The Tcl/Tk files of the pinned msi, unpacked into WORK/tcltk once. The stamp names the msi they came from and
    is written last: an unpack from an older pin, or one stopped after _tkinter.pyd was written, is done again."""
    dest = WORK / 'tcltk'
    msi = fetch('tcltk')
    stamp, want = dest / '.unpacked-from', f'{msi.name} {msi.stat().st_size}'
    if (dest / 'DLLs' / '_tkinter.pyd').is_file() and stamp.is_file() and stamp.read_text() == want:
        return dest
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    if os.name == 'nt':   # an administrative image: copies the files, installs nothing
        run(['msiexec', '/a', msi, '/qn', f'TARGETDIR={dest}'])
    else:
        run(['msiextract', '-C', dest, msi], stdout=subprocess.DEVNULL)
    if not (dest / 'DLLs' / '_tkinter.pyd').is_file():
        sys.exit(f'{dest}: no _tkinter.pyd in {msi.name}')
    stamp.write_text(want)
    return dest


def runtime_id():
    req = (ROOT / 'tools' / 'requirements-launcher.txt').read_bytes()
    return f'py{PY_VERSION}-{hashlib.sha256(req + PINS["tcltk"][1].encode()).hexdigest()[:8]}'


def build_python(py):
    with zipfile.ZipFile(fetch('python-embed')) as z:
        z.extractall(py)
    # The embeddable Python's sys.path is exactly this list; ../pydeps holds zbic (launcher/toolchain.py).
    (py / 'python314._pth').write_text('python314.zip\n.\nLib/site-packages\n../pydeps\n', encoding='utf-8')
    env = dict(os.environ, UV_CACHE_DIR=str(MAIN_ROOT / 'toolchain' / 'uv-cache'))
    run(['uv', 'pip', 'install', '--target', py / 'Lib' / 'site-packages', '--python', sys.executable,
         '--python-platform', 'windows', '--python-version', '3.14', '--only-binary', ':all:', '-q',
         '-r', ROOT / 'tools' / 'requirements-launcher.txt'], env=env)
    shutil.rmtree(py / 'Lib' / 'site-packages' / 'bin', ignore_errors=True)
    tk = tcltk_files()
    for dll in TCLTK_FILES:
        if (tk / 'DLLs' / dll).is_file():
            shutil.copy2(tk / 'DLLs' / dll, py / dll)
    copy_tree(tk / 'Lib' / 'tkinter', py / 'tkinter', skip_dirs={'__pycache__', 'test'})
    copy_tree(tk / 'tcl', py / 'tcl', skip_dirs={'__pycache__'})
    (py / 'runtime-id.txt').write_text(runtime_id() + '\n', encoding='utf-8')


# ---------------------------------------------------------------- app/<version>/

def build_app(app, ver, xlate, feed_repo, lock, ffx=None, dlss=None, art=None, dlssg=None, pipelines=None, ffx4=None,
              pgo_profile=None):
    # The runtime's own shaders are source in the repository and SPIR-V headers in a release: a player's PC compiles
    # runtime/ (make_game.py) and has no shader compiler. Build the missing and the stale ones before the copy.
    build_shaders.build(ROOT / 'runtime')
    copy_tree(ROOT / 'launcher', app / 'launcher')
    for d in ('runtime', 'recomp'):
        copy_tree(ROOT / d, app / d)
    (app / 'tools').mkdir(parents=True)
    for t in TOOLS:
        shutil.copy2(ROOT / 'tools' / t, app / 'tools' / t)
    for t in TOP:
        if (ROOT / t).is_file():
            shutil.copy2(ROOT / t, app / t)
    (app / 'VERSION').write_text(ver + '\n', encoding='utf-8')
    (app / 'bin').mkdir()
    for n in ('nx2navi_xlate.exe', 'nx2navi_xlate.id'):
        if not (xlate / n).is_file():
            sys.exit(f'{xlate / n} is missing: build the shader translator first (tools/build_xlate.py)')
        shutil.copy2(xlate / n, app / 'bin' / n)
    if ffx:   # FSR 3.1 loads it from beside the game (runtime/vk_ffx.cpp); make_game.py copies it there
        shutil.copy2(ffx, app / 'bin' / 'amd_fidelityfx_vk.dll')
    if ffx4:   # FSR 4 loads them from beside the game (runtime/vk_ffx4.cpp); make_game.py copies them there
        for n in ('amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll'):
            shutil.copy2(ffx4 / n, app / 'bin' / n)
        if (ffx4 / 'amd_fidelityfx_framegeneration_vk.dll').is_file():   # its frame generation (runtime/vk_fg4.cpp)
            shutil.copy2(ffx4 / 'amd_fidelityfx_framegeneration_vk.dll', app / 'bin' / 'amd_fidelityfx_framegeneration_vk.dll')
    if dlss:   # DLSS: NGX loads it from beside the game (runtime/vk_dlaa.c); make_game.py copies it there
        dlss_pin.place(dlss, app / 'bin')
    if dlssg:   # DLSS Frame Generation (runtime/vk_dlssg.c): make_game.py place_dlssg copies it beside the game
        streamline_pin.place(dlssg[0], dlssg[1], app / 'bin' / 'dlssg')
    if art:   # make_game.py draws the button prompts from it (its prompts step)
        n, size = prompt_art.copy_art(art, app / prompt_art.FOLDER)
        say(f'button art: {n} pictures, {size / 1e6:.1f} MB')
    if pipelines:   # prepare_shaders.py reads it there (BUNDLED_PIPELINES)
        (app / 'tools' / 'data').mkdir(parents=True, exist_ok=True)
        shutil.copy2(pipelines, app / 'tools' / 'data' / 'pipeline_list.bin')
        say(f'pipeline list: {pipelines.stat().st_size / 1e6:.1f} MB')
    if pgo_profile:   # make_game.py compiles with it (pgo_file); here before the prebuilt runtime is built with it
        placed = pgo.place(pgo_profile, app / 'data')
        say(f'execution profile: {placed.stat().st_size / 1e6:.1f} MB')
    else:
        say('execution profile: none (--pgo): the game\'s code is built without one, at -O1')
    (app / 'toolchain.lock.json').write_text(json.dumps(lock, indent=1), encoding='utf-8')
    (app / 'feed.json').write_text(json.dumps({'type': 'github', 'repo': feed_repo}, indent=1), encoding='utf-8')


# ---------------------------------------------------------------- the native programs

def native_compiler(lock):
    """(cc argv prefix, windres argv prefix) for x86_64 Windows: MSYS2's own clang on Windows, else the host's clang
    against the same MSYS2 packages as a sysroot."""
    lay = Layout(root=MAIN_ROOT / 'toolchain' / 'launcher-msys')
    if not toolchain.is_current(lay, lock):
        say('MSYS2 packages for the native programs')
        toolchain.install(lay, lock, log=say)
    sysroot = lay.msys
    if os.name == 'nt':
        return [str(sysroot / 'bin' / 'clang.exe')], [str(sysroot / 'bin' / 'windres.exe')]
    for tool in ('clang', 'ld.lld'):
        if not shutil.which(tool):
            sys.exit(f'{tool} is needed to cross-compile the launcher programs')
    windres = shutil.which('llvm-windres') or shutil.which('llvm-windres-18') or '/usr/lib/llvm-18/bin/llvm-windres'
    if not Path(windres).exists():
        sys.exit('llvm-windres is needed to compile the launcher programs\' resources')
    return (['clang', '--target=x86_64-w64-mingw32', f'--sysroot={sysroot}', '-fuse-ld=lld'],
            [windres, '--target=x86_64-w64-mingw32'])


def build_runtime_lib(folder, ver, out, wine):
    """app/<version>/lib/libnx2navirt.a: the runtime compiled once here with the release's MSYS2 clang, so players
    compile only the few runtime files that include per-install headers (tools/make_game.py --make-runtime-lib). It
    needs MSYS2's own clang.exe: native on Windows, through Wine with --wine elsewhere; without either players compile
    runtime/ themselves."""
    msys = Layout(root=MAIN_ROOT / 'toolchain' / 'launcher-msys').msys
    app = folder / 'app' / ver
    args = ['--make-runtime-lib', app / 'lib', '--obj', out / 'native' / 'runtime-obj', '--msys', msys]
    if os.name == 'nt':
        run([sys.executable, app / 'tools' / 'make_game.py', *args])
    elif wine:
        def win(p):
            return 'Z:' + str(Path(p).resolve()).replace('/', '\\')
        env = dict(os.environ, WINEDEBUG='-all', NX2NAVI_AR=win(msys / 'bin' / 'ar.exe'))
        run(['wine', folder / 'python' / 'python.exe', win(app / 'tools' / 'make_game.py'),
             *[win(a) if isinstance(a, Path) else a for a in args]], env=env)
    else:
        say('note: no prebuilt runtime (it is built on Windows, or with --wine): players compile runtime/ themselves')
        return
    if not (app / 'lib' / 'libnx2navirt.a').is_file():
        sys.exit('the prebuilt runtime was not written')
    # The release's compiler is the one players build with (toolchain.lock.json): a profile it left out of the
    # runtime (make_game.py pgo_profile says why in the lines above) their builds would leave out too
    built = json.loads((app / 'lib' / 'runtime-lib.json').read_text(encoding='utf-8'))
    if (app / 'data' / 'pgo-v111.profdata').is_file() and not built.get('pgo'):
        sys.exit('the prebuilt runtime was compiled without the release\'s execution profile (the build above says '
                 'why): the players\' builds would leave the profile out as well')


def build_native(work, cc, windres):
    from launcher.ui import art
    work.mkdir(parents=True, exist_ok=True)
    (work / 'nx2navi.ico').write_bytes(art.navi_ico())
    shutil.copy2(ROOT / 'launcher' / 'native' / 'app.manifest', work / 'app.manifest')
    shutil.copy2(ROOT / 'launcher' / 'native' / 'res.rc', work / 'res.rc')
    res = (work / 'res.o').resolve()
    # MSYS2's GNU windres runs its preprocessor through cmd.exe and, left to find it, names the gcc beside it by full
    # path, which cmd.exe splits at a space once the .rc's path holds one too. So gcc by bare name, found on PATH (the
    # toolchain's own bin first; no MSYS2 shell sets it), and the .rc by its name in the work folder, where windres runs.
    # llvm-windres (Linux) starts its preprocessor without a shell.
    pp = [] if 'llvm' in Path(windres[0]).name else [
        '--preprocessor=gcc', '--preprocessor-arg=-E', '--preprocessor-arg=-xc', '--preprocessor-arg=-DRC_INVOKED']
    env = dict(os.environ, PATH=str(Path(windres[0]).parent) + os.pathsep + os.environ.get('PATH', ''))
    run([*windres, *pp, '-I', '.', 'res.rc', '-O', 'coff', '-o', res], env=env, cwd=work)
    flags = ['-municode', '-mwindows', '-O2', '-Wall', '-static', '-s']
    stub = work / 'NX2navi.exe'
    setup = work / 'setup-stub.exe'
    run([*cc, *flags, ROOT / 'launcher' / 'native' / 'nx2navi.c', res, '-o', stub, '-lshell32', '-lole32', '-luuid'])
    run([*cc, *flags, ROOT / 'launcher' / 'native' / 'setup.c', res, '-o', setup, '-l:libz.a', '-lshell32',
         '-lole32', '-luuid', '-ladvapi32'])
    return stub, setup


# ---------------------------------------------------------------- packing

def zip_dir(src, dest, prefix=''):
    with zipfile.ZipFile(dest, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in sorted(src.rglob('*')):
            if p.is_file():
                z.write(p, (Path(prefix) / p.relative_to(src)).as_posix())
    return dest


def make_setup(stub, payload, dest, ver):
    """setup-stub.exe + the payload zip + the 48-byte trailer launcher/native/setup.c reads."""
    data = stub.read_bytes()
    off = len(data)
    z = payload.read_bytes()
    trailer = b'NX2NSET1' + off.to_bytes(8, 'little') + len(z).to_bytes(8, 'little') + \
        ver.encode('utf-8')[:24].ljust(24, b'\0')
    dest.write_bytes(data + z + trailer)
    return dest


def check_folder(folder):
    bad = [f'{p.relative_to(folder)}: {m}' for p in sorted(folder.rglob('*')) if p.is_file()
           for m in check_content.problems_for(p, allow_large=True)]
    if bad:
        sys.exit('check_content refuses these release files:\n  ' + '\n  '.join(bad[:20]))


def entry(path):
    return {'name': path.name, 'size': path.stat().st_size, 'sha256': sha256_file(path)}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--xlate', type=Path, required=True, help='folder with nx2navi_xlate.exe and nx2navi_xlate.id')
    ap.add_argument('--ffx', type=Path, help="AMD's signed amd_fidelityfx_vk.dll (FidelityFX SDK 1.1.4, MIT) for FSR 3.1")
    ap.add_argument('--ffx4', type=Path, help="the folder with AMD's signed amd_fidelityfx_loader_vk.dll and "
                                              'amd_fidelityfx_upscaler_vk.dll (FSR SDK 2.4.0, MIT) for FSR 4, and its '
                                              'amd_fidelityfx_framegeneration_vk.dll when it is there')
    ap.add_argument('--dlss', type=Path, help=f"NVIDIA's nvngx_dlss.dll (DLSS {dlss_pin.VERSION}) from disk instead of "
                                               "downloading it from NVIDIA's DLSS SDK repository")
    ap.add_argument('--no-dlss', action='store_true', help='leave NVIDIA DLSS out of the release')
    ap.add_argument('--dlssg', type=Path, metavar='DLL',
                    help='nx2navi_dlssg.dll (tools/build_dlssg.py) for DLSS Frame Generation')
    ap.add_argument('--streamline', type=Path, metavar='SDK',
                    help=f'the Streamline {streamline_pin.VERSION} SDK folder whose bin/x64 gives the Streamline and '
                         'nvngx_dlssg.dll files, instead of downloading the SDK release with --dlssg')
    ap.add_argument('--prompt-art', type=Path, nargs='+', metavar='FOLDER',
                    help='the button art: folders holding XGamepad, P5Gamepad and Keyboard_Mouse, or those folders')
    ap.add_argument('--pipeline-list', type=Path, metavar='FILE',
                    help='the shared pipeline list to carry (tools/pipeline_list.py merge writes it; it is derived from game data and stays out of the repository)')
    ap.add_argument('--pgo', type=Path, metavar='FILE',
                    help='the execution profile to carry, with its record beside it (tools/pgo.py merge writes '
                         'build/pgo/pgo-v111.profdata and .json; derived from the game\'s code, they stay out of the '
                         'repository)')
    ap.add_argument('--out', type=Path, default=MAIN_ROOT / 'build' / 'launcher-release')
    ap.add_argument('--feed', default=DEFAULT_FEED, help='the GitHub repository the launcher looks for releases in')
    ap.add_argument('--notes', type=Path, help='release notes (markdown) for the manifest and the GitHub release')
    ap.add_argument('--lock', type=Path, help='a toolchain.lock.json to reuse instead of resolving today\'s packages')
    ap.add_argument('--version', help='the release version instead of VERSION\'s (test releases)')
    ap.add_argument('--wine', action='store_true', help='on Linux: build the prebuilt runtime with MSYS2\'s clang '
                                                         'through Wine')
    ap.add_argument('--publish', action='store_true', help='upload as GitHub release v<version> with gh')
    ap.add_argument('--draft', action='store_true', help='with --publish: as a draft')
    a = ap.parse_args()
    if sys.version_info < (3, 14):
        sys.exit('Python 3.14 or later is needed (compression.zstd for the MSYS2 packages)')
    ver, build = version()
    if a.version:
        ver, build = a.version, a.version + build[len(ver):]
    if a.prompt_art:
        prompt_art.art_files(a.prompt_art)   # the art is all there before anything is downloaded or built
    if a.pgo:
        pgo.fitting(a.pgo)   # the profile fits this checkout's recomp/ before anything is downloaded or built
    out = a.out
    shutil.rmtree(out, ignore_errors=True)
    folder = out / 'NX2navi'
    t0 = time.time()
    lock = (json.loads(a.lock.read_text(encoding='utf-8')) if a.lock
            else toolchain.make_lock(fast_repo=toolchain.FAST_REPO))
    say(f'release {build}: toolchain lock with {len(lock["packages"])} packages'
        + (f' (CLANG64 clang left out: {lock["fast_skipped"]})' if lock.get('fast_skipped') else ''))
    dlss = None if a.no_dlss else dlss_pin.resolve(a.dlss, DL)
    say(f'NVIDIA DLSS {dlss_pin.VERSION}: {dlss}' if dlss else 'NVIDIA DLSS left out (--no-dlss)')
    if a.streamline and not a.dlssg:
        sys.exit('--streamline goes with --dlssg')
    dlssg = (a.dlssg, streamline_pin.resolve(a.streamline, DL)) if a.dlssg else None
    say(f'DLSS Frame Generation: {a.dlssg}, Streamline {streamline_pin.VERSION}: {dlssg[1]}' if dlssg
        else 'DLSS Frame Generation left out (no --dlssg)')
    if a.pipeline_list:
        import pipeline_list
        if pipeline_list.unpack(a.pipeline_list.read_bytes()) is None:
            sys.exit(f'{a.pipeline_list} is not a pipeline list (tools/pipeline_list.py merge writes one)')
    build_app(folder / 'app' / ver, ver, a.xlate, a.feed, lock, a.ffx, dlss, a.prompt_art, dlssg, a.pipeline_list, a.ffx4,
              a.pgo)
    (folder / 'app' / 'current.txt').write_text(ver + '\n', encoding='utf-8')
    build_python(folder / 'python')
    cc, windres = native_compiler(lock)
    build_runtime_lib(folder, ver, out, a.wine)
    stub, setup_stub = build_native(out / 'native', cc, windres)
    shutil.copy2(stub, folder / 'NX2navi.exe')
    check_folder(folder)
    app_zip = zip_dir(folder / 'app' / ver, out / f'NX2navi-app-{ver}.zip')
    rt_zip = zip_dir(folder / 'python', out / f'NX2navi-runtime-{runtime_id()}.zip')
    payload = zip_dir(folder, out / 'native' / 'payload.zip')
    setup = make_setup(setup_stub, payload, out / f'NX2navi-Setup-{ver}.exe', ver)
    notes = a.notes.read_text(encoding='utf-8') if a.notes else ''
    manifest = {'format': 1, 'version': ver, 'build': build, 'notes': notes,
                'app': entry(app_zip), 'runtime': {'id': runtime_id(), **entry(rt_zip)},
                'toolchain': toolchain.lock_digest(lock), 'setup': entry(setup),
                'made': time.strftime('%Y-%m-%d %H:%M')}
    man = out / 'NX2navi-update.json'
    man.write_text(json.dumps(manifest, indent=1), encoding='utf-8')
    for p in (setup, app_zip, rt_zip, man):
        say(f'{p.name}: {p.stat().st_size / 1e6:.1f} MB')
    say(f'release folder {out} in {time.time() - t0:.0f} s')
    if a.publish:
        cmd = ['gh', 'release', 'create', f'v{ver}', setup, app_zip, rt_zip, man, '--repo', a.feed,
               '--title', f'NX2navi {ver}']
        cmd += ['--notes-file', a.notes] if a.notes else ['--notes', f'NX2navi {ver}']
        if a.draft:
            cmd.append('--draft')
        run(cmd)


if __name__ == '__main__':
    main()
