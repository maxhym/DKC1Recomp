"""Compile and run the HD Donkey Kong layer's synthetic checks."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from c_build import compile_model

ROOT = Path(__file__).resolve().parents[1]


class HdKong(unittest.TestCase):
    def test_engine(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / ('hd' + ('.exe' if os.name == 'nt' else ''))
            compile_model([ROOT / 'tests/test_dkc1_hd.c', ROOT / 'runner/dkc1_hd.c'],
                          [ROOT / 'runner', ROOT / 'snesrecomp/runner/src'], executable)
            result = subprocess.run([str(executable)], check=True,
                                    capture_output=True, text=True)
            self.assertIn('dkc1 hd tests passed', result.stdout)

    def test_game_loop_drives_the_layer(self):
        game = (ROOT / 'runner/dkc1_game.c').read_text(encoding='utf-8')
        prepare = game.index('Dkc1HdPrepareFrame(g_ppu, g_ram, presentation_bias);')
        line = game.index('Dkc1HdObserveLine(g_ppu, line);')
        finish = game.index('Dkc1HdFinishFrame(g_ppu);')
        # The Baby Kong hook releases the OBJ overlay surface, so it runs first.
        self.assertLess(game.index('Dkc1BabyKongPrepareFrame(g_ppu'), prepare)
        self.assertLess(prepare, game.index('ppu_runLine(g_ppu, line);'))
        self.assertLess(game.index('ppu_runLine(g_ppu, line);'), line)
        self.assertLess(line, finish)


if __name__ == '__main__':
    unittest.main()
