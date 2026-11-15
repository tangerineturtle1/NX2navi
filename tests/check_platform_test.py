"""tools/check_platform.py on made-up runtime/ trees: a Win32 use behind a Windows-only preprocessor branch passes, an
unguarded one fails, and the repository's own runtime/ passes. No game files; stdlib only.

Run: python tests/check_platform_test.py
"""
import contextlib
import io
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))

import check_platform  # noqa: E402


class CheckPlatformTest(unittest.TestCase):
    def check(self, files, runtime=None):
        """(exit code, output) of check_platform.py on a runtime/ folder holding files ({name: text}), or on
        runtime."""
        with tempfile.TemporaryDirectory() as d:
            if runtime is None:
                runtime = Path(d) / 'runtime'
                runtime.mkdir()
                for name, text in files.items():
                    (runtime / name).write_text(text, encoding='utf-8')
            out = io.StringIO()
            with mock.patch.object(sys, 'argv', ['check_platform.py', '--runtime', str(runtime)]), \
                    contextlib.redirect_stdout(out):
                code = check_platform.main()
        return code, out.getvalue()

    def test_windows_only_branches_pass(self):
        code, out = self.check({
            'a.c': '#ifdef _WIN32\n#include <windows.h>\n#endif\nint a;\n',
            'b.c': '#ifndef _WIN32\n#include <dlfcn.h>\nvoid f(int h) { (void)h; }\n#else\n'
                   'void f(HANDLE h) { CloseHandle(h); }\n#endif\n',
            'c.cpp': '#if defined(_WIN32)   // the DLL\n#  if X\nstatic DWORD d;\n#  endif\n'
                     'static void g() { FreeLibrary(0); }\n#elif Y\nint y;\n#else\nint z;\n#endif\n',
            'd.h': '#if !defined(_WIN32)\nint p;\n#elif Z\nint q;\n#else\n#include <windows.h>\n#endif\n',
            'e.c': '/* CloseHandle in a comment */\nconst char *s = "GetLastError";\n',
        })
        self.assertEqual(code, 0, out)
        self.assertNotIn('FAIL', out)

    def test_unguarded_uses_fail(self):
        code, out = self.check({
            'a.c': '#include <windows.h>\nint a;\n',
            'b.c': '#ifndef _WIN32\nint b;\n#endif\nvoid f(void *h) { CloseHandle(h); }\n',
            'c.c': '#ifdef _WIN32\nint c;\n#else\nstatic DWORD d;\n#endif\n',
            'd.c': '#ifdef _WIN32\nint d;\n#elif 1\nvoid g(void) { Sleep(1); }\n#endif\n',
            'e.c': '#if defined(_WIN32) && X\nvoid h(void) { Sleep(1); }\n#endif\n',   # scanned: never let through
            'f.c': '#ifdef _WIN32\n#endif\n\n#include <windows.h>\n',
            'ok.c': '#ifdef _WIN32\nvoid f(void *h) { CloseHandle(h); }\n#endif\n',
        })
        self.assertEqual(code, 1)
        for name in 'abcdef':
            self.assertIn(f'FAIL runtime/{name}.c', out)
        self.assertNotIn('runtime/ok.c', out)

    def test_the_runtime_passes(self):
        code, out = self.check({}, ROOT / 'runtime')
        self.assertEqual(code, 0, out)


if __name__ == '__main__':
    unittest.main(verbosity=2)
