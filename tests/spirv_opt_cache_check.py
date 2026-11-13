"""Check optimizer fallback and cache handling without a GPU or game files."""
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
work = root / 'build' / 'test' / 'spirv_opt_cache'
work.mkdir(parents=True, exist_ok=True)
cc = os.environ.get('CC') or shutil.which('clang') or 'cc'
exe = work / ('spirv_opt_cache_check.exe' if os.name == 'nt' else 'spirv_opt_cache_check')
subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                str(root / 'tests/spirv_opt_cache_check.c'), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
