"""Compile bomb-arrow policy and native-call contract checks against synthetic guest memory."""
from pathlib import Path
import os,shutil,subprocess
root=Path(__file__).resolve().parents[1]
def main():
    cc=os.environ.get('CC') or shutil.which('clang') or r'C:\msys64\ucrt64\bin\clang.exe'
    out=root/'build/test/bomb-arrows';out.mkdir(parents=True,exist_ok=True)
    exe=out/('bomb_arrows_check.exe' if os.name=='nt' else 'bomb_arrows_check')
    env=dict(os.environ);env['PATH']=str(Path(cc).parent)+os.pathsep+env.get('PATH','')
    subprocess.run([cc,'-O2','-Wall','-Wextra','-I',str(root/'runtime'),str(root/'tests/bomb_arrows_check.c'),'-o',str(exe),'-lm'],check=True,env=env)
    subprocess.run([str(exe)],check=True,env=env)
if __name__=='__main__':main()
