"""AMD FSR 4 for the Linux build, made on the player's PC when they ask for it (the AppImage window's "AMD FSR 4"
button, or AppRun --get-fsr4 <game folder>). AMD ships FSR 4 for Windows only. A Vulkan FSR 4 upscaler for Linux is
built from source here, with the model and shaders it needs taken from AMD's own upscaler DLL of the FSR SDK 2.3.0,
which is downloaded from AMD's release and checked against its SHA-256. Nothing of AMD's is in the repository or in
the AppImage: the download, the extraction and the build happen on this PC, and the result stays in the game folder.

The result, amd_fidelityfx_upscaler_vk.so, goes beside the game (runtime/vk_ffx4.cpp loads it from there, and
runtime/vk_device.c makes the device with what it needs when it is there). Deleting it turns FSR 4 off again.

Steps (each prints an "@@" line with --events, as tools/make_game.py does):
  1. AMD's FSR SDK 2.3.0 release archive (about 120 MB) into the cache, checked; its DX12 upscaler DLL taken out, checked.
  2. The upscaler's source at a pinned commit, with its submodules (git).
  3. CMake and Ninja from PyPI into the AppImage's Python packages, when the system has none.
  4. Its asset setup (reads the DLL) and its build, with the system's GCC (build_env says why not clang).
  5. The library and its licence notices copied beside the game.

    python3 tools/fsr4_linux.py --out <game folder> --accept-amd-license [--events] [--kit <kit>] [--pydeps <dir>]
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

SDK_URL = ('https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v2.3.0/'
           'FidelityFX-Samples-v2.3.0-prebuilt.zip')
SDK_SHA256 = 'f90890b9323bb2f4f2404ac4cdc9395e8495ecdac6f7aa0bcdf1ad1848422273'
SDK_LICENSE_URL = 'https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/v2.3.0/docs/license.md'
DLL_MEMBER = 'Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/amd_fidelityfx_upscaler_dx12.dll'
DLL_SHA256 = 'd0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46'   # FSR 4.1.1.2740
SRC_URL = 'https://github.com/Astyyyyy/fsr4-vk.git'
SRC_COMMIT = 'b0125ea9798633750f83ebabbc00279bcb49d8dd'
LIBRARY = 'amd_fidelityfx_upscaler_vk.so'
NOTICES = 'amd_fidelityfx_upscaler_vk-NOTICES'   # a folder beside the library: its licence and third-party notices

CACHE = Path(os.environ.get('XDG_CACHE_HOME') or Path.home() / '.cache') / 'nx2navi' / 'fsr4'
EVENTS = False


def ev(kind, step='fsr4', **kw):
    if EVENTS:
        print('@@' + json.dumps({'ev': kind, 'step': step, **kw}), flush=True)


def say(text):
    print(text, flush=True)


def fail(message):
    ev('error', message=message)
    print(f'error: {message}', file=sys.stderr, flush=True)
    sys.exit(1)


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def download(url, dest, want, part):
    """dest, fetched once and checked; a file there that does not match is fetched again."""
    if dest.is_file() and sha256(dest) == want:
        return
    tmp = dest.with_suffix(dest.suffix + '.part')
    say(f'Downloading {url}')
    try:
        with urllib.request.urlopen(url, timeout=60) as r, open(tmp, 'wb') as f:
            total, got = int(r.headers.get('Content-Length') or 0), 0
            while block := r.read(1 << 20):
                f.write(block)
                got += len(block)
                if total:
                    ev('progress', fraction=part * got / total, detail=f'{got >> 20} / {total >> 20} MiB')
    except OSError as e:
        tmp.unlink(missing_ok=True)
        fail(f'the download of {url} failed ({e})')
    if sha256(tmp) != want:
        tmp.unlink(missing_ok=True)
        fail(f'{url} does not match its SHA-256: not used')
    tmp.replace(dest)


def run(cmd, cwd=None, env=None):
    say('$ ' + ' '.join(str(c) for c in cmd))
    p = subprocess.Popen([str(c) for c in cmd], cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, errors='replace')
    for line in p.stdout:
        print(line, end='', flush=True)
    if p.wait():
        fail(f'{Path(str(cmd[0])).name} {cmd[1] if len(cmd) > 1 else ""} ended with code {p.returncode}')


def source(work):
    """The pinned commit with its submodules, kept in the cache for the next time."""
    if not shutil.which('git'):
        fail('git is needed to fetch the FSR 4 source: install it, then press the button again')
    src = work / 'src'
    if not (src / '.git').is_dir():
        shutil.rmtree(src, ignore_errors=True)
        run(['git', 'clone', '--no-checkout', SRC_URL, src])
    have = subprocess.run(['git', '-C', src, 'cat-file', '-e', SRC_COMMIT + '^{commit}'], capture_output=True)
    if have.returncode:
        run(['git', '-C', src, 'fetch', 'origin'])
    # every time: a clone made with --no-checkout has no files yet even when its HEAD is already the pinned commit
    run(['git', '-C', src, '-c', 'advice.detachedHead=false', 'checkout', '--force', SRC_COMMIT])
    run(['git', '-C', src, 'submodule', 'update', '--init', '--recursive'])
    return src


def build_env(kit, pydeps):
    """PATH and compiler for the build: the system's GCC; CMake and Ninja from the system, else from PyPI.

    GCC, not the AppImage's clang: the setup checks every shader it converts against a SHA-256 and patches them as
    bytes, and the shader translator it builds numbers its SPIR-V ids in the order the compiler evaluates function
    arguments. Built with clang it writes the same shaders with other ids, and the setup refuses them."""
    env = dict(os.environ)
    path = [str(Path(sys.executable).parent)]
    if not (shutil.which('gcc') and shutil.which('g++')):
        fail('AMD FSR 4 is built with GCC: install gcc and g++ (Arch, CachyOS: base-devel; Fedora: gcc-c++; '
             'Debian, Ubuntu: build-essential), then press the button again')
    env.update(CC=shutil.which('gcc'), CXX=shutil.which('g++'))
    tc = kit / 'linux-toolchain' / 'llvm' / 'bin' if kit else None
    if tc and (tc / 'llvm-objdump').is_file():
        path.append(str(tc))   # its llvm-objdump; the compilers stay GCC (CC, CXX)
    if not (shutil.which('cmake') and shutil.which('ninja')):
        tools = (pydeps or CACHE / 'pydeps')
        if not (tools / 'bin' / 'cmake').is_file() or not (tools / 'bin' / 'ninja').is_file():
            run([sys.executable, '-m', 'pip', 'install', '--quiet', '--disable-pip-version-check', '--target', tools,
                 'cmake', 'ninja'])
        path.append(str(tools / 'bin'))
        env['PYTHONPATH'] = os.pathsep.join(filter(None, [str(tools), env.get('PYTHONPATH')]))
    env['PATH'] = os.pathsep.join(path + [env.get('PATH', '')])
    if not shutil.which('llvm-objdump', path=env['PATH']):
        fail('llvm-objdump is needed to read the DLL: install LLVM (llvm or llvm-tools), then press the button again')
    mem_gb = os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES') >> 30
    env['FSR4_BUILD_JOBS'] = str(max(1, min(os.cpu_count() or 1, mem_gb // 2)))
    return env


def main():
    global EVENTS
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--out', type=Path, required=True, help='the game folder (where ocarina is)')
    ap.add_argument('--accept-amd-license', action='store_true',
                    help=f'agree to the licence of AMD\'s FSR SDK ({SDK_LICENSE_URL}) for the DLL it reads')
    ap.add_argument('--events', action='store_true', help='print "@@" progress lines for the AppImage window')
    ap.add_argument('--kit', type=Path, help='the AppImage\'s kit folder (its compiler)')
    ap.add_argument('--pydeps', type=Path, help='where pip puts CMake and Ninja when the system has none')
    a = ap.parse_args()
    EVENTS = a.events
    ev('step', state='start')
    if not a.accept_amd_license:
        fail(f'the DLL comes under AMD\'s FSR SDK licence ({SDK_LICENSE_URL}): pass --accept-amd-license to agree')
    if not (a.out / 'ocarina').is_file():
        fail(f'{a.out} holds no built game: install it first')
    CACHE.mkdir(parents=True, exist_ok=True)

    zip_path = CACHE / 'FidelityFX-Samples-v2.3.0-prebuilt.zip'
    download(SDK_URL, zip_path, SDK_SHA256, 0.3)
    dll = CACHE / 'amd_fidelityfx_upscaler_dx12.dll'
    if not dll.is_file() or sha256(dll) != DLL_SHA256:
        with zipfile.ZipFile(zip_path) as z, z.open(DLL_MEMBER) as f, open(dll, 'wb') as o:
            shutil.copyfileobj(f, o)
        if sha256(dll) != DLL_SHA256:
            dll.unlink()
            fail('the upscaler DLL in AMD\'s archive does not match its SHA-256')
    ev('progress', fraction=0.3, detail='AMD FSR SDK 2.3.0')

    src = source(CACHE)
    ev('progress', fraction=0.4, detail='source')
    env = build_env(a.kit, a.pydeps)
    # the build folder holds CMake caches and shader tools made by one compiler: another one starts it again
    cc = subprocess.run([env['CXX'], '--version'], capture_output=True, text=True).stdout.split('\n')[0]
    stamp = src / 'build' / 'nx2navi-compiler.txt'
    if (src / 'build').is_dir() and (not stamp.is_file() or stamp.read_text().strip() != cc):
        shutil.rmtree(src / 'build')
    (src / 'build').mkdir(exist_ok=True)
    stamp.write_text(cc + '\n')
    run([sys.executable, 'tools/setup.py', '--dll', dll], cwd=src, env=env)
    ev('progress', fraction=0.6, detail='model and shaders')
    run(['bash', 'tools/build/linux.sh', '-G', 'Ninja'], cwd=src, env=env)
    built = src / 'build' / 'linux' / LIBRARY
    if not built.is_file():
        fail(f'the build wrote no {LIBRARY}')
    ev('progress', fraction=0.95, detail=LIBRARY)

    tmp = a.out / (LIBRARY + '.part')
    shutil.copy2(built, tmp)
    tmp.replace(a.out / LIBRARY)
    notes = a.out / NOTICES
    shutil.rmtree(notes, ignore_errors=True)
    notes.mkdir()
    for f in ('LICENSE', 'THIRD_PARTY_NOTICES.md'):
        if (src / f).is_file():
            shutil.copy2(src / f, notes / f)
    if (src / 'LICENSES').is_dir():
        shutil.copytree(src / 'LICENSES', notes / 'LICENSES')
    say(f'AMD FSR 4 for Linux: {a.out / LIBRARY}. The game offers it under Screen, Upscaling, where the card runs it; '
        f'delete the file to turn it off.')
    ev('step', state='done')


if __name__ == '__main__':
    main()
