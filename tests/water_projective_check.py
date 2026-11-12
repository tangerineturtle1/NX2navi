"""The water corrections the runtime makes in memory (runtime/spirv_water.c) against the same steps of
tools/projective_recipe.py, on two made-up modules: a vertex stage that writes a position and a location 0 output, and
a fragment stage that reads a location 0 input without perspective. Each side's result must be the same words, and
valid SPIR-V. A module of another shape must be refused. Needs spirv-as and spirv-val (MSYS2 UCRT64 spirv-tools).
Usage: python tests/water_projective_check.py [--cc CLANG]"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tools'))
import projective_recipe as recipe  # noqa: E402

VERTEX = '''
               OpCapability Shader
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint Vertex %main "main" %out0 %pv %in0
               OpDecorate %out0 Location 0
               OpDecorate %in0 Location 0
               OpMemberDecorate %PerVertex 0 BuiltIn Position
               OpDecorate %PerVertex Block
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
      %float = OpTypeFloat 32
      %v4    = OpTypeVector %float 4
  %PerVertex = OpTypeStruct %v4
 %p_out_pv   = OpTypePointer Output %PerVertex
         %pv = OpVariable %p_out_pv Output
  %p_out_v4  = OpTypePointer Output %v4
       %out0 = OpVariable %p_out_v4 Output
   %p_in_v4  = OpTypePointer Input %v4
        %in0 = OpVariable %p_in_v4 Input
        %int = OpTypeInt 32 1
      %int_0 = OpConstant %int 0
       %main = OpFunction %void None %fn
      %entry = OpLabel
          %a = OpLoad %v4 %in0
          %p = OpAccessChain %p_out_v4 %pv %int_0
               OpStore %p %a
               OpStore %out0 %a
               OpReturn
               OpFunctionEnd
'''
FRAGMENT = '''
               OpCapability Shader
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main" %in0 %col
               OpExecutionMode %main OriginUpperLeft
               OpDecorate %in0 Location 0
               OpDecorate %in0 NoPerspective
               OpDecorate %col Location 0
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
      %float = OpTypeFloat 32
      %v4    = OpTypeVector %float 4
   %p_in_v4  = OpTypePointer Input %v4
   %p_in_f   = OpTypePointer Input %float
        %in0 = OpVariable %p_in_v4 Input
  %p_out_v4  = OpTypePointer Output %v4
        %col = OpVariable %p_out_v4 Output
        %int = OpTypeInt 32 1
      %int_1 = OpConstant %int 1
   %p_fn_f   = OpTypePointer Function %float
       %main = OpFunction %void None %fn
      %entry = OpLabel
        %tmp = OpVariable %p_fn_f Function
          %a = OpLoad %v4 %in0
         %py = OpAccessChain %p_in_f %in0 %int_1
          %y = OpLoad %float %py
               OpStore %tmp %y
               OpStore %col %a
               OpReturn
               OpFunctionEnd
'''
HARNESS = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "spirv_water.h"
int main(int argc, char **argv) {   // check <vertex|fragment> <in.spv> <out.spv>: 0 written, 3 refused
    (void)argc;
    FILE *f = fopen(argv[2], "rb");
    fseek(f, 0, SEEK_END);
    long bytes = ftell(f);
    rewind(f);
    uint32_t *w = malloc((size_t)bytes);
    if (fread(w, 1, (size_t)bytes, f) != (size_t)bytes) return 2;
    fclose(f);
    uint64_t n = 0;
    uint32_t *o = argv[1][0] == 'v' ? spirv_water_vertex(w, (uint64_t)bytes / 4, &n)
                                    : spirv_water_fragment(w, (uint64_t)bytes / 4, &n);
    if (!o) return 3;
    f = fopen(argv[3], "wb");
    fwrite(o, 4, (size_t)n, f);
    return fclose(f) != 0;
}
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cc', default='clang')
    a = ap.parse_args()
    tools = [shutil.which(t) for t in ('spirv-as', 'spirv-val')]
    if not all(tools):
        sys.exit('water projective: SKIP (no spirv-as and spirv-val on PATH)')
    out = root / 'build/test/water-projective'
    out.mkdir(parents=True, exist_ok=True)
    (out / 'check.c').write_text(HARNESS)
    exe = out / 'check.exe'
    subprocess.run([a.cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror', '-I', str(root / 'runtime'),
                    str(out / 'check.c'), str(root / 'runtime/spirv_water.c'), '-o', str(exe)], check=True)
    for name, text, edit in (('vertex', VERTEX, recipe.vertex), ('fragment', FRAGMENT, recipe.fragment)):
        src, mine = out / f'{name}.spv', out / f'{name}-runtime.spv'
        (out / f'{name}.spvasm').write_text(text)
        subprocess.run([tools[0], '--target-env', 'vulkan1.2', str(out / f'{name}.spvasm'), '-o', str(src)], check=True)
        subprocess.run([str(exe), name, str(src), str(mine)], check=True)
        want = edit(src.read_bytes())
        assert mine.read_bytes() == want, f'{name}: the runtime\'s module differs from the recipe\'s'
        subprocess.run([tools[1], '--target-env', 'vulkan1.2', str(mine)], check=True)
    # each edit refuses the other stage's module
    for name, other in (('vertex', 'fragment'), ('fragment', 'vertex')):
        rc = subprocess.run([str(exe), name, str(out / f'{other}.spv'), str(out / 'refused.spv')]).returncode
        assert rc == 3, f'the {name} edit took a {other} module (exit {rc})'
    print('water projective: the runtime\'s vertex and fragment edits match the recipe\'s, valid, other shapes refused')


if __name__ == '__main__':
    main()
