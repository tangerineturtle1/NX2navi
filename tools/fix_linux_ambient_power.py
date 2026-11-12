#!/usr/bin/env python3
"""Apply the verified Ocarina 1.1.1 ambient-power workaround to a Linux shader cache file.

The affected shader evaluates exp2(log2(height) * exponent). A zero height
and zero exponent produce a NaN, which then contaminates ambient lighting
and volumetric history. Select a zero product when the exponent is zero.
This is a narrowly scoped compatibility workaround, not a claim of exact
NVIDIA instruction equivalence. It does not address exposure or memory use.
No game shader data is embedded in this tool.
"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

ORIGINAL_HASH = 0xA806B657C39684BD
PATCHED_HASH = 0x0A0281063D5EF5BD


def shader_hash(data):
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return value


def patch_text(source):
    pattern = re.compile(
        r'(?P<ptr>%\w+) = OpAccessChain %\w+ %cp_c3 %int_0 %int_2 %int_1\s+'
        r'(?P<exponent>%\w+) = OpLoad %float (?P=ptr)\s+'
        r'(?P<product>%\w+) = OpFMul %float (?P<logarithm>%\w+) (?P=exponent)\s+'
        r'(?P<result>%\w+) = OpExtInst %float %\w+ Exp2 (?P=product)'
    )
    matches = list(pattern.finditer(source))
    if len(matches) != 1:
        raise ValueError('Expected exactly one supported ambient-power expression')
    m = matches[0]
    old = f"{m['product']} = OpFMul %float {m['logarithm']} {m['exponent']}"
    replacement = (
        f"%linux_pow_product = OpFMul %float {m['logarithm']} {m['exponent']}\n"
        f"%linux_pow_zero_exponent = OpFOrdEqual %bool {m['exponent']} %float_0\n"
        f"{m['product']} = OpSelect %float %linux_pow_zero_exponent %float_0 %linux_pow_product"
    )
    if source.count(old) != 1:
        raise ValueError('The supported multiply instruction is not unique')
    return source.replace(old, replacement)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('shader', type=Path, help='Supported .spv shader cache file')
    destination = parser.add_mutually_exclusive_group(required=True)
    destination.add_argument('--output', type=Path)
    destination.add_argument('--in-place', action='store_true')
    args = parser.parse_args()
    original = args.shader.read_bytes()
    current = shader_hash(original)
    if current == PATCHED_HASH:
        if args.output:
            args.output.write_bytes(original)
        print(f'Already patched: {current:016x}')
        return
    if current != ORIGINAL_HASH:
        parser.error(f'Unsupported shader hash {current:016x}; file left unchanged')
    for command in ('spirv-dis', 'spirv-as', 'spirv-val'):
        if not shutil.which(command):
            parser.error(f'Required command not found: {command}')
    target = args.shader if args.in_place else args.output
    with tempfile.TemporaryDirectory(prefix='.ambient-power-', dir=target.parent) as work:
        work = Path(work)
        assembly = work / 'shader.spvasm'
        binary = work / 'shader.spv'
        subprocess.run(['spirv-dis', str(args.shader), '-o', str(assembly)], check=True)
        assembly.write_text(patch_text(assembly.read_text()))
        subprocess.run(['spirv-as', '--target-env', 'vulkan1.1', str(assembly), '-o', str(binary)], check=True)
        subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', str(binary)], check=True)
        patched = binary.read_bytes()
        result = shader_hash(patched)
        if result != PATCHED_HASH:
            raise ValueError(f'Unexpected assembled shader hash {result:016x}; file left unchanged')
        binary.chmod(args.shader.stat().st_mode & 0o777)
        binary.replace(target)
    print(f'Patched {target}: {current:016x} -> {result:016x}; Vulkan 1.1 validation passed')


if __name__ == '__main__':
    main()
