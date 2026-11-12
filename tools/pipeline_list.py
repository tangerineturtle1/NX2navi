"""The shared pipeline list: graphics pipelines players met, for an install to build before play.

A game folder records every graphics pipeline a run builds in pipeline-keys.bin (runtime/vk_pipeline.c "pipeline
keys": the pipeline state with its shaders named by input id) and every shader variant in shader-answers.bin
(runtime/gfx_shader.c "answers met in play": stage, code hash, input id, translator answers). The next start of that
folder builds its recorded pipelines ahead of play; another install, or the same one after its caches were removed,
meets each of them in play first. A mesh pipeline met that way has no stage libraries to be linked from, so its draws
are left out of the picture for its whole compile, 0.2 to 1.1 s.

This tool folds the two files of any number of game folders into one list file; a list file made before can stand
in for a folder, so a list grows by the folders played since. Beside the shader variants the pipelines name, the list
takes the compute variants each folder dispatched: no pipeline record names a compute shader, and a variant with
other answers than the shader list's default one was translated in the middle of play, 30 to 40 ms on the frame that
first dispatched it. tools/prepare_shaders.py reads the
list beside the shared shader list: the shader variants it names go into the seed, so the preparation translates them,
and its pipeline records go next to the game as seed-pipeline-keys.bin, which every start of the game counts as
recorded pipelines beside its own (runtime/vk_pipeline.c keys_open_seed), so a new install builds them before play,
and which `nx2navi.exe --prepare-shaders` (and
`--prepare-pipelines` on a folder that is prepared already) builds once into the pipeline cache and the graphics
driver's own.

What a record depends on. Its shaders are named by input id, a hash of the game's shader code and the answers, which
no translator, GPU or driver changes: a record made on one machine names the same shaders on another with the same
game version. Its state is the game's (formats, blend, depth and stencil, topology, sample count) and a few choices of
the runtime's settings (a culled foliage shadow pass, the depth range); everything that depends on the GPU is derived
from it when the pipeline is built. A record whose state this install never reaches costs one pipeline in the caches,
and one whose shader this game version lacks is left out by the preparation. The record layout belongs to the runtime
that wrote it (KEYS_VERSION and the record size, in the file's header): a list is made again for a runtime that
changes either, and the runtime ignores one that does not match.

The list is derived from game data: it stays out of the repository (tools/check_content.py refuses it) and travels
with the installer archive as data/pipeline_list.bin, as the shader list does.

    python tools/pipeline_list.py merge OUT.bin GAME_DIR_OR_LIST [GAME_DIR_OR_LIST ...] [--mesh-only]
    python tools/pipeline_list.py info FILE_OR_GAME_DIR [...]

File: "NX2NPLST", version u32, answers bytes u32, keys bytes u32, then a shader-answers.bin image (header, the
records the keys name and the compute variants', input id kept) and a pipeline-keys.bin image (header and records).
"""
import argparse
import struct
import sys
from pathlib import Path

MAGIC = b'NX2NPLST'
VERSION = 1
ANS_MAGIC, ANS_REC = b'NX2NSANS', 4 + 32 + 32 + 147 + 1   # gfx_shader.c: stage, code hash, input id, answers, pad
KEYS_MAGIC = b'NX2NPKEY'
ID = 32
STAGE_NAMES = {0: 'vertex', 4: 'fragment', 5: 'compute', 6: 'task', 7: 'mesh'}   # runtime/gfx.h GFX_STAGE_*
STAGE_COMPUTE, STAGE_MESH = 5, 7


def read_answers(data):
    """shader-answers.bin image -> its records (bytes each); [] for another magic or record size."""
    if len(data) < 16 or data[:8] != ANS_MAGIC:
        return []
    _, rec = struct.unpack_from('<II', data, 8)
    if rec != ANS_REC:
        return []
    n = (len(data) - 16) // rec
    return [data[16 + i * rec:16 + (i + 1) * rec] for i in range(n)]


def read_keys(data):
    """pipeline-keys.bin image -> (magic, version, record size, records); records [] for a file too short."""
    if len(data) < 16:
        return b'', 0, 0, []
    magic = data[:8]
    version, rec = struct.unpack_from('<II', data, 8)
    if rec < 3 * ID + 8:
        return magic, version, rec, []
    n = (len(data) - 16) // rec
    return magic, version, rec, [data[16 + i * rec:16 + (i + 1) * rec] for i in range(n)]


def key_ids(rec):
    """The input ids a key record names: vertex (or mesh), then fragment and task when it has them."""
    has_fs, has_ts = struct.unpack_from('<II', rec, 3 * ID)
    ids = [rec[:ID]]
    if has_fs:
        ids.append(rec[ID:2 * ID])
    if has_ts:
        ids.append(rec[2 * ID:3 * ID])
    return ids


def read_dir(d):
    """A game folder's recorded keys and answers; a missing file gives no records. A list file gives what it holds."""
    d = Path(d)
    if d.is_file():
        got = unpack(d.read_bytes())
        return (read_keys(got[1]), got[0]) if got else ((b'', 0, 0, []), [])
    kp, ap = d / 'pipeline-keys.bin', d / 'shader-answers.bin'
    keys = read_keys(kp.read_bytes()) if kp.is_file() else (b'', 0, 0, [])
    return keys, read_answers(ap.read_bytes()) if ap.is_file() else []


def merge(dirs, mesh_only=False, log=print):
    """One list from the folders' files (or earlier lists): each key record once, the answers record of every shader
    they name and, unless mesh_only, of every compute variant a folder dispatched."""
    header, keys, seen, answers = None, [], set(), {}
    for d in dirs:
        (magic, version, rec, recs), ans = read_dir(d)
        if not recs:
            log(f'{d}: no pipeline key records')
            continue
        if header is None:
            header = (magic, version, rec)
        elif header != (magic, version, rec):
            log(f'{d}: its pipeline-keys.bin is of another layout ({magic!r}, version {version}, {rec} bytes a record); '
                'left out')
            continue
        by_id = {a[36:68]: a for a in ans}
        added = lacking = 0
        for r in recs:
            ids = key_ids(r)
            if r in seen:
                continue
            if any(i not in by_id and i not in answers for i in ids):
                lacking += 1   # a shader the folder's answers do not name: another install could not translate it
                continue
            if mesh_only and by_id.get(ids[0], answers.get(ids[0]))[0:4] != struct.pack('<I', STAGE_MESH):
                continue
            seen.add(r)
            keys.append(r)
            added += 1
            for i in ids:
                answers.setdefault(i, by_id.get(i) or answers[i])
        compute = 0
        for a in ans if not mesh_only else ():
            if a[0:4] == struct.pack('<I', STAGE_COMPUTE) and a[36:68] not in answers:
                answers[a[36:68]] = a
                compute += 1
        log(f'{d}: {len(recs)} key records, {added} new to the list, {lacking} without their shaders\' answers; '
            f'{compute} compute variants new to the list')
    if header is None:
        return None
    return header, keys, list(answers.values())


def pack(header, keys, answers):
    magic, version, rec = header
    a = ANS_MAGIC + struct.pack('<II', 1, ANS_REC) + b''.join(answers)
    k = magic + struct.pack('<II', version, rec) + b''.join(keys)
    return MAGIC + struct.pack('<III', VERSION, len(a), len(k)) + a + k


def unpack(data):
    """A list file -> (answers records, pipeline-keys.bin image); None when it is not one."""
    if len(data) < 20 or data[:8] != MAGIC:
        return None
    version, na, nk = struct.unpack_from('<III', data, 8)
    if version != VERSION or len(data) != 20 + na + nk:
        return None
    return read_answers(data[20:20 + na]), data[20 + na:]


def describe(name, keys_image, answers):
    magic, version, rec, recs = read_keys(keys_image)
    stage = {a[36:68]: struct.unpack_from('<I', a)[0] for a in answers}
    mesh = sum(1 for r in recs if stage.get(r[:ID]) == STAGE_MESH)
    task = sum(1 for r in recs if struct.unpack_from('<I', r, 3 * ID + 4)[0])
    named = {i for r in recs for i in key_ids(r)}
    by = {}
    for i in named:
        s = STAGE_NAMES.get(stage.get(i), 'not in the answers')
        by[s] = by.get(s, 0) + 1
    compute = sum(1 for a in answers if a[0:4] == struct.pack('<I', STAGE_COMPUTE))
    print(f'{name}: {len(recs)} pipelines ({mesh} mesh, {task} of them with a task stage), layout {magic.decode("ascii", "replace")} '
          f'version {version}, {rec} bytes a record; {len(named)} shaders named: '
          + ', '.join(f'{v} {k}' for k, v in sorted(by.items())) + f'; {compute} compute variants')
    return len(recs), mesh


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('merge', help='fold game folders, and lists made before, into a list file')
    m.add_argument('out', type=Path)
    m.add_argument('dirs', nargs='+', type=Path)
    m.add_argument('--mesh-only', action='store_true', help='keep the mesh pipelines only')
    i = sub.add_parser('info', help='what a list file or a game folder holds')
    i.add_argument('paths', nargs='+', type=Path)
    a = ap.parse_args()
    if a.cmd == 'info':
        for p in a.paths:
            if p.is_dir():
                (magic, version, rec, recs), ans = read_dir(p)
                image = magic + struct.pack('<II', version, rec) + b''.join(recs)
                describe(str(p), image, ans)
            else:
                got = unpack(p.read_bytes())
                if got is None:
                    print(f'{p}: not a pipeline list')
                    return 1
                describe(str(p), got[1], got[0])
        return 0
    got = merge(a.dirs, a.mesh_only)
    if got is None:
        print('no pipeline key records in any folder')
        return 1
    header, keys, answers = got
    data = pack(header, keys, answers)
    a.out.parent.mkdir(parents=True, exist_ok=True)
    a.out.write_bytes(data)
    describe(str(a.out), unpack(data)[1], answers)
    print(f'{a.out}: {len(data)} bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main())
