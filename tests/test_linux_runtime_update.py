"""Actual load-only updater bridge; tiny authored ELF, no game or profile run."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'Monado_Linux'
_import_path = list(sys.path)
sys.path.insert(0, str(ROOT / 'tools'))
try:
    import update_linux as updater
finally:
    sys.path[:] = _import_path
spec = importlib.util.spec_from_file_location('update_bridge_runtime', PORT / 'scripts/linux_runtime_check.py')
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)


@unittest.skipUnless(sys.platform == 'linux', 'actual native Linux updater loading')
class RuntimeUpdateBridge(unittest.TestCase):
    def setUp(self):
        compiler = PORT / '.deps/bin/clang-cc'
        self.cc = str(compiler) if compiler.is_file() else shutil.which('clang')
        if not self.cc:
            self.skipTest('Clang is required for authored native updater fixtures')
        temp = tempfile.TemporaryDirectory(prefix='monado update runtime, with spaces ')
        self.addCleanup(temp.cleanup)
        self.base = Path(temp.name).resolve()
        self.owner = self.base / 'owner'
        self.old = self.owner / 'Monado_Linux'
        self.workspace = self.old / 'updates/bridge/workspace'
        self.new = self.workspace / 'Monado_Linux'
        (self.owner / 'game_files').mkdir(parents=True)
        self.new.mkdir(parents=True)
        (self.workspace / 'game_files').symlink_to(self.owner / 'game_files', target_is_directory=True)
        (self.new / 'scripts').mkdir()
        shutil.copyfile(PORT / 'scripts/install_layout.py', self.new / 'scripts/install_layout.py')
        self.state = self.old / 'play/launcher'
        (self.state / 'save').mkdir(parents=True)
        (self.state / 'save/authored-slot').write_bytes(b'AUTHORED SAVE SENTINEL')
        (self.state / 'xcde.ini').write_bytes(b'fps = 120\n')
        (self.owner / 'update-version.json').write_bytes(b'{"version":"0.1.9"}\n')
        (self.old / 'updates/active.json').write_bytes(b'AUTHORED ACTIVE SENTINEL')
        (self.old / 'updates/previous.json').write_bytes(b'AUTHORED PREVIOUS SENTINEL')
        self.protected = self.snapshot()
        self.constructor = self.base / 'constructor-must-not-run'

    def snapshot(self):
        files = [*self.state.rglob('*'), self.owner / 'update-version.json',
                 self.old / 'updates/active.json', self.old / 'updates/previous.json']
        return {str(p): (p.read_bytes(), p.stat().st_mtime_ns) for p in files if p.is_file()}

    def compile(self, arguments):
        result = subprocess.run([self.cc, *map(str, arguments)], capture_output=True, text=True,
                                timeout=60, env=check.clean_environment())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def runtime(self, port, *, legacy=False):
        libdir = port / '.deps/graphics/sysroots/authored/usr/lib/x86_64-linux-gnu'
        libdir.mkdir(parents=True)
        sysroot = libdir.parents[2]
        child = libdir / 'libmonado_update_fixture.so.3'
        source = port / 'authored-library.c'
        source.write_text('#include <stdio.h>\n'
            '__attribute__((constructor)) static void guard(void){FILE*f=fopen(' +
            json.dumps(str(self.constructor)) + ',"w");if(f)fclose(f);}\n'
            'int authored_dependency(void){return 7;}\n')
        self.compile(['-shared', '-fPIC', source, '-Wl,-soname,' + child.name, '-o', child])
        names = check.graph_module().inspect_elf(child)['needed']
        policy = {'schema': 1, 'root_sonames': [child.name],
            'library_dirs': ['usr/lib/x86_64-linux-gnu'],
            'owned_sonames': {child.name: {'path': str(child.relative_to(sysroot)), 'package': 'authored'}},
            'host_sonames': {name: {'reason': 'Authored fixture host boundary', 'category': 'platform'} for name in names}}
        lock = port / 'graphics-deps-linux.lock.json'
        lock.write_text(json.dumps({'schema': 1, 'runtime': policy}))
        marker = {'lock_sha256': check.digest(lock)}
        if not legacy:
            marker.update(runtime_policy=policy, runtime_policy_sha256=check.policy_digest(policy),
                owned_runtime_files={child.name: {'path': str(child.relative_to(sysroot)),
                    'sha256': check.digest(child), 'bytes': child.stat().st_size}})
        (sysroot / '.monado-deps.json').write_text(json.dumps(marker))
        return sysroot, child, None if legacy else policy, marker['lock_sha256']

    def executable(self, port, name, runtime):
        sysroot, child, policy, lock_sha = runtime
        target = port / 'build' / name / ('monado-launcher' if name == 'gui' else 'xcde-linux')
        target.parent.mkdir(parents=True)
        source = target.parent / 'authored-main.c'
        source.write_text('extern int authored_dependency(void); int main(void){return authored_dependency();}\n')
        self.compile([source, child, '-Xlinker', '-rpath', '-Xlinker', str(child.parent), '-o', target])
        receipt = {'status': 'passed', 'executable': {'path': str(target), 'sha256': check.digest(target)},
            'sources_before': {'graphics-deps-linux.lock.json': lock_sha},
            'dependencies': {'sysroot': str(sysroot)}, 'dlss_enabled': False, 'framegen_enabled': False,
            'runtime_validation': check.check_artifacts([target], sysroot, policy)}
        (target.parent / 'receipt.json').write_text(json.dumps(receipt))
        return target

    def selection(self, game_port, gui, game):
        (self.workspace / 'installation.json').write_text(json.dumps({'schema': 1,
            'owner': str(self.owner), 'workspace': str(self.workspace), 'game_port': str(game_port)}))
        pair = {'launcher': str(gui.relative_to(self.workspace)),
                'game': str(game.relative_to(game_port.parent))}
        with mock.patch.object(updater, 'pair', return_value=pair):
            result = updater.verify_runtime_selection(self.workspace, self.state)
        self.assertEqual(self.snapshot(), self.protected)
        self.assertFalse(self.constructor.exists())
        return result, pair

    def test_new_gui_and_new_game_use_real_strict_policy_checks(self):
        runtime = self.runtime(self.new)
        gui = self.executable(self.new, 'gui', runtime)
        game = self.executable(self.new, 'game', runtime)
        result, pair = self.selection(self.new, gui, game)
        self.assertEqual([x['artifacts'][0]['path'] for x in result['checks']], [str(gui), str(game)])
        self.assertTrue(all(x['policy'] == 'closed-owned-with-explicit-host-boundaries' for x in result['checks']))
        damaged = bytearray(game.read_bytes())
        damaged[18:20] = (183).to_bytes(2, 'little')
        game.write_bytes(damaged)
        path = game.parent / 'receipt.json'
        receipt = json.loads(path.read_text())
        receipt['executable']['sha256'] = check.digest(game)
        path.write_text(json.dumps(receipt))
        with mock.patch.object(updater, 'pair', return_value=pair), self.assertRaisesRegex(ValueError, 'x86-64 ELF'):
            updater.verify_runtime_selection(self.workspace, self.state)
        self.assertEqual(self.snapshot(), self.protected)
        self.assertFalse(self.constructor.exists())

    def test_new_gui_can_recheck_reused_legacy_game_then_refuses_missing_transitive(self):
        gui = self.executable(self.new, 'gui', self.runtime(self.new))
        old_runtime = self.runtime(self.old, legacy=True)
        game = self.executable(self.old, 'game', old_runtime)
        result, pair = self.selection(self.old, gui, game)
        self.assertEqual([x['artifacts'][0]['path'] for x in result['checks']], [str(gui), str(game)])
        self.assertEqual([x['policy'] for x in result['checks']],
                         ['closed-owned-with-explicit-host-boundaries', 'legacy-loader-list-only'])
        self.assertIn(str(old_runtime[0]), result['checks'][1]['library_path'])
        old_runtime[1].unlink()
        with mock.patch.object(updater, 'pair', return_value=pair), self.assertRaisesRegex(ValueError, 'loader check failed'):
            updater.verify_runtime_selection(self.workspace, self.state)
        self.assertEqual(self.snapshot(), self.protected)
        self.assertFalse(self.constructor.exists())


if __name__ == '__main__':
    unittest.main()
