"""Validate converted synthetic task and mesh shaders without game data.

Run after building the runtime and its shader headers:
    python tests/mesh_fb_spirv_check.py path/to/ocarina.exe

Requires spirv-val on PATH. The draw path is covered separately by --probe-meshfb.
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def shader_words(name: str) -> list[int]:
    header = ROOT / 'runtime' / f'meshfb_probe_{name}_spv.h'
    if not header.is_file():
        raise FileNotFoundError(f'{header} is missing; build the runtime shaders first')
    text = header.read_text(encoding='utf-8')
    words = [int(word, 16) for word in re.findall(r'0x[0-9a-fA-F]+', text)]
    result = words[:5]
    at = 5
    while at < len(words):
        count = words[at] >> 16
        if not count or at + count > len(words):
            raise ValueError(f'{header} contains malformed SPIR-V')
        if not (count == 2 and words[at] & 0xffff == 17 and words[at + 1] == 1):
            result.extend(words[at:at + count])
        at += count
    return result


def stage_input(stage: int, has_task: bool) -> bytes:
    control = bytearray(0x880)
    code = bytearray(0x100)
    for offset, value in ((0, 0x98761234), (0x38, 0x100), (0x54, stage << 16),
                          (0x58, 32 | 1 << 24), (0x5c, 3 << 24 | int(has_task) << 16),
                          (0x60, 64 | 32 << 16)):
        struct.pack_into('<I', control, offset, value)
    if stage == 7:
        code[0x35], code[0x36] = 0xf0, 0x0f
    return struct.pack('<6I', 0, 0, stage, 0, 0, 0) + control + code


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exe', type=Path, help='built runtime executable')
    args = parser.parse_args()
    exe = args.exe.resolve()
    if not exe.is_file():
        parser.error(f'runtime executable does not exist: {exe}')
    validator = shutil.which('spirv-val')
    if not validator:
        parser.error('spirv-val is not on PATH; add the project toolchain directory')

    build = ROOT / 'build'
    build.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='mesh-fb-check-', dir=build) as temp:
        folder = Path(temp)
        (folder / 'out').mkdir()
        names: list[str] = []
        for name, stage, has_task in (('m', 7, False), ('tm', 7, True), ('t', 6, False)):
            original = shader_words(name)
            for explicit in (False, True):
                label = name + ('_explicit' if explicit else '_implicit')
                # The shaders normally declare MeshShading alone; also exercise an input with Shader declared.
                words = original[:5] + ([0x00020011, 1] if explicit else []) + original[5:]
                (folder / f'{label}.spv').write_bytes(struct.pack(f'<{len(words)}I', *words))
                (folder / f'{label}.in').write_bytes(stage_input(stage, has_task))
                names.append(label)
        (folder / 'list.txt').write_text('\n'.join(names) + '\n', encoding='utf-8')
        env = os.environ.copy()
        env['NX2NAVI_PROBE_MESHFB_DIR'] = str(folder)
        subprocess.run([str(exe), '--probe-meshfb'], env=env, check=True)
        outputs = sorted((folder / 'out').glob('*.spv'))
        if len(outputs) != 10:
            raise RuntimeError(f'expected 10 converted modules, found {len(outputs)}')
        for path in outputs:
            subprocess.run([validator, '--target-env', 'vulkan1.2', str(path)], check=True)
        print(f'mesh-fb SPIR-V: {len(outputs)} converted modules validated')


if __name__ == '__main__':
    main()
