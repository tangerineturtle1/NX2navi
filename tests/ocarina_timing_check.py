"""Check the set orecord of recomp/patches-v111.txt: the ocarina recorder's clock carries each update's fraction of a
60 Hz frame on the patch page's +264 instead of cutting it.

1. The table: the set's switch and default, its four sites, their original word and the nine replacement words, the
   page word resolved to adrp x8 of the patch page at each site, and no other set's word loading or storing +264.
2. The lifted words (recomp/lifter.py, as the generator lifts them) against the game's word and a float32 model:
   steps of one and two frames give the game's count with the fraction left at 0; the steps of the 40 to 240 fps
   limits count 3600 frames a minute; NaN, negative, infinite and huge steps give the game's count; only x8, v16-v18
   and FPSR change.
3. Unicorn: the nine words, the adrp included, run at each site against the lifted C over random steps and fractions:
   registers and the page agree bit for bit.

Requires Unicorn, numpy and a C compiler (--cc or CC). No game files. Harness files stay in build/test/ocarina_timing.
"""
import argparse
import ctypes
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys

import numpy as np
import unicorn as uc
from unicorn import arm64_const as arm

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT), str(ROOT / 'recomp')]
from lifter import Lifter  # noqa: E402
from patches import load  # noqa: E402
from tools.check_content import GENERATED_MARKER  # noqa: E402

MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')
BASE = 0x7000000000
SIZE = 0x4ea0000   # a stand-in main size: the patch page lands at main+0x4ea0000, as on 1.1.1
SITES = (0x32926c8, 0x3293410, 0x3296b08, 0x3296da4)
GAME = 0x1e390008  # fcvtzu w8, s0
WORDS = [('page', 8), 0xbd410910, 0x1e302810, 0x1e25c211, 0x1e313a11, 0x2f00e412, 0x1e326a31, 0xbd010911,
         0x1e390208]
FRACTION = 264     # the page byte the words keep the fraction in
CPU_SIZE = 1024
X8, NZCV, FPCR, FPSR, V = 8 * 8, 272, 320, 328, 512


def compiler(cc):
    if cc:
        return cc
    if os.name == 'nt' and (MSYS_BIN / 'clang.exe').exists():
        return str(MSYS_BIN / 'clang.exe')
    return shutil.which('clang') or 'clang'


def f32(bits):
    return np.frombuffer(struct.pack('<I', bits), dtype='<f4')[0]


def bits(value):
    return struct.unpack('<I', np.float32(value).tobytes())[0]


def fcvtzu(value):
    """fcvtzu w, s: toward zero, NaN and negatives to 0, saturated at 2^32 - 1."""
    if np.isnan(value) or value <= 0:
        return 0
    if value >= np.float32(4294967296.0):
        return 0xffffffff
    return int(np.trunc(value))


def model(step, fraction):
    """The words in float32 with round to nearest: (w8, the new fraction's bits)."""
    with np.errstate(all='ignore'):
        total = np.float32(step) + np.float32(fraction)
        rest = total - np.trunc(total)
    if np.isnan(rest):
        rest = np.float32(0.0)
    elif rest < 0 or (rest == 0 and np.signbit(rest)):
        rest = np.float32(0.0)
    return fcvtzu(total), bits(rest)


def check_table():
    table = load(ROOT / 'recomp/patches-v111.txt', [('main', BASE, SIZE)])
    names = [s[0] for s in table.sets]
    assert table.sets[-1] == ('orecord', 'NX2NAVI_OCARINA_TIMING', 1), table.sets[-1]
    key = names.index('orecord')
    mine = sorted(off for off, alts in table.sites['main'].items() if any(k == key for k, _, _ in alts))
    assert mine == list(SITES), [hex(s) for s in mine]
    resolved = table.for_module('main', BASE)
    for site in SITES:
        alts = table.sites['main'][site]
        assert len(alts) == 1, (hex(site), alts)
        k, words, original = alts[0]
        assert k == key and original == GAME and words == WORDS, (hex(site), words, hex(original))
        (_, real), = resolved[BASE + site]
        adrp = real[0]
        imm = ((adrp >> 29) & 3) | ((adrp >> 5) & 0x7ffff) << 2
        imm -= (imm & 1 << 20) << 1
        target = ((BASE + site) & ~0xfff) + (imm << 12)
        assert adrp & 0x9f00001f == 0x90000008 and target == table.page, (hex(site), hex(adrp), hex(target))
    # No other set's word loads or stores the fraction's bytes through an unsigned offset.
    for off, alts in table.sites['main'].items():
        for k, words, _ in alts:
            if k == key or not any(isinstance(w, tuple) for w in words):
                continue
            for w in words:
                if isinstance(w, tuple) or w & 0x3b000000 != 0x39000000:
                    continue
                scale = w >> 30
                if (w >> 26) & 1 and (w >> 22) & 2:
                    scale = 4
                start = ((w >> 10) & 0xfff) << scale
                overlap = start < FRACTION + 4 and start + (1 << scale) > FRACTION
                assert not overlap, (names[k], hex(off), hex(w))
    return table


def build(cc, work):
    lift = Lifter(0, simd='c')
    export = '__declspec(dllexport) ' if os.name == 'nt' else ''
    # Each function sets the host's floating-point mode from c->fpcr as the runtime does on entry to guest code
    # (runtime/cpu.h rt_fpcr_sync) and puts Python's back after the words.
    out = [f'// {GENERATED_MARKER}: the set orecord\'s words lifted by tests/ocarina_timing_check.py.',
           '#include "cpu.h"', '',
           '#if defined(__x86_64__)',
           '#define ENTER unsigned saved = __builtin_ia32_stmxcsr(); rt_fpcr_sync(c);',
           '#define LEAVE __builtin_ia32_ldmxcsr(saved);',
           '#else',
           '#define ENTER',
           '#define LEAVE',
           '#endif', '']
    for i, site in enumerate(SITES):
        body = []
        for j, w in enumerate(WORDS[1:], 1):   # x8 is the page: the caller sets it as the adrp would
            insn = lift.lift(w, BASE + site + 4 * j)
            assert insn.kind == 'op', (hex(w), insn.kind)
            body.append('    ' + insn.code)
        out.append(f'{export}void orecord_{i}(Cpu *c) {{\n    ENTER\n' + '\n'.join(body) + '\n    LEAVE\n}\n')
    insn = lift.lift(GAME, BASE + SITES[0])
    out.append(f'{export}void game_word(Cpu *c) {{\n    ENTER\n    {insn.code}\n    LEAVE\n}}\n')
    source = work / 'orecord.c'
    library = work / ('orecord.dll' if os.name == 'nt' else 'orecord.so')
    source.write_text('\n'.join(out))
    env = dict(os.environ)
    if os.name == 'nt' and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    subprocess.run([cc, '-shared', '-O2', '-fno-strict-aliasing', '-I', str(ROOT / 'runtime'),
                    *([] if os.name == 'nt' else ['-fPIC']), str(source), '-o', str(library)], check=True, env=env)
    if os.name == 'nt' and MSYS_BIN.exists() and hasattr(os, 'add_dll_directory'):
        os.add_dll_directory(str(MSYS_BIN))
    dll = ctypes.CDLL(str(library))
    fns = []
    for name in [f'orecord_{i}' for i in range(len(SITES))] + ['game_word']:
        fn = getattr(dll, name)
        fn.argtypes = [ctypes.c_void_p]
        fn.restype = None
        fns.append(fn)
    return fns[:-1], fns[-1]


class Harness:
    """A Cpu and a page buffer for the lifted words; x8 is set to the page before each call."""

    def __init__(self, sites, game):
        self.sites, self.game = sites, game
        self.raw = ctypes.create_string_buffer(CPU_SIZE + 16)
        self.cpu = (ctypes.addressof(self.raw) + 15) & ~15
        self.page = ctypes.create_string_buffer(4096)

    def set_v(self, n, value):
        ctypes.memmove(self.cpu + V + 16 * n, value.to_bytes(16, 'little'), 16)

    def get_v(self, n):
        return int.from_bytes(ctypes.string_at(self.cpu + V + 16 * n, 16), 'little')

    def x8(self):
        return struct.unpack('<Q', ctypes.string_at(self.cpu + X8, 8))[0]

    def fraction(self):
        return struct.unpack_from('<I', self.page.raw, FRACTION)[0]

    def set_fraction(self, b):
        struct.pack_into('<I', self.page, FRACTION, b)

    def run(self, step_bits, site=0, game=False):
        """One call; returns w8. The Cpu changes only in x8, v16-v18 and FPSR, and the page only at +264."""
        self.set_v(0, step_bits)
        if not game:
            ctypes.memmove(self.cpu + X8, struct.pack('<Q', ctypes.addressof(self.page)), 8)
        before = ctypes.string_at(self.cpu, CPU_SIZE)
        page = self.page.raw
        (self.game if game else self.sites[site])(self.cpu)
        after = ctypes.string_at(self.cpu, CPU_SIZE)
        keep = [(0, X8), (X8 + 8, FPSR), (FPSR + 8, V + 16 * 16), (V + 19 * 16, CPU_SIZE)]
        for lo, hi in keep:
            assert before[lo:hi] == after[lo:hi], ('Cpu bytes changed', lo, hi)
        if game:
            assert self.page.raw == page
        else:
            assert self.page.raw[:FRACTION] == page[:FRACTION] and self.page.raw[FRACTION + 4:] == page[FRACTION + 4:]
        w8 = self.x8()
        assert w8 >> 32 == 0, hex(w8)
        return w8


def check_lifted(h):
    rng = random.Random(264)
    # Whole steps: the game's count, and the fraction stays +0.0.
    for step, want in ((1.0, 1), (2.0, 2)):
        h.set_fraction(0)
        for _ in range(1000):
            h.set_v(16, rng.getrandbits(128))
            assert h.run(bits(step)) == want and h.run(bits(step), game=True) == want
            assert h.fraction() == 0, hex(h.fraction())
    # The limits' steps: 3600 frames a minute, each update the floor or the ceiling of the step.
    patterns = {120: [0, 1], 240: [0, 0, 0, 1]}
    for hz in (40, 45, 90, 120, 144, 165, 240):
        step = bits(np.float32(60.0 / hz))
        h.set_fraction(0)
        lo, hi = int(np.floor(f32(step))), int(np.ceil(f32(step)))
        game = h.run(step, game=True)
        assert game == (1 if hz < 60 else 0), (hz, game)
        seq, fraction = [], 0
        for k in range(60 * hz):
            w = h.run(step, site=k % len(SITES))
            want, fraction = model(f32(step), f32(fraction))
            assert (w, h.fraction()) == (want, fraction), (hz, k, w, want, hex(h.fraction()), hex(fraction))
            assert lo <= w <= hi, (hz, k, w)
            seq.append(w)
        assert abs(sum(seq) - 3600) <= 1, (hz, sum(seq))
        if hz in patterns:
            p = patterns[hz]
            assert seq == p * (len(seq) // len(p)), (hz, seq[:16])
        if hz == 144:   # 5 in every 12 updates; the float step is a little under 5/12, so the first 12 count 4
            blocks = [sum(seq[i:i + 12]) for i in range(0, len(seq), 12)]
            assert blocks[0] in (4, 5) and all(b == 5 for b in blocks[1:]), (hz, sorted(set(blocks)))
    # A carried fraction: 0.75 + 0.5 gives one frame and leaves 0.25.
    h.set_fraction(bits(0.75))
    assert h.run(bits(0.5)) == 1 and h.fraction() == bits(0.25)
    # Steps the game never gives: the game's count, a fraction in [0, 1), and the next whole step counts one.
    specials = [0x7fc00000, 0xffc00000, bits(-0.5), 0x80000000, 0, 0x7f800000, 0xff800000, bits(1e10), bits(-1e10)]
    for start in (0, bits(0.75)):
        for s in specials:
            h.set_fraction(start)
            game = h.run(s, game=True)
            assert h.run(s) == game == fcvtzu(f32(s)), (hex(start), hex(s), game)
            f = f32(h.fraction())
            assert not np.isnan(f) and 0 <= f < 1 and not np.signbit(f), (hex(start), hex(s), hex(h.fraction()))
            if start == 0 and s not in (0, 0x80000000, bits(1e10)):
                assert h.fraction() == 0, (hex(s), hex(h.fraction()))
            assert h.run(bits(1.0)) == 1
    # v0 keeps the step, its upper lanes too.
    h.set_fraction(0)
    v0 = (rng.getrandbits(96) << 32) | bits(0.25)
    h.set_v(0, v0)
    ctypes.memmove(h.cpu + X8, struct.pack('<Q', ctypes.addressof(h.page)), 8)
    h.sites[0](h.cpu)
    assert h.get_v(0) == v0
    return 60 * (40 + 45 + 90 + 120 + 144 + 165 + 240)


def check_unicorn(h, table, count):
    page = table.page
    resolved = table.for_module('main', BASE)
    emu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
    emu.ctl_set_cpu_model(arm.UC_CPU_ARM64_A57)
    emu.reg_write(arm.UC_ARM64_REG_CPACR_EL1, 0x300000)   # FP and SIMD on
    code_lo, code_hi = (BASE + min(SITES)) & ~0xfff, (BASE + max(SITES) + 0x1000) & ~0xfff
    emu.mem_map(code_lo, code_hi - code_lo)
    emu.mem_map(page, 0x1000)
    for site in SITES:
        (_, words), = resolved[BASE + site]
        emu.mem_write(BASE + site, struct.pack('<9I', *words))
    xregs = [getattr(arm, f'UC_ARM64_REG_X{i}') for i in range(31)]
    qregs = [getattr(arm, f'UC_ARM64_REG_Q{i}') for i in range(32)]
    rng = random.Random(0x265fc)
    specials = [0x7fc00000, 0xffc00000, bits(-0.5), 0x80000000, 0, 0x7f800000, 0xff800000, bits(1e10), 0x00000001,
                0x7f7fffff, bits(1.0), bits(2.0), bits(0.5), bits(np.float32(60.0 / 144))]
    cases = [(s, f) for s in specials for f in (0, bits(0.75), bits(np.nextafter(np.float32(1), np.float32(0))))]
    cases += [(bits(rng.uniform(0, 4)), bits(rng.uniform(0, 1))) for _ in range(count)]
    for n, (step, fraction) in enumerate(cases):
        if f32(fraction) >= 1:
            fraction = 0
        i = n % len(SITES)
        x = [rng.getrandbits(64) for _ in range(31)]
        v = [rng.getrandbits(128) for _ in range(32)]
        v[0] = (v[0] & ~0xffffffff) | step
        nzcv = rng.randrange(16) << 28
        fpcr = rng.choice([0, 0, 0, 1 << 24, 1 << 25, 3 << 24])
        image = bytearray(rng.getrandbits(8) for _ in range(16)) * 256
        struct.pack_into('<I', image, FRACTION, fraction)
        # Unicorn: the nine words from the site's address.
        emu.mem_write(page, bytes(image))
        for reg, value in zip(xregs, x):
            emu.reg_write(reg, value)
        for reg, value in zip(qregs, v):
            emu.reg_write(reg, value)
        emu.reg_write(arm.UC_ARM64_REG_NZCV, nzcv)
        emu.reg_write(arm.UC_ARM64_REG_FPCR, fpcr)
        emu.emu_start(BASE + SITES[i], BASE + SITES[i] + 36)
        # The lifted C: x8 set to the page buffer as the adrp sets it to the page.
        x_c = list(x)
        x_c[8] = ctypes.addressof(h.page)
        cpu = struct.pack('<31Q', *x_c) + bytes(8 + 8 + 8) + struct.pack('<Q', nzcv)
        ctypes.memmove(h.cpu, cpu + bytes(CPU_SIZE - len(cpu)), CPU_SIZE)
        ctypes.memmove(h.cpu + FPCR, struct.pack('<Q', fpcr), 8)
        for k, value in enumerate(v):
            h.set_v(k, value)
        ctypes.memmove(ctypes.addressof(h.page), bytes(image), len(image))
        h.sites[i](h.cpu)
        want_x = [emu.reg_read(reg) for reg in xregs]
        have_x = list(struct.unpack('<31Q', ctypes.string_at(h.cpu, 31 * 8)))
        assert have_x == want_x, (hex(step), hex(fraction), [(k, hex(a), hex(b)) for k, (a, b) in
                                                             enumerate(zip(have_x, want_x)) if a != b])
        for k, reg in enumerate(qregs):
            assert h.get_v(k) == emu.reg_read(reg), (hex(step), hex(fraction), hex(fpcr), k, hex(h.get_v(k)),
                                                    hex(emu.reg_read(reg)))
        for k in (16, 17, 18):
            assert h.get_v(k) >> 32 == 0, (k, hex(h.get_v(k)))
        assert emu.reg_read(arm.UC_ARM64_REG_NZCV) & 0xf0000000 == nzcv
        assert struct.unpack('<Q', ctypes.string_at(h.cpu + NZCV, 8))[0] == nzcv
        mem = bytes(emu.mem_read(page, 0x1000))
        assert h.page.raw == mem, (hex(step), hex(fraction))
        assert mem[:FRACTION] == bytes(image[:FRACTION]) and mem[FRACTION + 4:] == bytes(image[FRACTION + 4:])
        if fpcr == 0:
            assert (want_x[8], struct.unpack_from('<I', mem, FRACTION)[0]) == model(f32(step), f32(fraction)), \
                (hex(step), hex(fraction))
    return len(cases)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--cc', default=os.environ.get('CC'))
    parser.add_argument('--count', type=int, default=10000, help='random steps compared with Unicorn')
    args = parser.parse_args()
    table = check_table()
    work = ROOT / 'build/test/ocarina_timing'
    work.mkdir(parents=True, exist_ok=True)
    sites, game = build(compiler(args.cc), work)
    h = Harness(sites, game)
    calls = check_lifted(h)
    compared = check_unicorn(h, table, args.count)
    print(f'Ocarina timing: the set orecord\'s four sites and words; {calls} lifted calls at the 40 to 240 fps steps '
          f'count 3600 frames a minute; {compared} C/Unicorn comparisons agree')


if __name__ == '__main__':
    main()
