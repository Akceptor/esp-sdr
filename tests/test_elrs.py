import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ['-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=undefined', '-O2']


@unittest.skipUnless(shutil.which('cc'), 'Host C compiler unavailable')
class ElrsHost(unittest.TestCase):
    def run_c(self, test_name, sources, args=()):
        with tempfile.TemporaryDirectory() as tmp:
            exe = str(Path(tmp) / test_name)
            cmd = ['cc', *FLAGS, '-I' + str(ROOT / 'main/elrs'), '-I' + str(ROOT / 'tests'),
                   str(ROOT / 'tests' / f'elrs_{test_name}_test.c'),
                   *[str(ROOT / 'main/elrs' / f'{s}.c') for s in sources], '-o', exe, '-lm']
            build = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([exe, *args], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            return run.stdout

    def test_cmd(self):
        self.run_c('cmd', ['cmd'])

    def test_ota3(self):
        self.run_c('ota3', ['ota3'])

    def test_fhss(self):
        self.run_c('fhss', ['fhss'])

    def test_dsp(self):
        self.run_c('dsp', ['dsp', 'ota3', 'fhss'], [str(ROOT / 'web/elrs/test/real_dk500.bin')])

    def test_decode(self):
        self.run_c('decode', ['dsp', 'ota3', 'fhss'], [str(ROOT / 'web/elrs/test/real_dk500.bin')])

    @unittest.skipUnless(os.environ.get('ELRS_SLOW', '1') != '0', 'slow test disabled')
    def test_decode_noise(self):
        self.run_c('decode', ['dsp', 'ota3', 'fhss'], ['--noise', '10000'])

    def test_caps(self):
        fx = ROOT / 'tests/fixtures/elrs'
        print(self.run_c('caps', ['dsp', 'ota3', 'fhss'], [str(fx / 'caps_rc.bin'), str(fx / 'caps_rc.json')]), end='')

    def test_caps_local(self):
        full = ROOT / 'tests/fixtures/elrs/local/caps_full.bin'
        if not full.exists():
            self.skipTest('local/caps_full.bin not present')
        print(self.run_c('caps', ['dsp', 'ota3', 'fhss'], ['--local', str(full)]), end='')


if __name__ == '__main__':
    unittest.main()
