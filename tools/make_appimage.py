"""Build NX2navi-Linux-<version>-x86_64.AppImage: the Linux build kit as one file, which builds Ocarina of Time on the
player's PC from their own dump with tools/make_game.py, as NX2navi-Setup.exe does on Windows. Started without
arguments on a desktop it opens a window to install, update and play (tools/appimage_gui.py, which runs AppRun
with --events and reads its progress lines).

    python3 tools/make_appimage.py --python <standalone CPython 3.14> --appimagetool <appimagetool> [--out dist]
                                   [--dlss libnvidia-ngx-dlss.so.<version> | --no-dlss]

The AppImage holds no game files: the kit's tools/, recomp/, runtime/ and docs, the shader translator built by
tools/build_xlate.py (bin/nx2navi_xlate), NVIDIA's DLSS library with its notice (bin/, tools/dlss_pin.py: downloaded
from NVIDIA's DLSS SDK repository and checked, or --dlss; not with --no-dlss: an AppImage made without the library
downloads it after a build on machines with NVIDIA's driver, AppRun's get_dlss), the pinned Vulkan Memory Allocator
header, the pinned Vulkan headers (toolchain/vulkan-headers, tools/vulkan_pin.py: the runtime names Vulkan extensions
newer than the headers many distributions package), and a relocatable CPython 3.14 (python-build-standalone, as `uv python install 3.14` puts in
~/.local/share/uv/python) with numpy, lz4, zstandard, fonttools and pillow, and with --prompt-art the button art
make_game.py's prompts step draws from (as make_kit.py --prompt-art: the pictures tools/prompt_art.py art_files names,
into the kit's prompt-art folder; without it that step is skipped and the game keeps the Switch prompts). zbic, the GPL
package that unpacks the game's program files, is not bundled: the AppImage installs it with pip into
~/.local/share/nx2navi/pydeps the first time it runs, as the Windows launcher downloads it.

The compiler and the libraries come from the system (README-BuildKit.md, "Linux"): the AppImage checks for clang,
clang++, lld and the headers before it starts, and names the packages for Arch, Fedora and Debian/Ubuntu. Whether
they are new enough is found out by trying them: tools/make_game.py checks the runtime's sources with that compiler
and links against those libraries before the build's long steps (preflight).
"""
import argparse
import os
import shutil
import stat
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
KIT_DIRS = ('recomp', 'runtime')
KIT_FILES = ('VERSION', 'LICENSE', 'LICENSE-ADDITIONS', 'THIRD_PARTY_NOTICES.md', 'nx2navi.ini.example')
TOOLS_SKIP = ('vendor', 'xlate', '__pycache__')   # the translator ships built (bin/); its sources stay out
PACKAGES = ('numpy==2.5.4', 'lz4==4.4.5', 'zstandard==0.25.0', 'fonttools==4.68.0', 'pillow==12.3.0')

APPRUN = r'''#!/bin/bash
# NX2navi build kit for Linux: builds Ocarina of Time (Switch 2, game 1.1.1) from the player's own dump.
HERE="$(dirname "$(readlink -f "$0")")"
PY="$HERE/usr/python/bin/python3"
KIT="$HERE/usr/kit"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/nx2navi"
PYDEPS="$DATA/pydeps"
unset PYTHONPATH PYTHONHOME

usage() {
    cat <<EOF
NX2navi $(cat "$KIT/VERSION") - build Ocarina of Time (Switch 2, game 1.1.1) natively for Linux from your own dump.

  $(basename "${APPIMAGE:-$0}")                                     a window to install, update and play (on a desktop)
  $(basename "${APPIMAGE:-$0}") <your game.nup> [game folder]       build (default folder: ~/Games/Ocarina)
  $(basename "${APPIMAGE:-$0}") --check <your game.nup>             only check the dump's version
  $(basename "${APPIMAGE:-$0}") --make-game <make_game.py options>  run tools/make_game.py directly

The game folder then holds 'ocarina' and 'play.sh', which starts it. Nothing of the game is in this file:
everything the build writes comes from your dump, so keep it to yourself.
EOF
}

# No arguments on a desktop (a double-click): the window (tools/appimage_gui.py), which runs this file with --events.
if [ $# -eq 0 ] && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
    exec "$PY" -I "$KIT/tools/appimage_gui.py" --apprun "$HERE/AppRun" --icon "$HERE/nx2navi.png"
fi
case "$1" in ""|-h|--help) usage; exit 0 ;; esac

# --events: the window's mode. make_game.py prints its "@@" progress lines (--events), and the steps around it print
# theirs (deps, dlss) in the same form.
EVENTS=()
ev() { [ ${#EVENTS[@]} -gt 0 ] && printf '@@{"ev":"%s","step":"%s"%s}\n' "$1" "$2" "${3:+,$3}"; return 0; }
if [ "$1" = --events ]; then EVENTS=(--events); shift; fi
ev step deps '"state":"start"'

missing=()
for tool in clang clang++ ld.lld; do command -v "$tool" >/dev/null || missing+=("$tool"); done
# The Vulkan headers come with this file (the kit's toolchain/vulkan-headers): a distribution's may be too old.
for h in SDL3/SDL.h opus/opus.h vpx/vpx_decoder.h dav1d/dav1d.h zstd.h; do
    [ -e "/usr/include/$h" ] || [ -e "/usr/local/include/$h" ] || missing+=("$h")
done
if [ ${#missing[@]} -gt 0 ]; then
    cat >&2 <<EOF
Missing: ${missing[*]}
Install the compiler and libraries, then run this again:
  Arch, CachyOS, Manjaro, SteamOS (developer mode):
    sudo pacman -S --needed clang lld sdl3 opus libvpx dav1d zstd vulkan-headers vulkan-icd-loader
  Fedora:
    sudo dnf install clang lld SDL3-devel opus-devel libvpx-devel libdav1d-devel libzstd-devel \\
        vulkan-headers vulkan-loader-devel libstdc++-static libatomic
  Debian, Ubuntu:
    sudo apt install clang lld libsdl3-dev libopus-dev libvpx-dev libdav1d-dev libzstd-dev libvulkan-dev
EOF
    ev error deps '"message":"missing packages"'
    exit 1
fi

if ! "$PY" -I -c 'import sys; sys.path[:0] = [sys.argv[1]]; import zbic' "$PYDEPS" 2>/dev/null; then
    echo "Installing zbic (GPL-2.0, unpacks the game's program files) into $PYDEPS ..."
    "$PY" -m pip install --quiet --disable-pip-version-check --target "$PYDEPS" zbic || {
        echo "error: pip could not install zbic (it needs the internet once)" >&2
        ev error deps '"message":"pip could not install zbic (it needs the internet once)"'; exit 1; }
fi
ev step deps '"state":"done"'

# One compile per 4 GB of RAM at most: with 16 GB, one per thread beside the RomFS image fills memory and swap.
mem_gb=$(awk '/MemTotal/ {print int($2 / 1048576)}' /proc/meminfo)
jobs=$(( mem_gb / 4 )); [ "$jobs" -lt 2 ] && jobs=2; [ "$jobs" -gt "$(nproc)" ] && jobs=$(nproc)

run_make_game() {
    "$PY" -I -c 'import sys; sys.path[:0] = [sys.argv[1], sys.argv[2] + "/tools"]; s = sys.argv[2] + "/tools/make_game.py"; sys.argv = [s] + sys.argv[3:]; import runpy; runpy.run_path(s, run_name="__main__")' \
        "$PYDEPS" "$KIT" "$@"
}

case "$1" in
    --make-game) shift; run_make_game "$@"; exit $? ;;
    --check) run_make_game --nup "$2" --check; exit $? ;;
esac

NUP="$1"
OUT="${2:-$HOME/Games/Ocarina}"
[ -f "$NUP" ] || { echo "error: $NUP is not a file" >&2; usage; exit 1; }
# NVIDIA's DLSS for Linux, which the game's DLSS modes need beside it (runtime/resolution.c finds
# libnvidia-ngx-dlss.so.<version> there). The build places the kit's when this file carries it; when it does not
# (made with --no-dlss) and the game folder holds none, the version tools/dlss_pin.py pins is downloaded from
# NVIDIA's DLSS SDK repository, on machines with NVIDIA's driver only, checked against its SHA-256 and kept
# unmodified with the notice that names its licence. NX2NAVI_NO_DLSS=1 skips it.
DLSS_VERSION=@DLSS_VERSION@
DLSS_SO=@DLSS_SO@
DLSS_SHA256=@DLSS_SHA256@
DLSS_URL=@DLSS_URL@
get_dlss() {
    [ -e /proc/driver/nvidia/version ] && [ -z "${NX2NAVI_NO_DLSS:-}" ] || return 0
    compgen -G "$OUT/libnvidia-ngx-dlss.so.*" >/dev/null && return 0   # the kit's, or one the player put there
    echo "Downloading NVIDIA DLSS $DLSS_VERSION for Linux (NVIDIA RTX SDKs License) ..."
    # not under the library's name: the game takes any file whose name starts like it
    local tmp="$OUT/.dlss-download.part"
    rm -f "$tmp"
    if curl -fsSL -o "$tmp" "$DLSS_URL" && echo "$DLSS_SHA256  $tmp" | sha256sum -c --status; then
        mv -f "$tmp" "$OUT/$DLSS_SO"
        cp -f "$KIT/docs/libnvidia-ngx-dlss-NOTICE.txt" "$OUT/libnvidia-ngx-dlss-NOTICE.txt"
    else
        rm -f "$tmp"
        echo "warning: DLSS could not be downloaded or did not match its checksum; the game runs without it" >&2
    fi
}

echo "Building into $OUT with $jobs compiles at a time (${mem_gb} GB of RAM); this takes 10 to 25 minutes."
run_make_game --nup "$NUP" --out "$OUT" --jobs "$jobs" "${EVENTS[@]+"${EVENTS[@]}"}" || exit $?
if [ -e /proc/driver/nvidia/version ] && [ -z "${NX2NAVI_NO_DLSS:-}" ]; then
    ev step dlss '"state":"start"'; get_dlss; ev step dlss '"state":"done"'
else
    ev step dlss '"state":"skip"'
fi

# play.sh: starts the game from its own folder. It sets no storage mode: the game leaves shadow storage off for
# NVIDIA's own driver itself (runtime/vk_memory.c mirror_mode), by the driver of the device it runs on.
{
    echo '#!/bin/bash'
    echo 'cd "$(dirname "$(readlink -f "$0")")"'
    echo 'exec ./ocarina "$@"'
} > "$OUT/play.sh"
chmod +x "$OUT/play.sh"
echo
echo "Done. Start the game with: $OUT/play.sh"
'''

DESKTOP = '''[Desktop Entry]
Type=Application
Name=NX2navi
Comment=Build Ocarina of Time natively for Linux from your own dump
Exec=AppRun
Icon=nx2navi
Terminal=true
Categories=Game;
'''


def copy_tree(src, dst, skip=()):
    shutil.copytree(src, dst, ignore=lambda d, names: [n for n in names if n in skip or n == '__pycache__'])


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--python', type=Path, required=True,
                    help='a relocatable CPython 3.14 folder (python-build-standalone, ~/.local/share/uv/python/cpython-3.14*)')
    ap.add_argument('--appimagetool', type=Path, required=True, help='appimagetool-x86_64.AppImage')
    ap.add_argument('--dlss', type=Path, help="NVIDIA's libnvidia-ngx-dlss.so.<version> from disk instead of "
                                               "downloading it from NVIDIA's DLSS SDK repository")
    ap.add_argument('--no-dlss', action='store_true', help='leave NVIDIA DLSS out of the AppImage')
    ap.add_argument('--out', type=Path, default=ROOT / 'dist', help='where the AppImage goes (default dist/)')
    ap.add_argument('--prompt-art', type=Path, nargs='+', metavar='FOLDER',
                    help='folders holding XGamepad, P5Gamepad and Keyboard_Mouse (or those folders): the button art, as '
                         'make_kit.py --prompt-art takes it (a release zip\'s prompt-art folder does too)')
    a = ap.parse_args()

    version = (ROOT / 'VERSION').read_text(encoding='utf-8').strip()
    xlate = ROOT / 'build' / 'xlate'
    for need in (xlate / 'nx2navi_xlate', xlate / 'nx2navi_xlate.id', ROOT / 'toolchain' / 'vma' / 'vk_mem_alloc.h',
                 a.python / 'bin' / 'python3'):
        if not need.exists():
            sys.exit(f'{need} is missing (tools/build_xlate.py builds the translator; tools/build_game.py fetches VMA)')
    sys.path.insert(0, str(ROOT / 'tools'))
    import dlss_pin
    dlss = None if a.no_dlss else dlss_pin.resolve(a.dlss, ROOT / 'build' / 'dl', linux=True)

    app = ROOT / 'build' / 'appimage' / 'NX2navi.AppDir'
    shutil.rmtree(app, ignore_errors=True)
    kit = app / 'usr' / 'kit'
    kit.mkdir(parents=True)
    import build_shaders
    build_shaders.build(ROOT / 'runtime')   # the image carries the runtime's shaders compiled (runtime/*_spv.h)
    for d in KIT_DIRS:
        copy_tree(ROOT / d, kit / d)
    copy_tree(ROOT / 'tools', kit / 'tools', TOOLS_SKIP)
    for f in KIT_FILES:
        shutil.copy2(ROOT / f, kit / f)
    (kit / 'docs').mkdir()
    shutil.copy2(ROOT / 'docs' / 'README-BuildKit.md', kit / 'docs' / 'README-BuildKit.md')
    # the notice AppRun's get_dlss writes beside a library it downloads; its sha256 line is the one make_game.py reads
    pin = dlss_pin.pin(linux=True)
    (kit / 'docs' / pin.notice).write_text(dlss_pin.notice_text(linux=True), encoding='utf-8')
    (kit / 'bin').mkdir()
    for f in ('nx2navi_xlate', 'nx2navi_xlate.id'):
        shutil.copy2(xlate / f, kit / 'bin' / f)
    if dlss:   # DLSS: NGX loads it from beside the game (runtime/vk_dlaa.c); make_game.py copies it there
        dlss_pin.place(dlss, kit / 'bin', linux=True)
    (kit / 'toolchain' / 'vma').mkdir(parents=True)
    shutil.copy2(ROOT / 'toolchain' / 'vma' / 'vk_mem_alloc.h', kit / 'toolchain' / 'vma' / 'vk_mem_alloc.h')
    # The Vulkan headers the runtime is written against, ahead of the system's on the player's machine
    # (tools/build_game.py vulkan_include finds them in the kit): fetched and checked here when not there yet
    import vulkan_pin
    shutil.copytree(vulkan_pin.resolve(ROOT / vulkan_pin.FOLDER), kit / vulkan_pin.FOLDER)
    if a.prompt_art:
        import prompt_art
        n, size = prompt_art.copy_art(a.prompt_art, kit / prompt_art.FOLDER)
        print(f'button art: {n} files, {size / 1e6:.1f} MB')

    py = app / 'usr' / 'python'
    shutil.copytree(a.python, py, symlinks=True)
    for marker in py.glob('lib/python3*/EXTERNALLY-MANAGED'):   # uv marks its Pythons; this copy is the AppImage's own
        marker.unlink()
    subprocess.run([py / 'bin' / 'python3', '-m', 'pip', 'install', '--quiet', '--disable-pip-version-check',
                    '--no-warn-script-location', *PACKAGES], check=True)
    for cache in py.rglob('__pycache__'):
        shutil.rmtree(cache, ignore_errors=True)

    apprun = APPRUN
    for key, value in (('VERSION', dlss_pin.VERSION), ('SO', pin.name), ('SHA256', pin.sha256), ('URL', pin.url)):
        apprun = apprun.replace(f'@DLSS_{key}@', value)
    (app / 'AppRun').write_text(apprun, encoding='utf-8')
    (app / 'AppRun').chmod(0o755)
    (app / 'nx2navi.desktop').write_text(DESKTOP, encoding='utf-8')
    from PIL import Image   # the window icon of runtime/nx2navi.ico, as PNG
    with Image.open(ROOT / 'runtime' / 'nx2navi.ico') as ico:
        ico.size = max(ico.ico.sizes())
        ico.convert('RGBA').save(app / 'nx2navi.png')

    a.out.mkdir(parents=True, exist_ok=True)
    target = a.out / f'NX2navi-Linux-{version}-x86_64.AppImage'
    env = dict(os.environ, ARCH='x86_64')
    subprocess.run([a.appimagetool, '--no-appstream', app, target], check=True, env=env)
    target.chmod(target.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    print(f'{target} ({target.stat().st_size / 1e6:.0f} MB)')


if __name__ == '__main__':
    main()
