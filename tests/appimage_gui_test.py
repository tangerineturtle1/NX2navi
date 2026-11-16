"""AppImage diagnostics without game files: python tests/appimage_gui_test.py (Tk and a display required)."""
import os
import sys
import tempfile
import time
import tkinter as tk
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'tools'))
import appimage_gui as gui


class DiagnosticsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.cache = Path(self.tmp.name)
        patch = mock.patch.object(gui, 'CACHE', self.cache)
        patch.start()
        self.addCleanup(patch.stop)
        translation = mock.patch.object(gui, 'T', gui.TEXT['en'])
        translation.start()
        self.addCleanup(translation.stop)
        self.root = tk.Tk()
        self.root.withdraw()
        self.addCleanup(self.close_root)
        self.app = gui.App(self.root, Path('/unused/AppRun'), None)

    def close_root(self):
        for callback in self.root.tk.call('after', 'info'):
            self.root.after_cancel(callback)
        self.root.destroy()

    def test_details_reads_saved_log_without_external_editor(self):
        (self.cache / 'build.log').write_text('Missing: ld.lld\n', encoding='utf-8')
        with mock.patch.dict(os.environ, {'PATH': ''}):
            self.app.details()
        window = self.app.details_window
        self.assertEqual(window.text.get('1.0', 'end-1c'), 'Missing: ld.lld\n')
        window.copy()
        window.destroy()
        self.assertEqual(self.root.clipboard_get(), 'Missing: ld.lld\n')

    def test_live_log_survives_unwritable_cache_and_repeated_details(self):
        blocked = self.cache / 'not-a-directory'
        blocked.write_text('blocked')
        with mock.patch.object(gui, 'CACHE', blocked):
            self.app.log = ['current error']
            self.app.details()
            first = self.app.details_window
            self.app.log.append('more detail')
            self.app.details()
        self.assertIs(first, self.app.details_window)
        self.assertIn('more detail', first.text.get('1.0', 'end-1c'))

    def test_missing_editor_reports_error_but_keeps_log(self):
        self.app.log = ['build error']
        self.app.details()
        window = self.app.details_window
        with mock.patch.dict(os.environ, {'PATH': ''}):
            window.open_editor()
        self.assertIn('xdg-open', window.error.get())
        self.assertIn('build error', window.text.get('1.0', 'end-1c'))

    def test_long_build_keeps_early_diagnostics(self):
        self.app.line('first diagnostic')
        for _ in range(5000):
            self.app.line('compile output')
        self.app.details()
        self.assertTrue(self.app.details_window.text.get('1.0', 'end-1c').startswith('first diagnostic\n'))

    def test_editor_exit_error_is_visible(self):
        opener = self.cache / 'xdg-open'
        opener.write_text('#!/bin/sh\necho "editor unavailable" >&2\nexit 4\n')
        opener.chmod(0o755)
        self.app.log = ['build error']
        self.app.details()
        window = self.app.details_window
        with mock.patch.dict(os.environ, {'PATH': str(self.cache)}):
            window.open_editor()
        deadline = time.monotonic() + 3
        while not window.error.get() and time.monotonic() < deadline:
            self.root.update()
            time.sleep(.02)
        self.assertIn('editor unavailable', window.error.get())

    def test_missing_dependencies_show_specific_fix_on_main_screen(self):
        self.app.cancelled, self.app.failed, self.app.missing = False, None, False
        self.app.line('Missing: ld.lld opus/opus.h')
        self.app.line('@@{"ev":"error","step":"deps","message":"missing packages"}')
        with mock.patch('platform.freedesktop_os_release', return_value={'ID': 'arch'}):
            self.app.finished(1)
        self.assertIn('ld.lld', self.app.status)
        self.assertIn('sudo pacman -S --needed lld opus', self.app.status)

    def test_dependency_advice_follows_distribution_and_deduplicates_packages(self):
        for release, command in (({'ID': 'ubuntu', 'ID_LIKE': 'debian'}, 'sudo apt install clang lld'),
                                 ({'ID': 'fedora'}, 'sudo dnf install clang lld')):
            with self.subTest(release=release), mock.patch('platform.freedesktop_os_release', return_value=release):
                self.assertIn(command, gui.dependency_message('clang clang++ ld.lld'))
        with mock.patch('platform.freedesktop_os_release', side_effect=OSError('no os-release')):
            self.assertIn('ld.lld', gui.dependency_message('ld.lld'))


if __name__ == '__main__':
    unittest.main()
