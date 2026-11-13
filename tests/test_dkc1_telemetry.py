"""Compile and run the synthetic DKCountry telemetry bridge tests."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from c_build import compile_model

ROOT = Path(__file__).resolve().parents[1]


class Dkc1TelemetryTests(unittest.TestCase):
    def test_bridge_model(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / ('telemetry.exe' if os.name == 'nt' else 'telemetry')
            compile_model([ROOT / 'tests/test_dkc1_telemetry.c',
                           ROOT / 'runner/dkc1_telemetry.c'], [ROOT / 'runner'], executable)
            env = {k: v for k, v in os.environ.items() if k != 'DKCOUNTRY_TELEMETRY'}
            result = subprocess.run([str(executable)], cwd=directory, check=True,
                                    capture_output=True, text=True, env=env)
            self.assertIn('tests passed', result.stdout)

    def test_hosts_feed_the_bridge(self):
        game = (ROOT / 'runner/dkc1_game.c').read_text(encoding='utf-8')
        self.assertIn('Dkc1TelemetryHostFrame(g_ram', game)
        self.assertIn('Dkc1TelemetryHostStateLoaded()', game)
        for host in ('runner/sdl_host.c', 'runner/win32_host.c'):
            text = (ROOT / host).read_text(encoding='utf-8')
            self.assertIn('Dkc1TelemetryHostInput(input)', text, host)
        cmake = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
        self.assertIn('runner/dkc1_telemetry.c', cmake)


if __name__ == '__main__':
    unittest.main()
