"""What every part of the launcher shares: the folder layout, the settings and the log (data/logs/launcher.log,
the run before it in launcher.1.log)."""
import os
import threading
import time

from .paths import Layout
from .settings import Settings

LOG_LIMIT = 4 << 20


class Context:
    def __init__(self, layout=None, echo=False):
        self.layout = (layout or Layout()).ensure()
        self.settings = Settings(self.layout.settings)
        self.version = self.layout.version
        self.echo = echo
        self._lock = threading.Lock()
        path = self.layout.logs / 'launcher.log'
        try:
            if path.is_file() and path.stat().st_size > LOG_LIMIT:
                os.replace(path, path.with_name('launcher.1.log'))
        except OSError:
            pass
        try:
            self._file = open(path, 'a', encoding='utf-8', errors='replace')
        except OSError:
            self._file = None
        self.log(f'--- NX2navi launcher {self.version} ({"checkout" if self.layout.dev else "release"}) in '
                 f'{self.layout.root}')
        from .toolchain import zstd_missing
        self.zstd_missing = zstd_missing(self.layout)
        if self.zstd_missing:
            self.log(self.zstd_missing)
        if self.settings.unreadable:
            self.log(f'{self.layout.settings} is not valid JSON: the settings start from their defaults; the file as '
                     'it was is kept as launcher.json.bad')

    def log(self, line):
        """One line (or several) into launcher.log. A log that cannot be written (a full disk, a file closed under
        it) loses the line and nothing else: the callers log from the build's reader thread and from the window's
        timers, where an exception would stop the build's output from being read or the timer from running."""
        stamp = time.strftime('%H:%M:%S')
        with self._lock:
            if self._file:
                try:
                    for part in str(line).splitlines() or ['']:
                        self._file.write(f'{stamp} {part}\n')
                    self._file.flush()
                except (OSError, ValueError):   # ValueError: the file was closed
                    pass
        if self.echo:
            print(line, flush=True)

    def log_tail(self, n=80):
        try:
            with open(self.layout.logs / 'launcher.log', encoding='utf-8', errors='replace') as f:
                return f.readlines()[-n:]
        except OSError:
            return []
