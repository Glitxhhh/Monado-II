"""LINUX-042: authored ELF ABI fixtures; no game input or host package changes."""
from pathlib import Path
import importlib.util
import json
import os
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'Monado_Linux'


def module(name, file):
    spec = importlib.util.spec_from_file_location(name, file)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


GRAPH = module('runtime_graph_test', PORT / 'scripts/linux_runtime_graph.py')
SETUP = module('runtime_setup_test', PORT / 'scripts/setup_linux_graphics.py')


class RuntimeLockPolicy(unittest.TestCase):
    def test_complete_media_abi_policy_keeps_native_host_interfaces_explicit(self):
        lock = json.loads((PORT / 'graphics-deps-linux.lock.json').read_text())
        policy = lock['runtime']
        self.assertEqual(policy['schema'], 1)
        self.assertIn('libbluray.so.3', policy['owned_sonames'])
        self.assertEqual(policy['owned_sonames']['libbluray.so.3']['package'], 'libbluray3')
        self.assertNotIn('libbluray.so.3', policy['host_sonames'])
        self.assertEqual(len(policy['root_sonames']), 10)
        self.assertTrue(set(policy['root_sonames']) <= set(policy['owned_sonames']))
        packages = {item['package'] for item in lock['packages']}
        for item in policy['owned_sonames'].values():
            self.assertIn(item['package'], packages)
            self.assertIn(str(Path(item['path']).parent), policy['library_dirs'])
        self.assertFalse(packages & {'libc6', 'libstdc++6', 'libgcc-s1', 'libdrm2', 'libgbm1',
                                     'libasound2t64', 'libpulse0', 'libdecor-0-0'})
        self.assertFalse(set(policy['host_sonames']) & set(policy['owned_sonames']))
        for name in ('libc.so.6', 'libstdc++.so.6', 'libgbm.so.1', 'libasound.so.2'):
            self.assertTrue(policy['host_sonames'][name]['reason'])


class RuntimeGraph(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        local = PORT / '.deps/bin/clang-cc'
        cls.compiler = os.environ.get('MONADO_TEST_CC') or (str(local) if local.is_file() else shutil.which('clang'))
        if not cls.compiler or not Path(GRAPH.READELF).is_file():
            raise unittest.SkipTest('Clang and readelf are required for authored ELF fixtures')

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='monado runtime ABI ')
        self.addCleanup(self.temporary.cleanup)
        self.base = Path(self.temporary.name)
        self.sysroot = self.base / 'private'
        self.lib = self.sysroot / 'lib'
        self.lib.mkdir(parents=True)
        self.host = self.base / 'native'
        self.host.mkdir()
        self.child = self.build(self.lib, 'libchild.so.3', 'int child(void){return 3;}',
                                'libchild.so.3', 'LIBCHILD_1')
        self.parent = self.build(self.lib, 'libparent.so.1',
                                 'extern int child(void); int parent(void){return child();}',
                                 'libparent.so.1', dependency='libchild.so.3')
        self.policy = {'schema': 1, 'root_sonames': ['libparent.so.1'],
                       'library_dirs': ['lib'], 'host_sonames': {},
                       'owned_sonames': {'libparent.so.1': {'path': 'lib/libparent.so.1', 'package': 'parent'},
                                         'libchild.so.3': {'path': 'lib/libchild.so.3', 'package': 'child'}}}

    def build(self, directory, name, text, soname=None, version=None, dependency=None):
        source = directory / (name + '.c')
        source.write_text(text)
        output = directory / name
        command = [self.compiler, '-shared', '-fPIC', '-nostdlib', str(source), '-o', str(output)]
        if soname:
            command += ['-Wl,-soname,' + soname]
        if version:
            script = directory / (name + '.map')
            script.write_text(version + ' { global: child; local: *; };\n')
            command += ['-Wl,--version-script=' + str(script)]
        if dependency:
            command += [str(self.lib / dependency), '-Wl,-rpath,$ORIGIN']
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return output

    def audit(self, policy=None, host=None, roots=None):
        return GRAPH.audit_closure(roots or [self.parent], self.sysroot, policy or self.policy, host or {})

    def test_exact_native_abi_and_recursive_versions_pass_with_spaces(self):
        metadata = GRAPH.inspect_elf(self.parent)
        self.assertEqual(metadata['needed'], ['libchild.so.3'])
        self.assertEqual(metadata['required_versions'], {'libchild.so.3': ['LIBCHILD_1']})
        self.assertIn('LIBCHILD_1', GRAPH.inspect_elf(self.child)['defined_versions'])
        result = self.audit()
        self.assertTrue(result['passed'], result['errors'])
        self.assertEqual(len(result['objects']), 2)
        self.assertEqual(result['host_requirements'], {})

    def test_missing_so3_does_not_use_available_so4_or_ambient_library_path(self):
        self.child.rename(self.lib / 'libchild.so.4')
        shutil.copyfile(self.lib / 'libchild.so.4', self.host / 'libchild.so.3')
        before = os.environ.get('LD_LIBRARY_PATH')
        os.environ['LD_LIBRARY_PATH'] = str(self.host)
        try:
            result = self.audit()
        finally:
            if before is None:
                os.environ.pop('LD_LIBRARY_PATH')
            else:
                os.environ['LD_LIBRARY_PATH'] = before
        self.assertFalse(result['passed'])
        self.assertEqual(result['errors'][0]['kind'], 'owned_missing')
        self.assertEqual(result['errors'][0]['chain'][-1], 'libchild.so.3')

    def test_same_filename_with_wrong_major_or_version_is_refused(self):
        self.build(self.lib, 'libchild.so.3', 'int child(void){return 4;}', 'libchild.so.4', 'LIBCHILD_1')
        self.assertTrue(any(error['kind'] == 'provider_abi' for error in self.audit()['errors']))
        self.build(self.lib, 'libchild.so.3', 'int child(void){return 3;}', 'libchild.so.3', 'LIBCHILD_2')
        result = self.audit()
        self.assertFalse(result['passed'])
        self.assertEqual(result['errors'][0]['missing_versions'], ['LIBCHILD_1'])

    def test_undeclared_transitive_name_has_full_requester_chain(self):
        self.policy['owned_sonames'].pop('libchild.so.3')
        result = self.audit()
        self.assertFalse(result['passed'])
        self.assertEqual(result['errors'][0]['kind'], 'undeclared_dependency')
        self.assertEqual(result['errors'][0]['chain'], [str(self.parent), 'libchild.so.3'])

    def test_owned_provider_cannot_escape_through_symlink(self):
        outside = self.host / 'libchild.so.3'
        self.child.rename(outside)
        self.child.symlink_to(outside)
        result = self.audit()
        self.assertFalse(result['passed'])
        self.assertEqual(result['errors'][0]['kind'], 'owned_missing')

    def test_owned_root_cannot_escape_through_an_explicit_symlink(self):
        outside = self.host / self.parent.name
        self.parent.rename(outside)
        self.parent.symlink_to(outside)
        with self.assertRaisesRegex(ValueError, 'owned root provider escapes'):
            self.audit()
        lock = {'runtime': self.policy, 'packages': [{'package': 'parent'}, {'package': 'child'}]}
        with self.assertRaisesRegex(ValueError, 'owned root provider escapes'):
            SETUP.validate_private_runtime(self.sysroot, lock)

    def test_explicit_native_host_interface_validates_exact_versions(self):
        self.policy['owned_sonames'].pop('libchild.so.3')
        self.policy['host_sonames']['libchild.so.3'] = {'category': 'fixture-interface', 'reason': 'Authored host ABI boundary.'}
        self.assertEqual(self.audit()['errors'][0]['kind'], 'host_missing')
        self.assertEqual(self.audit(host={'libchild.so.3': self.child})['errors'][0]['kind'], 'host_missing')
        native = self.host / 'libchild.so.3'
        self.child.rename(native)
        result = self.audit(host={'libchild.so.3': native})
        self.assertTrue(result['passed'], result['errors'])
        self.assertEqual(result['host_requirements']['libchild.so.3']['required_versions'], ['LIBCHILD_1'])
        self.build(self.host, 'libchild.so.3', 'int child(void){return 3;}', 'libchild.so.3', 'LIBCHILD_2')
        self.assertEqual(self.audit(host={'libchild.so.3': native})['errors'][0]['kind'], 'version_missing')

    def test_native_architecture_and_duplicate_root_abi_are_checked(self):
        raw = bytearray(self.child.read_bytes())
        raw[18:20] = (183).to_bytes(2, 'little')
        self.child.write_bytes(raw)
        self.assertEqual(self.audit()['errors'][0]['kind'], 'provider_abi')
        self.build(self.lib, 'libchild.so.3', 'int child(void){return 3;}', 'libchild.so.3', 'LIBCHILD_1')
        duplicate = self.build(self.host, 'libchild.so.3', 'int child(void){return 3;}', 'libchild.so.3', 'LIBCHILD_1')
        with self.assertRaises(ValueError):
            self.audit(roots=[self.child, duplicate])

    def test_explicit_companion_with_no_soname_uses_only_its_exact_basename(self):
        self.build(self.lib, 'libchild.so.3', 'int child(void){return 3;}', version='LIBCHILD_1')
        self.policy['owned_sonames'].pop('libchild.so.3')
        result = self.audit(roots=[self.parent, self.child])
        self.assertTrue(result['passed'], result['errors'])
        self.assertEqual(result['edges'][0]['scope'], 'root')
        duplicate = self.build(self.host, 'libchild.so.3', 'int child(void){return 3;}', 'libchild.so.3', 'LIBCHILD_1')
        self.policy['owned_sonames']['libchild.so.3'] = {'path': 'lib/libchild.so.3', 'package': 'child'}
        with self.assertRaises(ValueError):
            self.audit(roots=[self.parent, duplicate])

    def test_setup_persists_policy_and_refuses_changed_private_bytes(self):
        lock = {'runtime': self.policy, 'packages': [{'package': 'parent'}, {'package': 'child'}]}
        marker = SETUP.validate_private_runtime(self.sysroot, lock)
        self.assertEqual(marker['runtime_policy'], self.policy)
        self.assertEqual(marker['runtime_policy_sha256'], SETUP.runtime_policy_digest(self.policy))
        self.assertEqual(len(marker['owned_runtime_files']), 2)
        self.assertEqual(SETUP.validate_private_runtime(self.sysroot, lock, marker), marker)
        self.child.write_bytes(self.child.read_bytes() + b'changed fixture bytes')
        with self.assertRaisesRegex(ValueError, 'changed'):
            SETUP.validate_private_runtime(self.sysroot, lock, marker)

    def test_setup_accepts_declared_host_boundary_but_never_missing_owned_abi(self):
        lock = {'runtime': self.policy, 'packages': [{'package': 'parent'}, {'package': 'child'}]}
        self.child.unlink()
        with self.assertRaisesRegex(ValueError, 'closure failed'):
            SETUP.validate_private_runtime(self.sysroot, lock)
        self.policy['owned_sonames'].pop('libchild.so.3')
        self.policy['host_sonames']['libchild.so.3'] = {'reason': 'Authored native ABI', 'category': 'fixture'}
        marker = SETUP.validate_private_runtime(self.sysroot, lock)
        self.assertEqual(marker['required_host_interfaces'], ['libchild.so.3'])
        self.assertEqual(len(marker['owned_runtime_files']), 1)


if __name__ == '__main__':
    unittest.main()
