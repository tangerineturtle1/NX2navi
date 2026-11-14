"""Compile world marker checks against synthetic memory without starting the game."""
from pathlib import Path
import os,shutil,subprocess
root=Path(__file__).resolve().parents[1]
cc=os.environ.get('CC') or shutil.which('clang') or r'C:\msys64\ucrt64\bin\clang.exe'
out=root/'build/test/world-extra';out.mkdir(parents=True,exist_ok=True)
exe=out/('world_extra_check.exe' if os.name=='nt' else 'world_extra_check')
env=dict(os.environ);env['PATH']=str(Path(cc).parent)+os.pathsep+env.get('PATH','')
subprocess.run([cc,'-O2','-Wall','-Wextra','-I',str(root/'runtime'),str(root/'tests/world_extra_check.c'),'-o',str(exe)],check=True,env=env)
subprocess.run([str(exe)],check=True,env=env)
