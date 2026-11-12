"""Fragment inputs that no mesh output feeds (runtime/vk_pipeline.c spirv_mesh_outputs, spirv_fs_fill_inputs): builds
the two functions, cut from vk_pipeline.c by their text, and runs them on modules glslang makes here:

  - a mesh shader that draws points and writes no generic output, and one that draws triangles and writes location 1;
  - a fragment shader that reads locations 1 and 5 (5 through a component, as the translator's shaders do).

Checks that the point mesh's mask is 0 with points, the triangle mesh's is location 1; that the fragment module
rewritten for the point mesh has a PointCoord input and no Input at location 5, and for the triangle mesh keeps
location 1 as an Input; and that every rewritten module passes spirv-val. No game files.

Usage: python tests/fs_fill_inputs_check.py [--cc <clang>]
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MSYS_BIN = Path(r'C:\msys64\ucrt64\bin')

MESH_POINTS = '''#version 460
#extension GL_EXT_mesh_shader : require
layout(local_size_x = 1) in;
layout(points, max_vertices = 1, max_primitives = 1) out;
void main() {
    SetMeshOutputsEXT(1, 1);
    gl_MeshVerticesEXT[0].gl_Position = vec4(0.0, 0.0, 0.5, 1.0);
    gl_MeshVerticesEXT[0].gl_PointSize = 4.0;
    gl_PrimitivePointIndicesEXT[0] = 0;
}
'''
MESH_TRIANGLES = '''#version 460
#extension GL_EXT_mesh_shader : require
layout(local_size_x = 1) in;
layout(triangles, max_vertices = 3, max_primitives = 1) out;
layout(location = 1) out vec4 attr1[];
void main() {
    SetMeshOutputsEXT(3, 1);
    for (int i = 0; i < 3; i++) {
        gl_MeshVerticesEXT[i].gl_Position = vec4(float(i), 0.0, 0.5, 1.0);
        attr1[i] = vec4(1.0);
    }
    gl_PrimitiveTriangleIndicesEXT[0] = uvec3(0, 1, 2);
}
'''
FRAG = '''#version 460
layout(location = 1) in vec4 a1;
layout(location = 5) in vec4 a5;
layout(location = 0) out vec4 color;
void main() { color = a1 + vec4(a5.y, a5.x, 0.0, a5.w); }
'''

HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { SPV_OP_ENTRY_POINT = 15, SPV_OP_TYPE_INT = 21, SPV_OP_TYPE_FLOAT = 22, SPV_OP_TYPE_VECTOR = 23,
       SPV_OP_TYPE_POINTER = 32, SPV_OP_CONSTANT = 43, SPV_OP_CONSTANT_COMPOSITE = 44,
       SPV_OP_CONSTANT_NULL = 46, SPV_OP_FUNCTION = 54, SPV_OP_VARIABLE = 59,
       SPV_OP_DECORATE = 71, SPV_DEC_BUILTIN = 11, SPV_DEC_LOCATION = 30, SPV_SC_OUTPUT = 3 };
@BODY@
static uint32_t *rd(const char *p, uint64_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) exit(2);
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *w = malloc((size_t)l);
    if (fread(w, 1, (size_t)l, f) != (size_t)l) exit(2);
    fclose(f);
    *n = (uint64_t)l / 4;
    return w;
}
int main(int argc, char **argv) {   // mesh.spv frag.spv out.spv: prints "<mask> <points>", writes the rewrite
    (void)argc;
    uint64_t mn, fn, on = 0;
    uint32_t *m = rd(argv[1], &mn), *fr = rd(argv[2], &fn);
    int points = 0;
    uint32_t have = spirv_mesh_outputs(m, mn, &points);
    uint32_t *o = spirv_fs_fill_inputs(fr, fn, ((1u << 1) | (1u << 5)) & ~have, points, &on);
    printf("%u %d\n", have, points);
    if (!o) return 1;
    FILE *f = fopen(argv[3], "wb");
    fwrite(o, 4, on, f);
    fclose(f);
    return 0;
}
'''


def inputs_and_builtins(spv):
    w = struct.unpack('<%dI' % (len(spv) // 4), spv)
    loc, builtin, sc = {}, {}, {}
    i = 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xFFFF
        if op == 71 and w[i + 2] == 30:
            loc[w[i + 1]] = w[i + 3]
        if op == 71 and w[i + 2] == 11:
            builtin[w[i + 1]] = w[i + 3]
        if op == 59:
            sc[w[i + 2]] = w[i + 3]
        i += n
    return {loc[v] for v, s in sc.items() if s == 1 and v in loc}, {builtin[v] for v, s in sc.items() if s == 1 and v in builtin}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', help='the C compiler (default: MSYS2 UCRT64 clang on Windows, clang elsewhere)')
    args = ap.parse_args()
    win = os.name == 'nt'
    cc = args.cc or (str(MSYS_BIN / 'clang.exe') if win and MSYS_BIN.exists() else 'clang')
    glslang = str(MSYS_BIN / 'glslangValidator.exe') if win and MSYS_BIN.exists() else shutil.which('glslangValidator')
    val = str(MSYS_BIN / 'spirv-val.exe') if win and MSYS_BIN.exists() else shutil.which('spirv-val')
    env = dict(os.environ)
    if win and MSYS_BIN.exists():
        env['PATH'] = str(MSYS_BIN) + os.pathsep + env.get('PATH', '')
    src = (ROOT / 'runtime' / 'vk_pipeline.c').read_text(encoding='utf-8')
    body = src[src.index('// The locations a mesh module writes'):src.index('// Two fixes for what the translator declares')]
    fails = 0
    with tempfile.TemporaryDirectory(prefix='nx2navi-fs-fill-') as tmp:
        t = Path(tmp)
        for name, text, stage in (('points', MESH_POINTS, 'mesh'), ('triangles', MESH_TRIANGLES, 'mesh'), ('frag', FRAG, 'frag')):
            (t / f'{name}.{stage}').write_text(text)
            subprocess.check_call([glslang, '-V', '--target-env', 'vulkan1.3', '-o', str(t / f'{name}.spv'),
                                   str(t / f'{name}.{stage}')], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        (t / 'check.c').write_text(HARNESS.replace('@BODY@', body), encoding='utf-8')
        exe = t / ('check.exe' if win else 'check')
        subprocess.check_call([cc, '-O2', '-w', str(t / 'check.c'), '-o', str(exe)], env=env)
        for mesh, want_mask, want_points in (('points', 0, 1), ('triangles', 1 << 1, 0)):
            r = subprocess.run([str(exe), str(t / f'{mesh}.spv'), str(t / 'frag.spv'), str(t / f'out_{mesh}.spv')],
                               capture_output=True, text=True, env=env)
            mask, points = (int(x) for x in r.stdout.split())
            if (mask, points) != (want_mask, want_points) or r.returncode:
                print(f'FAIL {mesh}: mask {mask:#x} points {points} (want {want_mask:#x} {want_points}), rewrite {r.returncode == 0}')
                fails += 1
                continue
            out = (t / f'out_{mesh}.spv').read_bytes()
            ins, builtins = inputs_and_builtins(out)
            want_ins = set() if mesh == 'points' else {1}
            if ins != want_ins or (mesh == 'points') != (16 in builtins):
                print(f'FAIL {mesh}: inputs {sorted(ins)} builtins {sorted(builtins)}')
                fails += 1
            v = subprocess.run([val, '--target-env', 'vulkan1.3', str(t / f'out_{mesh}.spv')], capture_output=True, text=True)
            if v.returncode:
                print(f'FAIL {mesh}: spirv-val: {(v.stdout + v.stderr).strip()}')
                fails += 1
    print('fs fill inputs: ok' if not fails else f'fs fill inputs: {fails} failures')
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
