"""Memory the machine can still give to new processes, for sizing worker pools (stdlib only)."""
import ctypes
import ctypes.wintypes as wt
import sys
from pathlib import Path

PROC = Path('/proc')
CGROUP = Path('/sys/fs/cgroup')   # where the unified (v2) cgroup hierarchy is mounted

JOB_OBJECT_EXTENDED_LIMIT_INFORMATION = 9
LIMIT_PROCESS_MEMORY = 0x100
LIMIT_JOB_MEMORY = 0x200


class _MemStatus(ctypes.Structure):
    _fields_ = [('dwLength', wt.DWORD), ('dwMemoryLoad', wt.DWORD)] + \
               [(n, ctypes.c_ulonglong) for n in ('total', 'avail', 'tpf', 'apf', 'tv', 'av', 'aev')]


class _JobLimits(ctypes.Structure):
    _fields_ = [('PerProcessUserTimeLimit', ctypes.c_longlong), ('PerJobUserTimeLimit', ctypes.c_longlong),
                ('LimitFlags', wt.DWORD), ('MinimumWorkingSetSize', ctypes.c_size_t),
                ('MaximumWorkingSetSize', ctypes.c_size_t), ('ActiveProcessLimit', wt.DWORD),
                ('Affinity', ctypes.c_size_t), ('PriorityClass', wt.DWORD), ('SchedulingClass', wt.DWORD),
                ('r_ops', ctypes.c_ulonglong), ('w_ops', ctypes.c_ulonglong), ('o_ops', ctypes.c_ulonglong),
                ('r_bytes', ctypes.c_ulonglong), ('w_bytes', ctypes.c_ulonglong), ('o_bytes', ctypes.c_ulonglong),
                ('ProcessMemoryLimit', ctypes.c_size_t), ('JobMemoryLimit', ctypes.c_size_t),
                ('PeakProcessMemoryUsed', ctypes.c_size_t), ('PeakJobMemoryUsed', ctypes.c_size_t)]


def _job_headroom():
    """Bytes left under the memory limit of the job object this process runs in, or None without such a limit."""
    k32 = ctypes.WinDLL('kernel32', use_last_error=True)
    k32.QueryInformationJobObject.argtypes = [wt.HANDLE, ctypes.c_int, ctypes.c_void_p, wt.DWORD, ctypes.c_void_p]
    info = _JobLimits()
    # A null handle asks about the job of the calling process.
    if not k32.QueryInformationJobObject(None, JOB_OBJECT_EXTENDED_LIMIT_INFORMATION, ctypes.byref(info),
                                         ctypes.sizeof(info), None):
        return None
    if info.LimitFlags & LIMIT_JOB_MEMORY:
        return max(0, info.JobMemoryLimit - info.PeakJobMemoryUsed)
    if info.LimitFlags & LIMIT_PROCESS_MEMORY:
        return info.ProcessMemoryLimit
    return None


def _meminfo_available():
    """MemAvailable of /proc/meminfo in bytes: what new processes can use without swapping (free memory plus the page
    cache and slab the kernel gives back). None when it cannot be read."""
    try:
        for line in (PROC / 'meminfo').read_text(encoding='ascii', errors='replace').splitlines():
            if line.startswith('MemAvailable:'):
                return int(line.split()[1]) * 1024   # the file says kB but means KiB
    except (OSError, ValueError, IndexError):
        pass
    return None


def _cgroup_headroom():
    """Bytes left under the tightest memory limit of this process's cgroup and the cgroups above it (cgroup v2:
    memory.max less memory.current), or None without such a limit."""
    try:
        line = next(l for l in (PROC / 'self' / 'cgroup').read_text(encoding='ascii', errors='replace').splitlines()
                    if l.startswith('0::'))   # the v2 entry; a v1 system has none
    except (OSError, StopIteration):
        return None
    parts = [p for p in line[3:].split('/') if p]
    headroom = None
    for n in range(len(parts), -1, -1):
        folder = CGROUP.joinpath(*parts[:n])
        try:
            limit = (folder / 'memory.max').read_text(encoding='ascii').strip()
            if limit == 'max':
                continue
            left = max(0, int(limit) - int((folder / 'memory.current').read_text(encoding='ascii')))
        except (OSError, ValueError):
            continue
        headroom = left if headroom is None else min(headroom, left)
    return headroom


def available_ram():
    """Physical memory not in use right now, capped by the memory limit this process runs under if there is one (the
    job object's on Windows, the cgroup's on Linux). None where it cannot be read."""
    if sys.platform != 'win32':
        avail = _meminfo_available()
        if avail is None:
            return None
        headroom = _cgroup_headroom()
        return avail if headroom is None else min(avail, headroom)
    status = _MemStatus()
    status.dwLength = ctypes.sizeof(status)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        return None
    headroom = _job_headroom()
    return status.avail if headroom is None else min(status.avail, headroom)
