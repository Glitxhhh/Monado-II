"""[XCDE] LINUX-006: preserve failures and reject stale/unsafe build inputs."""
import importlib.util
from pathlib import Path
import tempfile
import sys
import threading
import types
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('build_linux', ROOT / 'scripts/build_linux.py')
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


@unittest.skipUnless(sys.platform == "linux", "Linux build safety tests")
class LinuxBuildSafety(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix='monado-linux-build-')
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.out = self.root / 'build/run'
        (self.out / 'obj').mkdir(parents=True)
        self.source = self.root / 'runtime/example.c'
        self.source.parent.mkdir()
        self.source.write_text('void authored_example(void) {}\n')
        patch = mock.patch.object(build, 'ROOT', self.root)
        patch.start(); self.addCleanup(patch.stop)

    def builder(self, run):
        b = build.Builder.__new__(build.Builder)
        b.output = self.out
        b.args = types.SimpleNamespace(keep_going=False)
        b.failed = threading.Event()
        b.lock = threading.Lock()
        b.flags = []
        b.cc = b.cxx = Path('/unused/compiler')
        b.receipt = {'objects': {}, 'errors': []}
        b.run = run
        return b

    def test_failed_compiler_never_publishes_partial_object(self):
        def fail(name, argv):
            Path(argv[-1]).write_bytes(b'partial compiler output')
            return {'exit': 1}, ''
        b = self.builder(fail)
        self.assertIsNone(b.compile(self.source))
        self.assertTrue(b.failed.is_set())
        self.assertEqual(b.receipt['objects'], {})
        self.assertFalse(list((self.out / 'obj').glob('*.o')))
        self.assertTrue(list((self.out / 'obj').glob('*.partial')))

    def test_source_changed_during_compile_is_not_published(self):
        def changed(name, argv):
            Path(argv[-1]).write_bytes(b'object from old source')
            self.source.write_text('void changed_source(void) {}\n')
            return {'exit': 0}, ''
        b = self.builder(changed)
        self.assertIsNone(b.compile(self.source))
        self.assertTrue(b.failed.is_set())
        self.assertFalse(list((self.out / 'obj').glob('*.o')))

    def test_default_fail_fast_does_not_start_waiting_work(self):
        b = self.builder(mock.Mock(side_effect=AssertionError('compiler must not start')))
        b.failed.set()
        self.assertIsNone(b.compile(self.source))
        b.run.assert_not_called()

    def test_missing_compiler_records_failure_instead_of_running_state(self):
        b = self.builder(None)
        b.env = {}
        b.receipt['commands'] = []
        (self.out / 'logs').mkdir()
        result, _ = build.Builder.run(b, 'missing-tool', [self.root / 'does-not-exist'])
        self.assertEqual(result['exit'], 127)
        self.assertIn('spawn_error', result)
        self.assertTrue(b.failed.is_set())

    def test_existing_output_is_preserved(self):
        sentinel = self.out / 'accepted.bin'
        sentinel.write_bytes(b'accepted prior artifact')
        with self.assertRaises(FileExistsError):
            build.fresh_output(self.out)
        self.assertEqual(sentinel.read_bytes(), b'accepted prior artifact')

    def test_output_cannot_escape_linux_tree(self):
        with self.assertRaises(ValueError):
            build.fresh_output(self.root.parent / 'outside-monado-build')

    def test_generated_tables_and_chunks_required(self):
        path = self.root / 'generated/sample'
        for module in ('main', 'sdk'):
            (path / module).mkdir(parents=True)
            (path / module / (module + '_000.c')).write_text('// synthetic test input\n')
        with self.assertRaisesRegex(ValueError, 'function table'):
            build.generated_sources(path)
        for module in ('main', 'sdk'):
            (path / module / (module + '_table.c')).write_text('// synthetic test table\n')
        self.assertEqual(len(build.generated_sources(path)), 4)

    def test_generated_symlinks_rejected(self):
        path = self.root / 'generated/sample'
        for module in ('main', 'sdk'):
            (path / module).mkdir(parents=True)
            (path / module / (module + '_table.c')).write_text('// synthetic table\n')
            (path / module / (module + '_000.c')).write_text('// synthetic chunk\n')
        chunk = path / 'main/main_000.c'
        chunk.unlink(); chunk.symlink_to(self.source)
        with self.assertRaisesRegex(ValueError, 'unexpected generated source'):
            build.generated_sources(path)

    def test_link_rejects_non_native_output(self):
        def wrong_target(name, argv, **kwargs):
            Path(argv[-1]).write_bytes(b'MZ' + b'\0' * 62)
            return {'exit': 0}, ''
        b = self.builder(wrong_target)
        b.libs = []
        with self.assertRaisesRegex(RuntimeError, 'x86-64 ELF'):
            b.link()
        self.assertFalse((self.out / 'xcde-linux').exists())

    def test_correctness_macros_reject_disabled_assertions_and_fast_math(self):
        for forbidden in ('NDEBUG', '__FAST_MATH__'):
            with self.subTest(forbidden=forbidden), self.assertRaisesRegex(RuntimeError, 'assertions'):
                build.check_correctness_macros('#define ' + forbidden + ' 1\n')
        build.check_correctness_macros('#define __OPTIMIZE__ 1\n')


if __name__ == '__main__':
    unittest.main()
