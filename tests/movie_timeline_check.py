"""Compare both movie timestamp patches with Unicorn and their lifted C.

Requires Unicorn and a C compiler (--cc or CC). No game files or GPU are used.
Generated harness files stay in build/test/movie_timeline.
"""
import argparse
import ctypes
import os
from pathlib import Path
import random
import struct
import subprocess
import sys

import unicorn as uc
from unicorn import arm64_const as arm

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'recomp')]
from lifter import Lifter
from patches import load
from tools.check_content import GENERATED_MARKER


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default=os.environ.get('CC', 'clang'))
    args = parser.parse_args()
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', 0x7000000000, 0x4ea0000)])
    sites = [0x3b5c7c4, 0x3b5c908]
    words = []
    for site in sites:
        alternatives = table.sites['main'][site]
        assert len(alternatives) == 1
        key, replacement, original = alternatives[0]
        assert table.sets[key] == ('ostep', 'NX2NAVI_OCARINA_REALTIME', 1)
        assert original == 0xb942fa88
        words.append(replacement)
    assert words[0] == words[1]

    work = ROOT / 'build/test/movie_timeline'
    work.mkdir(parents=True, exist_ok=True)
    code = '// ' + GENERATED_MARKER + '\n#include "cpu.h"\n'
    export = '__declspec(dllexport)' if os.name == 'nt' else ''
    for i, sequence in enumerate(words):
        code += f'{export} void movie_tick_{i}(Cpu *c) {{\n'
        for j, word in enumerate(sequence):
            instruction = Lifter(0, simd='c').lift(word, sites[i] + j * 4)
            assert instruction.kind == 'op'
            code += instruction.code + '\n'
        code += '}\n'
    source = work / 'harness.c'
    library = work / ('harness.dll' if os.name == 'nt' else 'harness.so')
    source.write_text(code)
    subprocess.run([args.cc, '-shared', '-O2', '-fno-strict-aliasing', '-I', str(ROOT / 'runtime'),
                    *([] if os.name == 'nt' else ['-fPIC']), str(source), '-o', str(library)], check=True)
    dll = ctypes.CDLL(str(library))

    # One synthetic layer -> controller -> player -> video surface chain for both engines.
    arena = ctypes.create_string_buffer(0x4000)
    address = ctypes.addressof(arena)
    image = bytearray(0x4000)
    for at, target in [(0x10, address + 0x1000), (0x1000, address + 0x2000),
                       (0x21e8, address + 0x3000)]:
        struct.pack_into('<Q', image, at, target)
    emulator = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
    start = address & ~0xfff
    emulator.mem_map(start, ((address + len(image) + 0xfff) & ~0xfff) - start)
    pc = 0x100000
    emulator.mem_map(pc, 0x1000)
    regs = [getattr(arm, 'UC_ARM64_REG_X' + str(i)) for i in range(31)]
    rng = random.Random(711)
    samples = [-2**63, -1, 0, 1, 8_333_333, 8_333_334, 16_000_000, 17_000_000,
               33_000_000, 154_000_000_000, 3_600_000_000_000]
    samples += [rng.randrange(12 * 3600 * 10**9) for _ in range(256)]
    # The selected 60 Hz video's millisecond-rounded PTS at different redraw rates.
    # Pauses/repeated pictures and skipped pictures must follow PTS, never redraw count.
    for rate in [30, 60, 120, 144, 240]:
        samples += [((render * 60 // rate) * 1000 + 30) // 60 * 1_000_000 for render in range(rate * 2)]
    samples += [1_000_000_000] * 100 + [0, 17_000_000]  # hold, then a new movie

    for i, sequence in enumerate(words):
        native = getattr(dll, f'movie_tick_{i}')
        native.argtypes = [ctypes.c_void_p]
        native.restype = None
        machine = struct.pack('<' + 'I' * len(sequence), *sequence)
        emulator.mem_write(pc, machine)
        for timestamp in samples:
            struct.pack_into('<q', image, 0x30c0, timestamp)
            ctypes.memmove(address, bytes(image), len(image))
            emulator.mem_write(address, bytes(image))
            before = [rng.getrandbits(64) for _ in regs]
            before[20] = address
            flags = rng.randrange(16) << 28
            cpu = ctypes.create_string_buffer(1024 + 16)
            state = (ctypes.addressof(cpu) + 15) & ~15
            ctypes.memmove(state, struct.pack('<31Q', *before), 31 * 8)
            ctypes.memmove(state + 272, struct.pack('<Q', flags), 8)
            for reg, value in zip(regs, before):
                emulator.reg_write(reg, value)
            emulator.reg_write(arm.UC_ARM64_REG_NZCV, flags)
            emulator.emu_start(pc, pc + len(machine))
            native(state)
            actual = struct.unpack('<31Q', ctypes.string_at(state, 31 * 8))
            oracle = tuple(emulator.reg_read(reg) for reg in regs)
            assert actual == oracle, (hex(sites[i]), timestamp, 'C/ARM mismatch')
            expected = (max(timestamp, 0) * 60 + 500_000_000) // 1_000_000_000
            assert actual[8] == expected, (timestamp, actual[8], expected)
            assert all(actual[j] == before[j] for j in range(31) if j not in (8, 9))
            assert emulator.reg_read(arm.UC_ARM64_REG_NZCV) == flags
            assert struct.unpack('<Q', ctypes.string_at(state + 272, 8))[0] == flags
            assert bytes(arena) == bytes(image) and bytes(emulator.mem_read(address, len(image))) == bytes(image)
    print(f'Movie timeline: {len(samples) * 2} C/Unicorn comparisons passed across both patch sites')


if __name__ == '__main__':
    main()
