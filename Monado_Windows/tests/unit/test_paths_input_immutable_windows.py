"""Exercise obsolete deletion entry points on authored disposable input only."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[2]


@unittest.skipUnless(sys.platform == 'win32', 'native Windows input boundary')
class WindowsInputBoundary(unittest.TestCase):
    def test_legacy_deletion_apis_refuse_even_the_old_app_owned_layout(self):
        prefix = Path(json.loads((PORT / '.deps/windows-deps.json').read_text(encoding='utf8'))['prefix'])
        with tempfile.TemporaryDirectory(prefix='immutable-input-', dir=PORT / 'build') as temporary:
            root = Path(temporary)
            selected = root / 'profile' / 'extracted'
            selected.mkdir(parents=True)
            (selected / 'romfs.bin').write_bytes(b'authored immutable input sentinel')
            (selected / 'exefs').mkdir()
            (selected / 'exefs' / 'main').write_bytes(b'authored fixture, no game data')
            source = root / 'fixture.c'
            source.write_text('#include "' + (PORT / 'runtime/paths.c').as_posix() + '"\n' + r'''
static unsigned removals;
bool plat_exe_path(char *out, size_t size) { (void)out; (void)size; return false; }
bool plat_remove_tree(const char *path) { (void)path; ++removals; return true; }
int main(int argc, char **argv) {
    if (argc != 2) return 10;
    if (snprintf(g_root, sizeof g_root, "%s/profile", argv[1]) >= (int)sizeof g_root) return 11;
    char extracted[2048];
    if (snprintf(extracted, sizeof extracted, "%s/extracted", g_root) >= (int)sizeof extracted) return 12;
    if (rt_remove_game_file(extracted, "romfs.bin")) return 13;
    if (rt_remove_game_file(extracted, "exefs")) return 14;
    if (rt_remove_game_files(extracted)) return 15;
    if (removals) return 16;
    return 0;
}
''', encoding='utf8')
            executable = root / 'authored-input-boundary.exe'
            build = subprocess.run([sys.executable, '-B', '-m', 'ziglang', 'cc', '-std=gnu11',
                '-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-fast-math',
                '-I' + str(prefix / 'include'), '-I' + str(PORT / 'runtime'), str(source),
                str(prefix / 'lib/libSDL3.dll.a'), '-o', str(executable)],
                capture_output=True, text=True, timeout=120)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            environment = dict(os.environ, PATH=str(prefix / 'bin') + os.pathsep + os.environ.get('PATH', ''))
            result = subprocess.run([str(executable), str(root)], env=environment,
                                    capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((selected / 'romfs.bin').read_bytes(), b'authored immutable input sentinel')
            self.assertEqual((selected / 'exefs' / 'main').read_bytes(), b'authored fixture, no game data')


if __name__ == '__main__':
    unittest.main()
