"""Build and run the adaptive DLSS frame-generation controller with a synthetic clock; no game files or GPU needed."""
import os
import shutil
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
bin_dir = Path(r'C:\msys64\ucrt64\bin')
cc = os.environ.get('CC') or (str(bin_dir / 'clang.exe') if (bin_dir / 'clang.exe').exists() else shutil.which('clang'))
if not cc:
    raise SystemExit('clang is required')
vma = os.environ.get('VMA_INCLUDE') or (str(bin_dir.parent / 'include' / 'vma') if bin_dir.exists() else '/usr/include/vma')
out = root / 'build' / 'test' / 'adaptive_framegen'
out.mkdir(parents=True, exist_ok=True)
exe = out / ('check.exe' if os.name == 'nt' else 'check')
env = dict(os.environ)
if bin_dir.exists():
    env['PATH'] = str(bin_dir) + os.pathsep + env.get('PATH', '')
subprocess.check_call([cc, '-O1', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                       '-I', str(root / 'runtime'), '-I', vma,
                       str(root / 'tests' / 'adaptive_framegen_check.c'), '-o', str(exe)], env=env)
raise SystemExit(subprocess.run([str(exe)], env=env).returncode)
