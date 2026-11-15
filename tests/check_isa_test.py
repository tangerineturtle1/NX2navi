"""tools/check_isa.py on the runtime files that hold instructions above x86-64-v2 behind a runtime CPU check
(runtime/kernel.c kern_ap_stub_ymm, runtime/sha256.c blocks_ni): built as tools/build_game.py --cpu v2 builds the
runtime (-march=x86-64-v2) and as players' builds do (no -march), they pass, and a function with such an instruction
and no entry in DISPATCHED still fails. Needs the MSYS2 UCRT64 clang and an objdump (skipped without them); no game
files.

Run: python tests/check_isa_test.py
"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))

import check_isa  # noqa: E402

UCRT = Path(r'C:\msys64\ucrt64\bin')
CC = next((str(c) for c in (UCRT / 'clang.exe', shutil.which('clang')) if c and Path(c).is_file()), None)
OBJDUMP = check_isa.find_objdump(str(UCRT / 'objdump.exe') if (UCRT / 'objdump.exe').is_file() else None)
FLAGS = ['-O2', '-g', '-fno-strict-aliasing', '-w']   # tools/build_game.py RT_FLAGS, without its warnings


@unittest.skipUnless(CC and OBJDUMP, 'needs clang and objdump')
class CheckIsaTest(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory()
        self.addCleanup(td.cleanup)
        self.tmp = Path(td.name)

    def compile(self, src, march, name=None):
        out = self.tmp / f'{name or Path(src).stem}{march or ""}.o'
        subprocess.run([CC, *FLAGS, *([march] if march else []), '-I', str(ROOT / 'runtime'), '-c', str(src),
                        '-o', str(out)], check=True, capture_output=True, timeout=600)
        return out

    def test_the_dispatched_functions_pass(self):
        for march in ('-march=x86-64-v2', None):
            objs = [self.compile(ROOT / 'runtime' / f, march) for f in ('kernel.c', 'sha256.c')]
            self.assertEqual(check_isa.scan(objs, OBJDUMP), {}, march)

    def test_an_undispatched_avx_function_fails(self):
        src = self.tmp / 'wide.c'
        src.write_text('void wide(void) { __asm__ volatile("vzeroupper"); }\n', encoding='utf-8')
        hits = check_isa.scan([self.compile(src, '-march=x86-64-v2')], OBJDUMP)
        self.assertEqual([f for _, f in hits], ['wide'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
