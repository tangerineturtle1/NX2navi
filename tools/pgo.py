"""Merge the raw profiles of an instrumented build into the execution profile a build compiles with.

    python tools/pgo.py merge <game folder> --version v111 --toolchain C:\\msys64\\ucrt64
    python tools/pgo.py merge build/v143-pgogen --version v143 [--out build/pgo/pgo-v143.profdata]

An instrumented build (tools/make_game.py --pgo gen for game 1.1.1, tools/build_game.py --pgo gen for the other
versions) writes <build>/pgo/default_<id>.profraw while the game runs (every minute and at exit); successive runs of
the same exe add to that file. llvm-profdata must come from the toolchain the instrumented build was compiled with
(--toolchain DIR, its bin/llvm-profdata): the raw format is tied to the compiler's version. The merged file is read
by the compilers of the builds that use it; one of the same LLVM release reads it, an older one may refuse it, which
tools/make_game.py finds out with a trial compile before it uses the file (pgo_trial).

Beside the profile goes <out>.json, the training record: the game version, recomp/ at the merge as its git tree
(recomp_tree) and by content (recomp_digest, which a kit without git compares: tools/make_game.py pgo_record), and
the tool that merged. The profile matches the generated C of that generator, since functions are matched by name and
control-flow hash and a regenerated function silently loses its counts. tools/make_game.py therefore compiles with a
profile only while recomp/ is the recorded one, and tools/make_launcher.py --pgo and tools/make_release.py say so
when it is not. Merge right after the training runs, from the commit they were built at: a game folder made by
make_game.py --pgo gen names the recomp/ it was built from (pgo/training.json), and the merge stops when the
checkout holds another.
"""
import argparse
import datetime
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = '.exe' if sys.platform == 'win32' else ''
DEFAULT_DIR = ROOT / 'build' / 'pgo'   # make_game.py, make_launcher.py and make_release.py look here
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')


def llvm_profdata(toolchain):
    """llvm-profdata of --toolchain; without one the newest llvm-mingw under toolchain/, else MSYS2 UCRT64's, else
    the one on PATH."""
    if toolchain:
        tool = toolchain / 'bin' / f'llvm-profdata{EXE}'
    else:
        found = sorted((ROOT / 'toolchain').glob(f'llvm-mingw-*/llvm-mingw-*/bin/llvm-profdata{EXE}'))
        on_path = shutil.which('llvm-profdata')
        tool = (found[-1] if found else MSYS_BIN / 'llvm-profdata.exe' if (MSYS_BIN / 'llvm-profdata.exe').is_file()
                else Path(on_path) if on_path else Path(f'llvm-profdata{EXE}'))
    if not tool.is_file():
        sys.exit(f'{tool} not found: pass --toolchain with the folder of the compiler that built the instrumented '
                 'game (docs/building.md, "Profile-guided build", says which package holds llvm-profdata)')
    return tool


def recomp_tree():
    """The git tree id of recomp/ at HEAD; None when recomp/ has uncommitted changes."""
    dirty = subprocess.run(['git', '-C', str(ROOT), 'status', '--porcelain', '--', 'recomp'], capture_output=True,
                           text=True).stdout.strip()
    if dirty:
        return None
    return subprocess.run(['git', '-C', str(ROOT), 'rev-parse', 'HEAD:recomp'], capture_output=True,
                          text=True).stdout.strip() or None


def recomp_digest(version, root=ROOT):
    """recomp/ by content, as a kit without git works it out too: the lifter's code and the version's patch table
    (for v111, tools/make_game.py recomp_digest)."""
    sys.path.insert(0, str(ROOT / 'tools'))
    import make_game
    return make_game.tree_digest(Path(root) / 'recomp', ('*.py', f'patches-{version}.txt'))


def training(build, version):
    """What the instrumented game in build was made from (pgo/training.json, tools/make_game.py pgo_gen_record), {}
    for a build that wrote none (tools/build_game.py). A game built from another recomp/ than this checkout's, or for
    another game version, stops the merge: the record would name a generator the counts were not gathered on."""
    try:
        made = json.loads((build / 'pgo' / 'training.json').read_text(encoding='utf-8'))
    except OSError:
        return {}
    except ValueError:
        sys.exit(f'{build / "pgo" / "training.json"} cannot be read')
    if made.get('version') != version:
        sys.exit(f'{build} was built for game version {made.get("version")}, not {version}')
    if made.get('recomp_digest') != recomp_digest(version):
        sys.exit(f'{build} was built from another recomp/ than this checkout\'s: merge from the commit the '
                 'instrumented game was built at')
    return made


def llvm_release(text):
    """The LLVM release a --version line names, '' when it names none."""
    m = re.search(r'version (\d+\.\d+\.\d+)', text or '')
    return m.group(1) if m else ''


def merge(build, out, version, toolchain):
    raw = sorted((build / 'pgo').glob('*.profraw'))
    if not raw:
        sys.exit(f'no .profraw in {build / "pgo"}: run the instrumented game first')
    tree = recomp_tree()
    if not tree:
        sys.exit('recomp/ has uncommitted changes: the record could not name the generator the profile matches')
    made = training(build, version)
    tool = llvm_profdata(toolchain)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix('.profdata.tmp')
    cmd = [str(tool), 'merge', '--failure-mode=all', '-o', str(tmp), *map(str, raw)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        tmp.unlink(missing_ok=True)
        sys.exit(f'llvm-profdata merge failed: {(p.stdout + p.stderr).strip()[-2000:]}')
    tmp.replace(out)
    said = subprocess.run([str(tool), '--version'], capture_output=True, text=True).stdout
    record = {'version': version, 'recomp_tree': tree, 'recomp_digest': recomp_digest(version),
              'merged': datetime.date.today().isoformat(), 'raw_files': len(raw),
              'tool': next((line.strip() for line in said.splitlines() if 'version' in line), ''),
              'compiler': made.get('compiler', '')}
    out.with_suffix('.json').write_text(json.dumps(record, indent=1) + '\n', encoding='utf-8')
    if made.get('compiler') and llvm_release(made['compiler']) != llvm_release(record['tool']):
        print(f'pgo: warning: the game was built with {made["compiler"]} and its counts were merged with '
              f'{record["tool"]}: merge with the llvm-profdata of the same LLVM release (--toolchain)', file=sys.stderr)
    size = sum(f.stat().st_size for f in raw)
    print(f'{out}: {len(raw)} raw files ({size / 2**20:.1f} MiB) -> {out.stat().st_size / 2**20:.1f} MiB; '
          f'recomp tree {tree[:12]}')


def fitting(profile, version='v111'):
    """The record beside a profile, for a release from this checkout. A profile whose record does not name this
    checkout's recomp/ stops the release: the builds made from it would leave the profile out (tools/make_game.py
    pgo_record), and the release would carry it for nothing."""
    profile = Path(profile)
    rec = profile.with_suffix('.json')
    if not profile.is_file():
        sys.exit(f'{profile} not found')
    try:
        record = json.loads(rec.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        sys.exit(f'{rec} is missing or cannot be read: tools/pgo.py merge writes it beside the profile')
    if record.get('version') != version:
        sys.exit(f'{profile} was trained on game version {record.get("version")}, not {version}')
    if record.get('recomp_digest') != recomp_digest(version):
        sys.exit(f'{profile} was trained on another recomp/ than the one in this checkout (its record names the tree '
                 f'{str(record.get("recomp_tree", "unknown"))[:12]}): the builds would leave it out. Train a profile '
                 'again (docs/building.md, "Profile-guided build") or release without one')
    return record


def place(profile, data, version='v111'):
    """Copy a profile and its record into a release's data/ as pgo-<version>.profdata and .json, where the builds
    made from that release look for them (tools/make_game.py pgo_file). The path of the profile's copy."""
    fitting(profile, version)
    data = Path(data)
    data.mkdir(parents=True, exist_ok=True)
    shutil.copy2(profile, data / f'pgo-{version}.profdata')
    shutil.copy2(Path(profile).with_suffix('.json'), data / f'pgo-{version}.json')
    return data / f'pgo-{version}.profdata'


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('merge', help='merge <build>/pgo/*.profraw into one .profdata and its training record')
    m.add_argument('build', type=Path, help='the instrumented build: the game folder of make_game.py --pgo gen, or '
                                            'the build directory of build_game.py --pgo gen')
    m.add_argument('--version', required=True, help='game version the build was made for (v111, v143)')
    m.add_argument('--out', type=Path, help=f'default {DEFAULT_DIR.relative_to(ROOT).as_posix()}/pgo-<version>.profdata')
    m.add_argument('--toolchain', type=Path,
                   help='the folder of the compiler that built the instrumented game, with bin/llvm-profdata: an '
                        'MSYS2 UCRT64 folder or an llvm-mingw directory (default: the newest llvm-mingw under '
                        'toolchain/, else C:\\msys64\\ucrt64, else PATH)')
    a = ap.parse_args()
    out = (a.out or DEFAULT_DIR / f'pgo-{a.version}.profdata').resolve()
    merge(a.build.resolve(), out, a.version, a.toolchain)


if __name__ == '__main__':
    main()
