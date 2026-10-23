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


if __name__ == '__main__':
    unittest.main()
