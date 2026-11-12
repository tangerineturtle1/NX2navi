"""What the launcher needs from the operating system: crisp drawing on scaled displays, the game's fonts for this
process only, starting the game and the stub, shortcuts, the uninstall entry and a gamepad. Windows first; the
other branches exist so the window can be developed and tested on Linux."""
import ctypes
import os
import subprocess
import sys
from pathlib import Path

CREATE_NO_WINDOW = 0x08000000
DETACHED_PROCESS = 0x00000008
CREATE_NEW_PROCESS_GROUP = 0x00000200
UNINSTALL_KEY = r'Software\Microsoft\Windows\CurrentVersion\Uninstall\NX2navi'


def dpi_aware():
    """Draw at the display's real resolution instead of letting Windows stretch a 96-dpi window (blurry)."""
    if os.name != 'nt':
        return
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except (AttributeError, OSError):
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except (AttributeError, OSError):
            pass


def app_id():
    """Group the window under the launcher's own taskbar icon rather than python's."""
    if os.name == 'nt':
        try:
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID('NX2navi.Launcher')
        except (AttributeError, OSError):
            pass


GWL_STYLE, GWL_EXSTYLE = -16, -20
WS_MINIMIZEBOX, WS_SYSMENU = 0x00020000, 0x00080000
WS_EX_APPWINDOW, WS_EX_TOOLWINDOW = 0x00040000, 0x00000080


def _hwnd(root):
    """The Win32 window of a Tk toplevel (the wrapper around Tk's child window)."""
    user32 = ctypes.windll.user32
    user32.GetParent.restype = ctypes.c_void_p
    user32.GetParent.argtypes = [ctypes.c_void_p]
    return user32.GetParent(root.winfo_id()) or root.winfo_id()


def _post(hwnd, msg, wparam, lparam):
    """Queue a message for the window instead of sending it. A sent message runs the window's handler inside this
    ctypes call, which has released Python's lock; when that handler dispatches Tk events (the move loop of a title
    bar drag does), Tk calls back into Python without the lock and Python aborts. A posted message is handled by Tk's
    own event loop, where calling back into Python is allowed."""
    user32 = ctypes.windll.user32
    user32.PostMessageW.restype = ctypes.c_int
    user32.PostMessageW.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_size_t, ctypes.c_ssize_t]
    return user32.PostMessageW(hwnd, msg, wparam, lparam)


def frameless(root):
    """Windows: drop the system title bar (the window draws its own), but keep a taskbar button, Alt+Tab, minimise
    from the taskbar and Alt+F4. False elsewhere, where the window keeps its frame."""
    if os.name != 'nt':
        return False
    try:
        root.overrideredirect(True)
        root.update_idletasks()
        user32 = ctypes.windll.user32
        get, put = user32.GetWindowLongPtrW, user32.SetWindowLongPtrW
        get.restype, get.argtypes = ctypes.c_ssize_t, [ctypes.c_void_p, ctypes.c_int]
        put.restype, put.argtypes = ctypes.c_ssize_t, [ctypes.c_void_p, ctypes.c_int, ctypes.c_ssize_t]
        hwnd = _hwnd(root)
        put(hwnd, GWL_EXSTYLE, (get(hwnd, GWL_EXSTYLE) & ~WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW)
        put(hwnd, GWL_STYLE, get(hwnd, GWL_STYLE) | WS_MINIMIZEBOX | WS_SYSMENU)
        root.withdraw()            # the taskbar takes a changed window style only when the window is shown again
        root.after(10, root.deiconify)
        return True
    except (AttributeError, OSError, Exception):
        return False


def minimize(root):
    """Tk refuses to iconify a window without a frame, so ask Windows; asynchronously, for the reason in _post."""
    if os.name == 'nt':
        ctypes.windll.user32.ShowWindowAsync(ctypes.c_void_p(_hwnd(root)), 6)   # SW_MINIMIZE
    else:
        root.iconify()


def start_drag(root):
    """Windows: let the system move the window from the mouse's current press (as on a title bar, with snapping).
    False elsewhere: the caller moves the window itself."""
    if os.name != 'nt':
        return False
    try:
        ctypes.windll.user32.ReleaseCapture()
        return bool(_post(_hwnd(root), 0x00A1, 2, 0))   # WM_NCLBUTTONDOWN, HTCAPTION
    except (AttributeError, OSError):
        return False


_ICONS = []


def set_window_icon(root, ico):
    """Windows: the window's and the taskbar button's icon, straight from an .ico (the large and the small size)."""
    if os.name != 'nt':
        return False
    try:
        user32 = ctypes.windll.user32
        user32.LoadImageW.restype = ctypes.c_void_p
        user32.LoadImageW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint, ctypes.c_int, ctypes.c_int,
                                      ctypes.c_uint]
        hwnd = _hwnd(root)
        for which, (mx, my) in ((1, (11, 12)), (0, (49, 50))):   # ICON_BIG: SM_CXICON; ICON_SMALL: SM_CXSMICON
            h = user32.LoadImageW(None, str(ico), 1, user32.GetSystemMetrics(mx), user32.GetSystemMetrics(my),
                                  0x10)   # IMAGE_ICON, LR_LOADFROMFILE
            if h:
                _ICONS.append(h)
                _post(hwnd, 0x0080, which, h)   # WM_SETICON
        return True
    except (AttributeError, OSError):
        return False


def register_font(path):
    """Make a font file usable by this process only (Tk then finds it by family name). True on success."""
    path = str(path)
    if os.name == 'nt':
        try:
            return ctypes.windll.gdi32.AddFontResourceExW(ctypes.c_wchar_p(path), 0x10, None) > 0   # FR_PRIVATE
        except (AttributeError, OSError):
            return False
    try:
        fc = ctypes.CDLL('libfontconfig.so.1')
        fc.FcConfigAppFontAddFile.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
        return bool(fc.FcConfigAppFontAddFile(None, path.encode()))
    except OSError:
        return False


def unregister_font(path):
    '''Give up a font file register_font added, so the file can be written again: Windows keeps a registered font
    file open, and writing it then fails with "Permission denied".'''
    if os.name != 'nt':
        return
    try:
        ctypes.windll.gdi32.RemoveFontResourceExW(ctypes.c_wchar_p(str(path)), 0x10, None)   # FR_PRIVATE
    except (AttributeError, OSError):
        pass


def open_path(path):
    path = str(path)
    if os.name == 'nt':
        os.startfile(path)   # noqa: S606 — opens a folder in Explorer
    else:
        subprocess.Popen(['xdg-open', path], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def reveal(path):
    """Show a file in its folder, selected in Explorer (a file to drag into a bug report); elsewhere open the folder."""
    path = Path(path)
    if os.name == 'nt':
        subprocess.Popen(['explorer', f'/select,{path}'])   # noqa: S603, S607
    else:
        open_path(path.parent)


KNOWN_FOLDERS = {'downloads': '{374DE290-123F-4565-9164-39C4925E467B}',
                 'desktop': '{B4BFCC3A-DB2C-424C-B029-7FE99A87C641}',
                 'pictures': '{33E28130-4E1E-4676-835A-98395C3BC3BB}'}


def known_folder(name):
    """Windows: the folder the shell keeps for name ('downloads', 'desktop', 'pictures'), also when the player moved it to another
    drive; None elsewhere or when the shell names none."""
    if os.name != 'nt' or name not in KNOWN_FOLDERS:
        return None
    try:
        from ctypes import wintypes

        class GUID(ctypes.Structure):
            _fields_ = [('d1', wintypes.DWORD), ('d2', wintypes.WORD), ('d3', wintypes.WORD),
                        ('d4', ctypes.c_ubyte * 8)]
        ole32, shell32 = ctypes.WinDLL('ole32'), ctypes.WinDLL('shell32')   # private handles: argtypes stay here
        ole32.CLSIDFromString.argtypes = (wintypes.LPCWSTR, ctypes.POINTER(GUID))
        ole32.CoTaskMemFree.argtypes = (ctypes.c_void_p,)
        shell32.SHGetKnownFolderPath.argtypes = (ctypes.POINTER(GUID), wintypes.DWORD, wintypes.HANDLE,
                                                 ctypes.POINTER(ctypes.c_void_p))
        g, out = GUID(), ctypes.c_void_p()
        if ole32.CLSIDFromString(KNOWN_FOLDERS[name], ctypes.byref(g)) != 0:
            return None
        if shell32.SHGetKnownFolderPath(ctypes.byref(g), 0, None, ctypes.byref(out)) != 0:
            return None
        try:
            return Path(ctypes.wstring_at(out.value))
        finally:
            ole32.CoTaskMemFree(out)
    except (AttributeError, OSError):
        return None


def start_game(exe):
    """Start the game from its own folder; returns the process to watch. ocarina.exe is a console program and the
    launcher has no console, so without CREATE_NO_WINDOW Windows opened an empty console window beside the game,
    titled with the exe's path. With it the game still has a console (hidden), which its restarts inherit."""
    exe = Path(exe)
    flags = {'creationflags': CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP} if os.name == 'nt' \
        else {'start_new_session': True}
    return subprocess.Popen([str(exe)], cwd=str(exe.parent), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, close_fds=True, **flags)


def start_detached(cmd, cwd=None):
    flags = {'creationflags': DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP} if os.name == 'nt' \
        else {'start_new_session': True}
    subprocess.Popen(cmd, cwd=cwd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     close_fds=True, **flags)


def restart(layout, args=()):
    """Start the launcher again through its stub (which swaps in a staged Python and starts app/current.txt's
    version). False in a checkout, which has no stub."""
    if not layout.stub.is_file():
        return False
    start_detached([str(layout.stub), *args], cwd=str(layout.root))
    return True


def stub_run(layout, args, timeout=30):
    """Run the stub for one of its own commands (shortcuts); True when it succeeded."""
    if not layout.stub.is_file():
        return False
    flags = {'creationflags': CREATE_NO_WINDOW} if os.name == 'nt' else {}
    try:
        return subprocess.run([str(layout.stub), *args], timeout=timeout, capture_output=True, **flags).returncode == 0
    except (OSError, subprocess.TimeoutExpired):
        return False


def desktop_shortcut(layout, enable):
    return stub_run(layout, ['--desktop-shortcut', 'on' if enable else 'off'])


def keep_awake(on):
    if os.name != 'nt':
        return
    try:
        ES_CONTINUOUS, ES_SYSTEM_REQUIRED = 0x80000000, 0x00000001
        ctypes.windll.kernel32.SetThreadExecutionState(ES_CONTINUOUS | (ES_SYSTEM_REQUIRED if on else 0))
    except (AttributeError, OSError):
        pass


def background_thread():
    """Windows: put the calling thread in background mode (SetThreadPriority THREAD_MODE_BACKGROUND_BEGIN), which
    lowers its processor, disk and memory priority, so work done beside the game takes only what the game leaves.
    False where that is not possible."""
    if os.name != 'nt':
        return False
    try:
        from ctypes import wintypes
        k = ctypes.WinDLL('kernel32')   # a private handle: the argument types set here touch no other caller
        k.GetCurrentThread.restype = wintypes.HANDLE
        k.SetThreadPriority.argtypes = (wintypes.HANDLE, ctypes.c_int)
        return bool(k.SetThreadPriority(k.GetCurrentThread(), 0x00010000))
    except (AttributeError, OSError):
        return False


# What the launcher puts in its folder (launcher/paths.py, the stub's python.next and python.old-<n>). Uninstall deletes
# only these: the default game folder is <root>\game, and a folder given to the setup with --dest may hold other things.
OWNED = ('NX2navi.exe', 'app', 'python', 'python.next', 'pydeps', 'data', 'toolchain')


def uninstall_plan(layout, remove_game=''):
    """(paths to delete with all they hold, folders to delete only when that left them empty), in order. A kept game
    folder stays whether or not it lies inside the launcher's folder."""
    root = Path(layout.root)
    gone = [Path(remove_game)] if remove_game else []
    gone += [root / n for n in OWNED]
    try:
        gone += sorted(p for p in root.glob('python.old-*'))
    except OSError:
        pass
    empty = [root]
    if Path(layout.toolchain).parent != root:   # the toolchain went to C:\NX2navi (launcher/paths.py ansi_safe)
        gone.append(Path(layout.toolchain))
        empty.append(Path(layout.toolchain).parent)
    return gone, empty


def uninstall_script(gone, empty):
    """The cmd.exe line that deletes them once the launcher has exited: a file with del, a folder with rmdir /s, the
    folders that must be empty with a plain rmdir (which leaves a folder that still holds something). A path holding
    % is left out: cmd.exe expands %NAME% even inside quotes, so a folder named C:\\%HOMEPATH% would delete another."""
    steps = ['ping -n 3 127.0.0.1 >nul']
    for p in gone:
        if '%' not in str(p):
            steps.append(f'(if exist "{p}\\*" (rmdir /s /q "{p}") else if exist "{p}" (del /f /q "{p}"))')
    steps += [f'rmdir "{p}" 2>nul' for p in empty if '%' not in str(p)]
    return ' & '.join(steps)


def uninstall(layout, remove_game='', log=print):
    """Remove the shortcuts, the uninstall entry and, once this process has exited, what the launcher put in its folder
    (and the game folder when remove_game names it). A kept game folder and its saves stay, also when it lies inside
    the launcher's folder."""
    stub_run(layout, ['--shortcuts', 'off'])
    gone, empty = uninstall_plan(layout, remove_game)
    if os.name == 'nt':
        try:
            import winreg
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, UNINSTALL_KEY)
        except OSError:
            pass
        # one command line as cmd.exe reads it: a list would have its quotes escaped as \", which cmd.exe does not
        # understand, and no rmdir would find its folder
        kept = [str(p) for p in list(gone) + list(empty) if '%' in str(p) and os.path.exists(p)]
        if kept:
            log('uninstall: remove these yourself (their names hold %): ' + ', '.join(kept))
        start_detached(f'cmd.exe /c "{uninstall_script(gone, empty)}"', cwd=os.environ.get('TEMP', 'C:\\'))
    else:
        log('uninstall: remove these yourself: ' + ', '.join(str(p) for p in gone if p.exists()))


# ---------------------------------------------------------------- gamepad (XInput)

class _XInputGamepad(ctypes.Structure):
    _fields_ = [('wButtons', ctypes.c_ushort), ('bLeftTrigger', ctypes.c_ubyte), ('bRightTrigger', ctypes.c_ubyte),
                ('sThumbLX', ctypes.c_short), ('sThumbLY', ctypes.c_short), ('sThumbRX', ctypes.c_short),
                ('sThumbRY', ctypes.c_short)]


class _XInputState(ctypes.Structure):
    _fields_ = [('dwPacketNumber', ctypes.c_uint), ('Gamepad', _XInputGamepad)]


BUTTONS = {0x0001: 'up', 0x0002: 'down', 0x0004: 'left', 0x0008: 'right', 0x1000: 'a', 0x2000: 'b', 0x8000: 'y',
           0x4000: 'x', 0x0010: 'start'}


def in_front(root):
    """Whether the window the user is typing into belongs to this process. XInput reads the pad whichever window is in
    front, so without this check the launcher, left open behind the game, took the presses meant for the game: A
    activated its focused row (it started the game five times in five minutes and tried a rebuild), the stick moved
    its focus and Y checked for updates. True where it cannot tell."""
    if os.name != 'nt':
        return True
    try:
        user32 = ctypes.windll.user32
        user32.GetForegroundWindow.restype = ctypes.c_void_p
        user32.GetWindowThreadProcessId.restype = ctypes.c_uint
        user32.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint)]
        hwnd = user32.GetForegroundWindow()
        if not hwnd:
            return False   # no window in front (the desktop is switching, a lock screen): nothing is meant for us
        pid = ctypes.c_uint(0)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        return pid.value == os.getpid()
    except (AttributeError, OSError):
        return True


class Gamepad:
    """Polls the first XInput pad; poll() returns the buttons pressed since the last call (edges), the left stick
    counted as the d-pad. poll(False) still reads the pad but returns nothing: a button held while another window was
    in front does not count as pressed when this one comes back."""

    def __init__(self):
        self.dll = None
        self.held = set()
        if os.name == 'nt':
            for name in ('xinput1_4', 'xinput1_3', 'xinput9_1_0'):
                try:
                    self.dll = getattr(ctypes.windll, name)
                    break
                except (AttributeError, OSError):
                    continue

    def poll(self, active=True):
        if not self.dll:
            return set()
        st = _XInputState()
        down = set()
        for pad in range(4):
            try:
                if self.dll.XInputGetState(pad, ctypes.byref(st)) == 0:
                    g = st.Gamepad
                    down = {name for bit, name in BUTTONS.items() if g.wButtons & bit}
                    if g.sThumbLY > 16000:
                        down.add('up')
                    elif g.sThumbLY < -16000:
                        down.add('down')
                    if g.sThumbLX > 16000:
                        down.add('right')
                    elif g.sThumbLX < -16000:
                        down.add('left')
                    break
            except OSError:
                return set()
        pressed = down - self.held if active else set()
        self.held = down
        return pressed


def is_windows():
    return os.name == 'nt'


def frozen_python():
    return Path(sys.executable).parent
