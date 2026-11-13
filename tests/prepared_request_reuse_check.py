"""Compare preparation request sharing with independent jobs using six synthetic shaders.

Pass the candidate executable, the translator, and --reference-exe for a paired build.
No game data or installed cache is used. All output stays in build/.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]

def seed(directory):
    programs = bytearray(struct.pack('<8sII', b'NX2NSPRG', 1, 0))
    answers = bytearray(struct.pack('<8sII', b'NX2NSANS', 1, 216))
    exit_op = struct.pack('<4I', 0x794d, 0, 0x03800000, 0x000fea00)
    for tag, group, code, c1 in [(0, 1, exit_op, b''), (1, 1, exit_op, b''), (2, 1, exit_op, b''),
                                 (0, 2, exit_op, b''), (0, 1, 2 * exit_op, b''), (0, 1, exit_op, bytes(16))]:
        control = bytearray(0x880)
        struct.pack_into('<I', control, 0, 0x98761234)
        control[0x50] = tag
        struct.pack_into('<I', control, 0x38, len(code))
        struct.pack_into('<II', control, 0x40, len(code), len(code) + len(c1))
        struct.pack_into('<3I', control, 0x58, group, 1, 1)
        full = code + c1
        key = hashlib.sha256(struct.pack('<I', 5) + control + struct.pack('<Q', len(full)) + full
                             + struct.pack('<I', len(c1)) + c1).digest()
        programs += struct.pack('<III', 5, len(full), len(c1)) + key + control + full + c1
        answers += struct.pack('<I', 5) + key + bytes(32 + 147 + 1)
    (directory / 'seed-programs.bin').write_bytes(programs)
    (directory / 'seed-answers.bin').write_bytes(answers)


def snapshot(cache):
    return {str(p.relative_to(cache)): p.read_bytes() for p in cache.rglob('*')
            if p.is_file() and (p.suffix in ('.spv', '.use', '.in') or p.name.endswith('.res.json'))}


def run(exe, translator, folder, keep=False):
    env = dict(os.environ, NX2NAVI_XLATE=str(translator), NX2NAVI_XLATE_ID='request-reuse-test',
               NX2NAVI_XLATE_KEEP='2' if keep else '0', NX2NAVI_SHADER_CARRY='0',
               NX2NAVI_PREWARM_XLATE='4', NX2NAVI_PIPELINE_CACHE=str(folder/'pipeline-cache.bin'),
               NX2NAVI_SHADER_CACHE=str(folder/'shader-cache'))
    t=time.perf_counter()
    result=subprocess.run([str(exe),'--prepare-shaders'],cwd=folder,env=env,capture_output=True,timeout=90)
    elapsed=time.perf_counter()-t
    log=(result.stdout+result.stderr).decode(errors='replace')
    (folder/('warm.log' if (folder/'run.log').exists() else 'run.log')).write_text(log)
    assert result.returncode == 0, log
    translated=re.search(r'6 seed variants of 6 programs: (\d+) translated, 0 failed',log)
    assert translated,log
    return int(translated[1]),elapsed,log


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('game',type=Path)
    ap.add_argument('translator',type=Path)
    ap.add_argument('--reference-exe',type=Path)
    ap.add_argument('--out',type=Path,default=ROOT/'build/request-reuse-check')
    args=ap.parse_args()
    args.out.mkdir(parents=True,exist_ok=True)
    exe=args.game.resolve(); ref=(args.reference_exe or args.game).resolve(); translator=args.translator.resolve()
    results=[]
    for iteration in range(3):
        folders=[args.out.resolve()/f'{iteration}-{n}' for n in ('independent','shared')]
        for folder in folders:
            folder.mkdir(exist_ok=False)
            seed(folder)
        count0,time0,_=run(ref,translator,folders[0],keep=True)
        count1,time1,log1=run(exe,translator,folders[1])
        assert count0==6 and count1==4 and '2 equivalent translator requests reused' in log1,log1
        assert snapshot(folders[0]/'shader-cache')==snapshot(folders[1]/'shader-cache')
        assert (folders[0]/'shader-prepared.bin').read_bytes()==(folders[1]/'shader-prepared.bin').read_bytes()
        before=snapshot(folders[1]/'shader-cache')
        count2,_,_=run(exe,translator,folders[1])
        assert count2==0 and before==snapshot(folders[1]/'shader-cache')
        results.append(dict(independent_jobs=count0,shared_jobs=count1,independent_seconds=time0,shared_seconds=time1,
                            identical_outputs=True,warm_jobs=count2))
    (args.out/'results.json').write_text(json.dumps(results,indent=2))
    print(json.dumps(results,indent=2))


if __name__=='__main__':
    main()
