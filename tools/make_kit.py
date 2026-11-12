"""Assemble the build kit: what a player needs to build Ocarina of Time on their own PC with tools/make_game.py.

    python tools/make_kit.py --xlate <dir with nx2navi_xlate.exe and .id> [--ffx amd_fidelityfx_vk.dll]
                             [--ffx4 FOLDER]
                             [--dlss nvngx_dlss.dll | --no-dlss] [--dlssg nx2navi_dlssg.dll [--streamline SDK]]
                             [--prompt-art FOLDER [FOLDER ...]] [--out build/kit] [--zip]

The kit holds this repository's runtime/ with its shaders compiled (tools/build_shaders.py builds the SPIR-V headers
here, so a player's PC needs no shader compiler; their sources go along), recomp/, the build tools, the shader
translator built from tools/xlate,
NVIDIA's DLSS library with its notice (tools/dlss_pin.py: downloaded from NVIDIA's DLSS SDK repository and checked,
or --dlss; not with --no-dlss), DLSS Frame Generation in bin/dlssg/ with --dlssg (nx2navi_dlssg.dll from
tools/build_dlssg.py beside NVIDIA Streamline's runtime, tools/streamline_pin.py: downloaded from NVIDIA's Streamline
SDK release and checked, or --streamline), AMD's FSR 3.1 library with --ffx, its two FSR 4 libraries with --ffx4, the
button art with --prompt-art, and README-BuildKit.md; no game code, data or anything generated from the game.
check_content runs over the folder before it is zipped.

--prompt-art names folders that hold XGamepad, P5Gamepad and Keyboard_Mouse, or those three folders themselves: the
pictures tools/pad_prompts.py reads go into the kit's prompt-art folder (tools/prompt_art.py art_files), and
make_game.py draws the Xbox, PlayStation, Steam Deck and keyboard and mouse prompts from them. Without it the kit builds the game
with the game's own prompts only.
"""
import argparse
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NAME = 'NX2navi-BuildKit'
TOOLS = ('make_game.py', 'nup.py', 'romfs_index.py', 'check_content.py', 'hostmem.py', 'prepare_shaders.py',
         'shader_census.py', 'shader_list.py', 'pipeline_list.py', 'zstd_compat.py', 'pad_prompts.py', 'prompt_art.py', 'button_icons.py', 'romfs_check.py',
         'build_game.py', 'check_isa.py', 'build_shaders.py', 'make_dlaa_mask.py', 'make_mvobj_fs.py',
         'pack_shader_cache.py', 'vulkan_pin.py', 'mod_files.py', 'cheats.py', 'exefs_patches.py')
# make_game.py imports build_game's vma_include and vulkan_include on Linux for the VMA header and for Vulkan headers
# newer than the system's (which build_game takes from vulkan_pin), and build_game.py imports check_isa and
# build_shaders, which reads the two make_ scripts' shaders; the lift runs cheats.py and exefs_patches.py for the mods'
# code, and both import mod_files.py
TOP = ('LICENSE', 'LICENSE-ADDITIONS', 'THIRD_PARTY_NOTICES.md', 'VERSION', 'nx2navi.ini.example')
SKIP_DIRS = {'__pycache__', '.pytest_cache'}
SKIP_SUFFIXES = {'.o', '.obj', '.pyc', '.exe', '.dll', '.a', '.lib'}


def copy_tree(src, dst):
    for p in sorted(src.rglob('*')):
        rel = p.relative_to(src)
        if any(part in SKIP_DIRS for part in rel.parts) or p.is_dir() or p.suffix.lower() in SKIP_SUFFIXES:
            continue
        (dst / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(p, dst / rel)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--xlate', type=Path, required=True, help='folder with nx2navi_xlate.exe and nx2navi_xlate.id')
    ap.add_argument('--ffx', type=Path, help="AMD's signed amd_fidelityfx_vk.dll (FidelityFX SDK 1.1.4, MIT) for FSR 3.1")
    ap.add_argument('--ffx4', type=Path, help="the folder with AMD's signed amd_fidelityfx_loader_vk.dll and "
                                              'amd_fidelityfx_upscaler_vk.dll (FSR SDK 2.4.0, MIT) for FSR 4, and its '
                                              'amd_fidelityfx_framegeneration_vk.dll when it is there')
    ap.add_argument('--dlss', type=Path, help="NVIDIA's nvngx_dlss.dll from disk instead of downloading it from "
                                               "NVIDIA's DLSS SDK repository")
    ap.add_argument('--no-dlss', action='store_true', help='leave NVIDIA DLSS out of the kit')
    ap.add_argument('--dlssg', type=Path, metavar='DLL',
                    help='nx2navi_dlssg.dll (tools/build_dlssg.py) for DLSS Frame Generation')
    ap.add_argument('--streamline', type=Path, metavar='SDK',
                    help='the Streamline SDK folder instead of downloading the SDK release with --dlssg')
    ap.add_argument('--prompt-art', type=Path, nargs='+', metavar='FOLDER',
                    help='the button art: folders holding XGamepad, P5Gamepad and Keyboard_Mouse, or those folders')
    ap.add_argument('--pipeline-list', type=Path, metavar='FILE',
                    help='the shared pipeline list to carry (tools/pipeline_list.py merge writes it; it is derived from game data and stays out of the repository)')
    ap.add_argument('--out', type=Path, default=ROOT / 'build' / 'kit')
    ap.add_argument('--zip', action='store_true')
    a = ap.parse_args()
    sys.path.insert(0, str(ROOT / 'tools'))
    import build_shaders
    import dlss_pin
    import prompt_art
    import streamline_pin
    if a.streamline and not a.dlssg:
        sys.exit('--streamline goes with --dlssg')
    if a.prompt_art:
        prompt_art.art_files(a.prompt_art)   # the art is all there before anything is downloaded
    dlss = None if a.no_dlss else dlss_pin.resolve(a.dlss, ROOT / 'build' / 'dl')
    sdk = streamline_pin.resolve(a.streamline, ROOT / 'build' / 'dl') if a.dlssg else None
    kit = a.out / NAME
    if kit.exists():
        shutil.rmtree(kit)
    kit.mkdir(parents=True)
    build_shaders.build(ROOT / 'runtime')   # the kit carries the shader headers: build the missing and the stale ones
    for d in ('runtime', 'recomp'):
        copy_tree(ROOT / d, kit / d)
    (kit / 'tools').mkdir()
    for t in TOOLS:
        shutil.copy2(ROOT / 'tools' / t, kit / 'tools' / t)
    for t in TOP:
        if (ROOT / t).is_file():
            shutil.copy2(ROOT / t, kit / t)
    shutil.copy2(ROOT / 'docs' / 'README-BuildKit.md', kit / 'README-BuildKit.md')
    (kit / 'bin').mkdir()
    for n in ('nx2navi_xlate.exe', 'nx2navi_xlate.id'):
        shutil.copy2(a.xlate / n, kit / 'bin' / n)
    if a.ffx:   # FSR 3.1 loads it from beside the game (runtime/vk_ffx.cpp); its licence is runtime/third_party/ffx_api
        shutil.copy2(a.ffx, kit / 'bin' / 'amd_fidelityfx_vk.dll')
    if a.ffx4:   # FSR 4 loads them from beside the game (runtime/vk_ffx4.cpp); their licence is runtime/third_party/ffx_sdk
        for n in ('amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_upscaler_vk.dll'):
            shutil.copy2(a.ffx4 / n, kit / 'bin' / n)
        if (a.ffx4 / 'amd_fidelityfx_framegeneration_vk.dll').is_file():   # its frame generation (runtime/vk_fg4.cpp)
            shutil.copy2(a.ffx4 / 'amd_fidelityfx_framegeneration_vk.dll', kit / 'bin' / 'amd_fidelityfx_framegeneration_vk.dll')
    if dlss:   # DLSS: NGX loads it from beside the game (runtime/vk_dlaa.c); make_game.py copies it there
        dlss_pin.place(dlss, kit / 'bin')
    if sdk:   # DLSS Frame Generation (runtime/vk_dlssg.c): make_game.py place_dlssg copies it beside the game
        streamline_pin.place(a.dlssg, sdk, kit / 'bin' / 'dlssg')
    if a.prompt_art:   # make_game.py draws the button prompts from it (its prompts step)
        n, size = prompt_art.copy_art(a.prompt_art, kit / prompt_art.FOLDER)
        print(f'button art: {n} pictures, {size / 1e6:.1f} MB')
    if a.pipeline_list:   # prepare_shaders.py reads it there (BUNDLED_PIPELINES)
        import pipeline_list
        if pipeline_list.unpack(a.pipeline_list.read_bytes()) is None:
            sys.exit(f'{a.pipeline_list} is not a pipeline list (tools/pipeline_list.py merge writes one)')
        (kit / 'tools' / 'data').mkdir(exist_ok=True)
        shutil.copy2(a.pipeline_list, kit / 'tools' / 'data' / 'pipeline_list.bin')
    import check_content
    bad = [f'{p.relative_to(kit)}: {m}' for p in sorted(kit.rglob('*')) if p.is_file()
           for m in check_content.problems_for(p, allow_large=p.suffix in ('.exe', '.dll'))]   # the translator, the DLLs
    if bad:
        sys.exit('check_content refuses these kit files, not zipped:\n  ' + '\n  '.join(bad[:20]))
    size = sum(p.stat().st_size for p in kit.rglob('*') if p.is_file())
    print(f'{kit}: {size / 1e6:.1f} MB')
    if a.zip:
        z = a.out / f'{NAME}.zip'
        with zipfile.ZipFile(z, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
            for p in sorted(kit.rglob('*')):
                if p.is_file():
                    zf.write(p, Path(NAME) / p.relative_to(kit))
        print(f'{z}: {z.stat().st_size / 1e6:.1f} MB')


if __name__ == '__main__':
    main()
