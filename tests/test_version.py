"""Test version metadata without changing the application repository."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class VersionTest(unittest.TestCase):
    def test_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'source'
            template = source / 'src/core/version.h.in'
            template.parent.mkdir(parents=True)
            shutil.copyfile(ROOT / 'src/core/version.h.in', template)
            output = pathlib.Path(directory) / 'version.h'

            def git(*args):
                return subprocess.check_output(['git', '-c', 'commit.gpgsign=false', *args], cwd=source, stderr=subprocess.DEVNULL, text=True).strip()

            def version():
                subprocess.run(['cmake', f'-DSOURCE_DIR={source}', f'-DOUTPUT_FILE={output}', '-DBASE_VERSION=0.1.0', '-P', str(ROOT / 'cmake/version.cmake')], check=True, capture_output=True)
                return output.read_text()

            self.assertIn('0.1.0+unknown', version())
            git('init', '-q')
            git('config', 'user.name', 'Version test')
            git('config', 'user.email', 'version@example.invalid')
            git('add', '.')
            git('commit', '-qm', 'Add version template')
            first = git('rev-parse', 'HEAD')
            self.assertIn(f'0.1.0+g{first[:12]}"', version())
            timestamp = output.stat().st_mtime_ns
            version()
            self.assertEqual(output.stat().st_mtime_ns, timestamp)
            template.write_text(template.read_text() + '\n')
            self.assertIn('-dirty', version())
            git('commit', '-qam', 'Change template')
            second = git('rev-parse', 'HEAD')
            self.assertNotEqual(first, second)
            self.assertIn(second, version())
            git('tag', 'v0.1.0')
            self.assertIn('#define SWAK_GIT_TAG "v0.1.0"', version())
            git('tag', '-d', 'v0.1.0')
            self.assertIn('#define SWAK_GIT_TAG ""', version())


if __name__ == '__main__':
    unittest.main()
