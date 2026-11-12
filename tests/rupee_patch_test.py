"""Verify the versioned rupee-refill patch without game files."""
import sys
import os
import random
import shutil
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))
import patches

BASE = 0x7000000000
SITE = 0x5efe0
ORIGINAL = 0x0b080129
REFILL = 0x2a1403e9
MODULES = [('main', BASE, 0x4ea0000), ('subsdk0', 0x7005000000, 0xe0000), ('sdk', 0x7005200000, 0x1080000)]


def definition():
    table = patches.load(ROOT / 'recomp/patches-v111.txt', MODULES)
    matches = [(i, item) for i, item in enumerate(table.sets) if item[0] == 'rupees_refill']
    if len(matches) != 1:
        raise AssertionError('Expected one versioned rupees_refill patch set')
    index, item = matches[0]
    return table, index, item


class RupeePatchTest(unittest.TestCase):
    def test_supported_patch_is_off_and_preserves_engine_limit_register(self):
        table, index, item = definition()
        self.assertEqual(item, ('rupees_refill', 'NX2NAVI_CHEAT_RUPEES_REFILL', 0))
        sites = [(name, offset, words, original)
                 for name, entries in table.sites.items()
                 for offset, alternatives in entries.items()
                 for selected, words, original in alternatives if selected == index]
        self.assertEqual(sites, [('main', SITE, [REFILL], ORIGINAL)])

    def test_other_versions_do_not_advertise_this_hook(self):
        for version in ('v110', 'v121', 'v143'):
            text = (ROOT / f'recomp/patches-{version}.txt').read_text()
            self.assertNotIn('rupees_refill', text)

    def test_expected_original_word_is_checked(self):
        table, index, _ = definition()
        selected = patches.Table(table.sets, {'main': {SITE: table.sites['main'][SITE]}}, table.page)
        class Mod:
            base = BASE
            replacement = ORIGINAL
            def in_text(self, address):
                return address == BASE + SITE
            def word(self, address):
                return self.replacement
        mod = Mod()
        patches.check(selected, mod, 'main')
        mod.replacement = 0xd503201f
        with self.assertRaisesRegex(patches.PatchError, 'patch expects'):
            patches.check(selected, mod, 'main')

    def test_arm_patch_changes_only_the_result_register(self):
        try:
            import unicorn
            from unicorn import arm64_const as arm
        except ImportError:
            self.skipTest('Unicorn is required for the ARM reference')
        table, index, _ = definition()
        words = table.for_module('main', BASE)[BASE + SITE]
        opcode = next(code for selected, code in words if selected == index)
        self.assertEqual(len(opcode), 1)
        emulator = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
        emulator.mem_map(0x10000, 0x1000)
        emulator.mem_write(0x10000, struct.pack('<I', opcode[0]))
        registers = [getattr(arm, f'UC_ARM64_REG_X{i}') for i in range(31)]
        rng = random.Random(111)
        for _ in range(128):
            initial = [rng.getrandbits(64) for _ in registers]
            initial[8] = rng.randrange(0, 10001)
            initial[9] = rng.randrange(-10000, 10001) & 0xffffffff
            initial[20] = rng.choice((0, 1, 200, 500, 1000, 9999, 0x7fffffff))
            flags = rng.randrange(16) << 28
            for register, value in zip(registers, initial):
                emulator.reg_write(register, value)
            emulator.reg_write(arm.UC_ARM64_REG_SP, 0x12345000)
            emulator.reg_write(arm.UC_ARM64_REG_NZCV, flags)
            emulator.emu_start(0x10000, 0x10004, count=1)
            expected = initial.copy()
            expected[9] = initial[20]
            self.assertEqual([emulator.reg_read(r) for r in registers], expected)
            self.assertEqual(emulator.reg_read(arm.UC_ARM64_REG_NZCV), flags)
            self.assertEqual(emulator.reg_read(arm.UC_ARM64_REG_SP), 0x12345000)

    def test_real_emitter_switches_between_original_and_refill(self):
        cc = shutil.which(os.environ.get('CC', 'clang'))
        if not cc:
            self.skipTest('A native Clang compiler is required')
        import gen
        from lifter import Lifter
        table, index, _ = definition()
        entry = BASE + SITE
        class Synthetic:
            base = BASE
            lo, hi = entry, entry + 8
            lifter = Lifter(BASE, 'c')
            def lift(self, address):
                return self.lifter.lift(ORIGINAL if address == entry else 0xd65f03c0, address)
        emitter = gen.Emitter(Synthetic(), {entry}, patched=table.for_module('main', BASE))
        source = gen.chunk_text('rupee_probe', 0, emitter, [(entry, entry + 8)])
        driver = f'''
#include "cpu.h"
#include <assert.h>
uint8_t rt_patch_on[{len(table.sets)}];
volatile int g_fnprof;
void fnprof_hit(uint64_t address, uint64_t lr, uint64_t x0) {{
    (void)address; (void)lr; (void)x0; assert(0);
}}
void f_{entry:x}(Cpu *c);
int main(void) {{
    Cpu c = {{0}};
    c.x[8] = 279; c.x[9] = (uint32_t)-30; c.x[20] = 1000;
    f_{entry:x}(&c);
    assert(c.x[9] == 279 - 30 && c.x[8] == 279 && c.x[20] == 1000);
    rt_patch_on[{index}] = 1;
    c.x[9] = (uint32_t)-30;
    f_{entry:x}(&c);
    assert(c.x[9] == 1000 && c.x[8] == 279 && c.x[20] == 1000);
    rt_patch_on[{index}] = 0;
    c.x[9] = 50;
    f_{entry:x}(&c);
    assert(c.x[9] == 279 + 50);
    return 0;
}}
'''
        with tempfile.TemporaryDirectory(prefix='nx2navi-rupee-patch-') as temp:
            folder = Path(temp)
            (folder / 'emitted.c').write_text(source)
            (folder / 'driver.c').write_text(driver)
            exe = folder / ('check.exe' if os.name == 'nt' else 'check')
            subprocess.run([cc, '-std=gnu11', '-O2', '-I', str(ROOT / 'runtime'),
                            str(folder / 'emitted.c'), str(folder / 'driver.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == '__main__':
    unittest.main()
