"""No 64-to-32-bit narrowing in the files that hold page write generations: runtime/vk_memory.c, runtime/vk_texture.c
and runtime/tex_probe.c, compiled with the build's flags plus -Wshorten-64-to-32, -Wformat and
-Wincompatible-pointer-types as errors (clang -fsyntax-only). The generations are 64 bits (vk_memory.c "page write
generations"); one local, field or format left at 32 bits would bring the wrap of 2^32 back without a word, and the
normal build sees a narrowing only as a silent conversion. The baseline is zero such diagnostics in the whole of the
three files: a narrowing elsewhere in them takes an explicit cast, which also says it is meant.

Needs clang, the Vulkan headers, VMA and the shader headers of tools/build_shaders.py (runtime/*_spv.h). No GPU, no
game files, any Python.

Usage: gen_width_check.py [--cc <clang>]
"""
import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import build_game   # noqa: E402  (RT_FLAGS, the compiler and the include folders of the build)

FILES = ['vk_memory.c', 'vk_texture.c', 'tex_probe.c']
STRICT = ['-Werror=shorten-64-to-32', '-Werror=format', '-Werror=incompatible-pointer-types']
# the diagnostics this check is about, by the flag clang names at the end of the line
WATCHED = re.compile(r'^(?P<where>[^\n]*?:\d+:\d+): (?:error|warning): (?P<what>[^\n]*?)'
                     r'\[(?:-Werror,)?(?P<flag>-Wshorten-64-to-32|-Wformat[\w-]*|-Wincompatible[\w-]*pointer-types)\]',
                     re.M)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cc', help='the C compiler (default: the build\'s clang)')
    args = ap.parse_args()
    if not (ROOT / 'runtime' / 'wb_merge_spv.h').is_file():
        print('gen_width_check: FAIL: runtime/wb_merge_spv.h is missing: run tools/build_shaders.py first')
        return 1
    cc = args.cc or os.environ.get('CC') or str(build_game.CLANG)
    build_game.set_cpu('v3')   # adds -DRT_CPU_LEVEL=3, as the build does
    flags = [f for f in build_game.RT_FLAGS if f != '-g']
    inc = ['-I', str(ROOT / 'runtime'), '-I', str(build_game.vma_include())]
    vk = build_game.vulkan_include(cc)
    if vk:
        inc = ['-I', str(vk)] + inc
    env = dict(os.environ)
    if os.name == 'nt':
        env['PATH'] = str(Path(cc).parent) + os.pathsep + env.get('PATH', '')
    bad = 0
    for name in FILES:
        src = ROOT / 'runtime' / name
        p = subprocess.run([cc, '-fsyntax-only', '-std=gnu11'] + flags + STRICT + inc + [str(src)], capture_output=True,
                           text=True, errors='replace', env=env)
        hits = [m for m in WATCHED.finditer(p.stderr)]
        for m in hits:
            print(f'gen_width_check: FAIL: {m.group("where")}: {m.group("what").strip()} [{m.group("flag")}]')
        if p.returncode and not hits:
            print(f'gen_width_check: FAIL: runtime/{name} does not compile:\n{p.stderr[-3000:]}')
            bad += 1
        bad += len(hits)
        if not hits and not p.returncode:
            print(f'gen_width_check: runtime/{name}: no narrowing, format or pointer-type diagnostic')
    if bad:
        print(f'gen_width_check: {bad} problems')
        return 1
    print('gen_width_check: all checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
