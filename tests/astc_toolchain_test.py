"""Cold ASTC builds use the selected launcher toolchain without a system Clang."""
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import ui_textures
import world_map_night
import world_extra_art
import world_map_art
import make_game


class StopPreparation(Exception):
    pass


class AstcToolchainTest(unittest.TestCase):
    def test_cold_decoder_uses_explicit_compiler_and_environment(self):
        with tempfile.TemporaryDirectory(prefix='launcher toolchain ') as folder:
            out=Path(folder)
            compiler=out/'toolchain/ucrt64/bin/clang.exe'
            env={'PATH':str(compiler.parent)}
            with mock.patch.object(ui_textures.shutil,'which',side_effect=AssertionError('must not search PATH')), \
                 mock.patch.object(ui_textures.subprocess,'run',return_value=mock.Mock(returncode=0)) as run, \
                 mock.patch.object(ui_textures.ctypes,'CDLL'):
                ui_textures.astc_library(out,compiler=compiler,env=env)
            command=run.call_args.args[0]
            self.assertEqual(command[0],compiler)
            self.assertEqual(command[-1],str(out/'astc.dll'))
            self.assertIs(run.call_args.kwargs['env'],env)
            self.assertIn(str(compiler.resolve().parents[1]/'include/vma'),command)

    def test_both_icon_generators_forward_the_toolchain(self):
        with tempfile.TemporaryDirectory() as folder:
            for generate in (world_map_night.texture_patch,world_extra_art.textures):
                compiler=Path(folder)/'private clang.exe';env={'PATH':'private toolchain'}
                with mock.patch.object(ui_textures,'astc_library',side_effect=StopPreparation) as library:
                    with self.assertRaises(StopPreparation):
                        generate(b'',Path(folder),compiler=compiler,env=env)
                library.assert_called_once_with(Path(folder),compiler=compiler,env=env)

    def test_compile_step_resolves_toolchain_before_preparing_world_art(self):
        with tempfile.TemporaryDirectory() as folder:
            out=Path(folder);compiler=out/'private clang.exe';env={'PATH':'private toolchain'}
            with mock.patch.object(make_game,'toolchain',return_value=(compiler,compiler,env,out)) as resolve, \
                 mock.patch.object(world_map_art,'prepare',side_effect=StopPreparation) as prepare:
                with self.assertRaises(StopPreparation):
                    make_game.step_compile(out,out/'build-cache',out/'msys',1)
            resolve.assert_called_once_with(out/'msys')
            prepare.assert_called_once_with(out,compiler=compiler,env=env)


if __name__=='__main__':
    unittest.main()
