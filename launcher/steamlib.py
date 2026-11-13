"""Steam's library file for co-op, put beside the game so that no player has to fetch it.

Co-op (runtime/steam.c, docs/coop.md) loads steam_api64.dll from beside ocarina.exe. NX2navi's repository may not hold
that file, so the launcher supplies it on the player's PC: before the game starts, when the file is not there, it takes
one a release shipped in its bin folder, else the newest suitable one of the player's own Steam library (every Steam
game has one in its folder), checks that Valve signed it and that it has the calls the game uses, and copies it.

"Suitable" is steam.c's list: the manual dispatch calls, the networking messages interface and the lobby calls, which
libraries since 2020 have. The names an exe can import are stored in the file as plain text, so the check is a search
for them. A file that is not signed by Valve (a replacement that makes a game run without Steam) is never taken.

Windows only: elsewhere ensure() does nothing and co-op says that the file is missing.
"""
import ctypes
import os
import re
import shutil
from pathlib import Path

NAME = 'steam_api64.dll'
MAX_BYTES = 8 << 20      # the real file is under half a megabyte
SEARCH_DEPTH = 5         # folders below a game's own: Unity keeps the file in <game>_Data/Plugins/x86_64
MAX_FOLDERS = 60000      # of all libraries together: a search that would take minutes ends without a file

NEEDS = (
    b'SteamAPI_ManualDispatch_Init', b'SteamAPI_ManualDispatch_RunFrame', b'SteamAPI_ManualDispatch_GetNextCallback',
    b'SteamAPI_ManualDispatch_FreeLastCallback', b'SteamAPI_ManualDispatch_GetAPICallResult', b'SteamAPI_GetHSteamPipe',
    b'SteamAPI_Shutdown', b'SteamAPI_SteamMatchmaking_v009', b'SteamAPI_SteamNetworkingMessages_SteamAPI_v002',
    b'SteamAPI_ISteamNetworkingMessages_SendMessageToUser', b'SteamAPI_ISteamNetworkingMessages_ReceiveMessagesOnChannel',
    b'SteamAPI_ISteamMatchmaking_CreateLobby', b'SteamAPI_ISteamMatchmaking_InviteUserToLobby',
    b'SteamAPI_ISteamFriends_HasFriend', b'SteamAPI_SteamNetworkingIdentity_SetSteamID64',
)
ONE_OF = (
    (b'SteamAPI_InitFlat', b'SteamAPI_InitSafe', b'SteamAPI_Init'),
    (b'SteamAPI_SteamUser_v023', b'SteamAPI_SteamUser_v022', b'SteamAPI_SteamUser_v021', b'SteamAPI_SteamUser_v020'),
    (b'SteamAPI_SteamFriends_v018', b'SteamAPI_SteamFriends_v017'),
    (b'SteamAPI_SteamUtils_v010', b'SteamAPI_SteamUtils_v009'),
)


def suitable(path):
    """True when the file is a library with every call runtime/steam.c needs (its NEED list and interface versions)."""
    try:
        path = Path(path)
        if not path.is_file() or path.stat().st_size > MAX_BYTES:
            return False
        data = path.read_bytes()
    except OSError:
        return False
    if data[:2] != b'MZ':
        return False

    def has(name):
        return b'\0' + name + b'\0' in data

    return all(has(n) for n in NEEDS) and all(any(has(n) for n in group) for group in ONE_OF)


def signed_by_valve(path):
    """True when Windows finds the file's signature valid and its signer is Valve."""
    if os.name != 'nt':
        return False
    try:
        return _trusted(str(path)) and 'valve' in _signer(str(path)).lower()
    except (OSError, ValueError, AttributeError):
        return False


def _trusted(path):
    from ctypes import wintypes

    class Guid(ctypes.Structure):
        _fields_ = [('a', wintypes.DWORD), ('b', wintypes.WORD), ('c', wintypes.WORD), ('d', ctypes.c_ubyte * 8)]

    class FileInfo(ctypes.Structure):
        _fields_ = [('size', wintypes.DWORD), ('path', wintypes.LPCWSTR), ('file', wintypes.HANDLE), ('subject', ctypes.c_void_p)]

    class Data(ctypes.Structure):
        _fields_ = [('size', wintypes.DWORD), ('policy', ctypes.c_void_p), ('sip', ctypes.c_void_p), ('ui', wintypes.DWORD),
                    ('revocation', wintypes.DWORD), ('union', wintypes.DWORD), ('info', ctypes.POINTER(FileInfo)),
                    ('action', wintypes.DWORD), ('state', wintypes.HANDLE), ('url', wintypes.LPCWSTR),
                    ('flags', wintypes.DWORD), ('context', wintypes.DWORD), ('settings', ctypes.c_void_p)]

    # WINTRUST_ACTION_GENERIC_VERIFY_V2
    action = Guid(0x00AAC56B, 0xCD44, 0x11D0, (ctypes.c_ubyte * 8)(0x8C, 0xC2, 0x00, 0xC0, 0x4F, 0xC2, 0x95, 0xEE))
    info = FileInfo(ctypes.sizeof(FileInfo), path, None, None)
    # no window (ui 2), the whole chain's revocation is not asked from the network (0), a file (1), verify (1), and
    # the cache only for revocation (flags 0x1000): a start of the game never waits for the internet
    data = Data(ctypes.sizeof(Data), None, None, 2, 0, 1, ctypes.pointer(info), 1, None, None, 0x1000, 0, None)
    verify = ctypes.windll.wintrust.WinVerifyTrust
    verify.argtypes = [wintypes.HWND, ctypes.POINTER(Guid), ctypes.POINTER(Data)]
    verify.restype = ctypes.c_long
    ok = verify(None, ctypes.byref(action), ctypes.byref(data)) == 0
    data.action = 2   # close: frees what the verification kept
    verify(None, ctypes.byref(action), ctypes.byref(data))
    return ok


def _signer(path):
    """The name on the certificate that signed the file, '' when it has none."""
    from ctypes import wintypes
    crypt = ctypes.windll.crypt32
    store, msg = wintypes.HANDLE(), wintypes.HANDLE()
    crypt.CryptQueryObject.argtypes = [wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                       ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(wintypes.HANDLE),
                                       ctypes.POINTER(wintypes.HANDLE), ctypes.c_void_p]
    crypt.CryptQueryObject.restype = wintypes.BOOL
    name = ctypes.create_unicode_buffer(path)
    # a file (1), a signature embedded in it (1 << 10), binary (1 << 1)
    if not crypt.CryptQueryObject(1, name, 1 << 10, 1 << 1, 0, None, None, None, ctypes.byref(store), ctypes.byref(msg), None):
        return ''
    try:
        crypt.CryptMsgGetParam.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                           ctypes.POINTER(wintypes.DWORD)]
        crypt.CryptMsgGetParam.restype = wintypes.BOOL
        size = wintypes.DWORD(0)
        if not crypt.CryptMsgGetParam(msg, 7, 0, None, ctypes.byref(size)):   # CMSG_SIGNER_CERT_INFO_PARAM
            return ''
        info = ctypes.create_string_buffer(size.value)
        if not crypt.CryptMsgGetParam(msg, 7, 0, info, ctypes.byref(size)):
            return ''
        crypt.CertFindCertificateInStore.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD,
                                                     ctypes.c_void_p, ctypes.c_void_p]
        crypt.CertFindCertificateInStore.restype = ctypes.c_void_p
        # X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, CERT_FIND_SUBJECT_CERT
        cert = crypt.CertFindCertificateInStore(store, 0x10001, 0, 11 << 16, info, None)
        if not cert:
            return ''
        try:
            crypt.CertGetNameStringW.argtypes = [ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                                 wintypes.LPWSTR, wintypes.DWORD]
            crypt.CertGetNameStringW.restype = wintypes.DWORD
            out = ctypes.create_unicode_buffer(256)
            crypt.CertGetNameStringW(cert, 4, 0, None, out, 256)   # CERT_NAME_SIMPLE_DISPLAY_TYPE
            return out.value
        finally:
            crypt.CertFreeCertificateContext.argtypes = [ctypes.c_void_p]
            crypt.CertFreeCertificateContext(cert)
    finally:
        crypt.CryptMsgClose.argtypes = [wintypes.HANDLE]
        crypt.CryptMsgClose(msg)
        crypt.CertCloseStore.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        crypt.CertCloseStore(store, 0)


def libraries(steam_folder):
    """The steamapps folders of the player's Steam libraries: Steam's own and those libraryfolders.vdf lists."""
    steam_folder = Path(steam_folder)
    out = [steam_folder / 'steamapps']
    try:
        text = (steam_folder / 'steamapps' / 'libraryfolders.vdf').read_text(encoding='utf-8', errors='replace')
        for raw in re.findall(r'"path"\s+"([^"]+)"', text):
            out.append(Path(raw.replace('\\\\', '\\')) / 'steamapps')
    except OSError:
        pass
    seen, unique = set(), []
    for p in out:
        key = os.path.normcase(os.path.abspath(str(p)))
        if key not in seen and p.is_dir():
            seen.add(key)
            unique.append(p)
    return unique


def candidates(steam_folder):
    """Every steam_api64.dll in the games of the player's libraries, the newest file first."""
    found, budget = [], [MAX_FOLDERS]

    def walk(folder, depth):
        if budget[0] <= 0:
            return
        budget[0] -= 1
        try:
            entries = list(os.scandir(folder))
        except OSError:
            return
        for e in entries:
            try:
                if e.is_file(follow_symlinks=False):
                    if e.name.lower() == NAME:
                        found.append((e.stat().st_mtime, e.path))
                elif depth < SEARCH_DEPTH and e.is_dir(follow_symlinks=False):
                    walk(e.path, depth + 1)
            except OSError:
                continue

    for lib in libraries(steam_folder):
        walk(lib / 'common', 0)
    return [Path(p) for _, p in sorted(found, reverse=True)]


def find(steam_folder, signed=signed_by_valve):
    """The newest suitable library of the player's Steam games that Valve signed; None when there is none."""
    for path in candidates(steam_folder):
        if suitable(path) and signed(path):
            return path
    return None


_none = set()   # game folders for which this run of the launcher found no library: not searched again at each Play


def ensure(game_dir, kit_bin=None, steam_folder=None, log=print, signed=signed_by_valve):
    """Puts the library beside the game when it is not there. 'there', 'copied' or 'none' (no Steam, or no game of
    the player's library has a suitable one). Never raises: co-op then says that the file is missing. The search of
    the Steam library takes a second or two, once: a folder that got none is not searched again by this process."""
    try:
        dst = Path(game_dir) / NAME
        if suitable(dst):
            return 'there'
        if str(dst) in _none:
            return 'none'
        shipped = Path(kit_bin) / NAME if kit_bin else None
        src = shipped if shipped is not None and suitable(shipped) and signed(shipped) else None
        if src is None and os.name == 'nt':
            if steam_folder is None:
                from . import steam
                at = steam.find_steam()
                steam_folder = at[0] if at else None
            src = find(steam_folder, signed) if steam_folder else None
        if src is None:
            log("co-op: no suitable steam_api64.dll (Steam's library file) was found for the game: co-op needs Steam "
                'installed with at least one game from 2021 or later')
            _none.add(str(dst))
            return 'none'
        shutil.copyfile(src, dst)
        log(f"co-op: Steam's library file was copied beside the game from {src.parent}")
        return 'copied'
    except OSError as e:
        log(f"co-op: Steam's library file could not be put beside the game: {e}")
        return 'none'
