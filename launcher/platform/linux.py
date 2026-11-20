"""Linux services used by the launcher's compatibility facade (launcher/winsys.py)."""
import ctypes
import ctypes.util
import os
import shutil
import subprocess
from pathlib import Path


UDMABUF_MINIMUM_MB = 1024


def udmabuf_status(device='/dev/udmabuf', parameter='/sys/module/udmabuf/parameters/size_limit_mb',
                  which=shutil.which, read_text=None, accessible=None, os_release='/etc/os-release'):
    """Describe udmabuf without privilege; injectable paths and probes keep the check isolated in tests."""
    device, parameter = Path(device), Path(parameter)
    read_text = read_text or (lambda path: path.read_text(encoding='utf-8').strip())
    accessible = accessible or (lambda path: os.access(path, os.R_OK | os.W_OK))
    try:
        limit = int(read_text(parameter))
    except (OSError, ValueError):
        return None
    device_accessible = bool(accessible(device))
    steamos = False
    try:
        steamos = any(line.strip() == 'ID=steamos' for line in Path(os_release).read_text(encoding='utf-8').splitlines())
    except OSError:
        pass
    needs_fix = not device_accessible or limit < UDMABUF_MINIMUM_MB
    reason = ('module writable only on SteamOS' if steamos else 'pkexec not installed') if needs_fix and (
        steamos or not which('pkexec')) else ''
    offered = bool(needs_fix and not reason)
    return {'accessible': device_accessible, 'limit_mb': limit, 'needs_fix': needs_fix,
            'offered': offered, 'reason': reason, 'steamos': steamos}


def run_udmabuf_fix(script, action, run=subprocess.run):
    """Ask PolicyKit to run the single packaged script after the player confirms the requested action."""
    if action not in ('apply', 'remove'):
        raise ValueError('unknown udmabuf action')
    try:
        result = run(['pkexec', str(script), action], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.SubprocessError) as error:
        return False, str(error)[:240]
    if result.returncode:
        if result.returncode in (126, 127):
            return False, 'authorization refused or cancelled'
        detail = (result.stderr or result.stdout or f'exit status {result.returncode}').strip()
        return False, detail[:240]
    return True, (result.stdout or 'completed').strip()[:240]


def udmabuf_log_line(status):
    """One bounded diagnostic line for the launcher log at startup."""
    if status is None:
        return 'udmabuf: module absent; offer=none'
    accessible = 'yes' if status['accessible'] else 'no'
    offered = 'yes' if status['offered'] else 'no'
    reason = f" ({status['reason']})" if status['reason'] else ''
    return (f"udmabuf: accessible={accessible}; size_limit_mb={status['limit_mb']}; "
            f"offer={'offered' if offered == 'yes' else 'none'}{reason}")[:240]


def udmabuf_fix_installed(rules='/etc/udev/rules.d/70-udmabuf-uaccess.rules',
                           tmpfiles='/etc/tmpfiles.d/udmabuf.conf'):
    return Path(rules).is_file() or Path(tmpfiles).is_file()


def udmabuf_change_available(which=shutil.which, os_release='/etc/os-release'):
    try:
        steamos = any(line.strip() == 'ID=steamos'
                      for line in Path(os_release).read_text(encoding='utf-8').splitlines())
    except OSError:
        steamos = False
    return not steamos and bool(which('pkexec'))


def _sdl_library():
    """Load the kit's SDL3 first, then the system copy; return a short source label for the log."""
    bundled = next((root / 'toolchain' / 'sdl3' / 'lib' / 'libSDL3.so.0'
                    for root in Path(__file__).resolve().parents
                    if (root / 'toolchain' / 'sdl3' / 'lib' / 'libSDL3.so.0').is_file()), None)
    system = ctypes.util.find_library('SDL3') or 'libSDL3.so.0'
    candidates = ([(str(bundled), 'SDL3 from the kit')] if bundled else []) + [(system, 'SDL3 from system')]
    for candidate, source in candidates:
        if not candidate:
            continue
        try:
            return ctypes.CDLL(candidate), source
        except OSError:
            pass
    return None, 'none (SDL3 library not found)'


class Gamepad:
    """Read SDL3 GameController input, including Steam Input's virtual controller on Steam Deck."""
    BUTTONS = {11: 'up', 12: 'down', 13: 'left', 14: 'right', 0: 'a', 1: 'b', 3: 'y'}
    AXIS_LEFT_X, AXIS_LEFT_Y, AXIS_RIGHT_Y = 0, 1, 3
    INIT_GAMEPAD = 0x00002000

    def __init__(self):
        self.dll, self.description = _sdl_library()
        self.pad = None
        self.held = set()
        self.scroll = 0
        if not self.dll:
            return
        try:
            self.dll.SDL_InitSubSystem.argtypes = [ctypes.c_uint32]
            self.dll.SDL_InitSubSystem.restype = ctypes.c_bool
            self.dll.SDL_GetGamepads.argtypes = [ctypes.POINTER(ctypes.c_int)]
            self.dll.SDL_GetGamepads.restype = ctypes.POINTER(ctypes.c_int32)
            self.dll.SDL_OpenGamepad.argtypes = [ctypes.c_int32]
            self.dll.SDL_OpenGamepad.restype = ctypes.c_void_p
            self.dll.SDL_GetGamepadButton.argtypes = [ctypes.c_void_p, ctypes.c_int]
            self.dll.SDL_GetGamepadButton.restype = ctypes.c_bool
            self.dll.SDL_GetGamepadAxis.argtypes = [ctypes.c_void_p, ctypes.c_int]
            self.dll.SDL_GetGamepadAxis.restype = ctypes.c_int16
            self.dll.SDL_PumpEvents.argtypes = []
            self.dll.SDL_PumpEvents.restype = None
            self.dll.SDL_free.argtypes = [ctypes.c_void_p]
            self.dll.SDL_free.restype = None
            self.dll.SDL_CloseGamepad.argtypes = [ctypes.c_void_p]
            self.dll.SDL_CloseGamepad.restype = None
            self.dll.SDL_GamepadConnected.argtypes = [ctypes.c_void_p]
            self.dll.SDL_GamepadConnected.restype = ctypes.c_bool
            if not self.dll.SDL_InitSubSystem(self.INIT_GAMEPAD):
                self.description = 'none (SDL3 gamepad initialization failed)'
                self.dll = None
                return
            self._open_first()
        except (AttributeError, OSError):
            self.description = 'none (SDL3 gamepad API unavailable)'
            self.dll = None
            self.pad = None

    def _open_first(self):
        if not self.dll or self.pad:
            return
        count = ctypes.c_int()
        ids = self.dll.SDL_GetGamepads(ctypes.byref(count))
        if ids and count.value:
            self.pad = self.dll.SDL_OpenGamepad(ids[0])
            self.dll.SDL_free(ids)

    def poll(self, active=True):
        self.scroll = 0
        if not self.dll:
            return set()
        try:
            self.dll.SDL_PumpEvents()
            if self.pad and not self.dll.SDL_GamepadConnected(self.pad):
                self.dll.SDL_CloseGamepad(self.pad)
                self.pad = None
            self._open_first()
            down = set()
            if self.pad:
                down = {name for button, name in self.BUTTONS.items()
                        if self.dll.SDL_GetGamepadButton(self.pad, button)}
                lx = self.dll.SDL_GetGamepadAxis(self.pad, self.AXIS_LEFT_X)
                ly = self.dll.SDL_GetGamepadAxis(self.pad, self.AXIS_LEFT_Y)
                ry = self.dll.SDL_GetGamepadAxis(self.pad, self.AXIS_RIGHT_Y)
                if ly < -16000:
                    down.add('up')
                elif ly > 16000:
                    down.add('down')
                if lx < -16000:
                    down.add('left')
                elif lx > 16000:
                    down.add('right')
                if active and ry < -16000:
                    self.scroll = -1
                elif active and ry > 16000:
                    self.scroll = 1
            pressed = down - self.held if active else set()
            self.held = down
            return pressed
        except (AttributeError, OSError):
            self.held.clear()
            return set()

    def close(self):
        if self.dll and self.pad:
            self.dll.SDL_CloseGamepad(self.pad)
            self.pad = None


_inhibitor = None


def keep_awake(on, log=None):
    """Hold a systemd sleep inhibitor for the duration of a foreground build job."""
    global _inhibitor
    if not on:
        held = _inhibitor is not None
        if held:
            try:
                if _inhibitor.poll() is None:
                    _inhibitor.stdin.close()
                    _inhibitor.wait(timeout=2)
            except subprocess.TimeoutExpired:
                _inhibitor.kill()
                _inhibitor.wait()
            except (AttributeError, OSError, ValueError):
                pass
        _inhibitor = None
        if held and log:
            log('keep awake: systemd-inhibit released')
        return
    if _inhibitor and _inhibitor.poll() is None:
        return
    if _inhibitor:
        _inhibitor = None
    inhibitor = shutil.which('systemd-inhibit')
    if not inhibitor:
        if log:
            log('keep awake: systemd-inhibit unavailable')
        return
    try:
        _inhibitor = subprocess.Popen(
            [inhibitor, '--what=idle:sleep', '--who=NX2Saria', '--why=Building the game',
             '--mode=block', 'cat'],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, close_fds=True)
        if _inhibitor.poll() is None:
            if log:
                log('keep awake: systemd-inhibit held')
        else:
            _inhibitor = None
            if log:
                log('keep awake: systemd-inhibit unavailable')
    except OSError:
        _inhibitor = None
        if log:
            log('keep awake: systemd-inhibit unavailable')


def toolchain_missing(which=shutil.which, run=subprocess.run):
    """Check the compiler and headers the Linux build requires, with distro guidance and no package changes."""
    required = ('clang', 'clang++', 'ld.lld')
    missing = [name for name in required if not which(name)]
    clang = which('clang')
    if clang:
        for header in ('opus/opus.h', 'vpx/vpx_decoder.h', 'dav1d/dav1d.h', 'zstd.h'):
            probe = run([clang, '-E', '-x', 'c', '-', '-o', os.devnull], input=f'#include <{header}>\n',
                        capture_output=True, text=True)
            if probe.returncode:
                missing.append(header)
    if not missing:
        return ''
    return (f'Missing Linux build tools or headers: {", ".join(missing)}. Install the compiler and libraries, then '
            'run this again:\n'
            'Arch, CachyOS, Manjaro, SteamOS (developer mode): sudo pacman -S --needed clang lld sdl3 opus libvpx '
            'dav1d zstd vulkan-headers vulkan-icd-loader\n'
            'Fedora: sudo dnf install clang lld SDL3-devel opus-devel libvpx-devel libdav1d-devel libzstd-devel '
            'vulkan-headers vulkan-loader-devel libstdc++-static libatomic\n'
            'Debian, Ubuntu, Linux Mint: sudo apt install clang lld libopus-dev libvpx-dev libdav1d-dev libzstd-dev '
            'libvulkan-dev. SDL3 is carried by the build kit when the distribution has no SDL3 package.\n'
            'NX2Saria will not install system packages for you.')
