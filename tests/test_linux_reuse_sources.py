"""Producer/consumer receipt compatibility; authored files, no game execution."""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'Monado_Linux'


def module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


build = module('monado_reuse_build_app', ROOT / 'tools/build_linux_app.py')
producer = module('monado_reuse_native_builder', PORT / 'scripts/build_linux.py')


def linux_source_snapshot():
    # This fixture models a Linux producer receipt, whose source keys use '/'.
    # The producer is Linux-only; normalize its host-native key spelling when
    # these metadata/source-contract tests run on a Windows CI host.
    return {name.replace('\\', '/'): value
            for name, value in producer.source_snapshot().items()}


class ReusedNativeSources(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='monado reuse source, with spaces ')
        self.addCleanup(temporary.cleanup)
        self.owner = Path(temporary.name).resolve()
        self.port = self.owner / 'Monado_Linux'
        self.port.mkdir()
        # Let the real native producer define the script/lock inputs. Do not
        # repeat the consumer's required-source list in a receipt fixture.
        for name in linux_source_snapshot():
            if name.startswith(('runtime/', 'third_party/')):
                continue
            destination = self.port / name
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(PORT / name, destination)
        for name in ('runtime/authored.c', 'third_party/authored.h'):
            path = self.port / name
            path.parent.mkdir(exist_ok=True)
            path.write_text('/* Authored receipt fixture; never compiled. */\n')
        with mock.patch.object(producer, 'ROOT', self.port):
            snapshot = linux_source_snapshot()
        self.input = self.owner / 'game_files'
        (self.input / 'exefs').mkdir(parents=True)
        (self.input / 'romfs').mkdir()
        input_file = self.input / 'exefs/authored.txt'
        input_file.write_bytes(b'AUTHORED INPUT SENTINEL; NOT GAME CONTENT')
        self.state = self.port / 'play/launcher'
        (self.state / 'save').mkdir(parents=True)
        (self.state / 'save/authored.txt').write_bytes(b'AUTHORED SAVE SENTINEL')
        (self.state / 'xcde.ini').write_bytes(b'quickstep_slot = 0\n')
        self.binary = self.port / 'build/authored/xcde-linux'
        self.binary.parent.mkdir(parents=True)
        self.binary.write_bytes(b'AUTHORED NEVER-EXECUTED BINARY SENTINEL')
        self.receipt = self.binary.parent / 'receipt.json'
        self.document = dict(status='passed', generated={'authored': {}}, errors=[],
            dlss_enabled=False, framegen_enabled=False,
            sources_before=snapshot, sources_after=dict(snapshot),
            input={'path': str(self.input), 'exefs': {'authored.txt': build.digest(input_file)}})
        self.save_receipt()
        self.planned = dict(input=str(self.input), build_port=str(self.port),
                            binary_sha256=build.digest(self.binary))

    def save_receipt(self):
        self.receipt.write_text(json.dumps(self.document))

    def inventory(self):
        return {p.relative_to(self.owner).as_posix():
                (hashlib.sha256(p.read_bytes()).hexdigest(), p.stat().st_mtime_ns)
                for p in self.owner.rglob('*') if p.is_file()}

    def validate(self):
        return build.validate_reused_game(self.port, self.binary, self.state,
                                          dlss=False, framegen=False)

    def test_current_native_producer_snapshot_can_be_reused(self):
        before = self.inventory()
        # The native ELF/runtime stage has its own real load-only bridge tests.
        # This fixture tests the independent source-closure contract before it.
        with mock.patch.object(build.run_linux, 'plan', return_value=self.planned) as plan:
            result = self.validate()
        plan.assert_called_once_with(self.binary, self.state, port=self.port)
        self.assertEqual(result['path'], str(self.binary))
        self.assertEqual(result['sha256'], self.planned['binary_sha256'])
        self.assertEqual(self.inventory(), before)

    def test_changed_runtime_checker_rejects_reuse_before_native_plan(self):
        helper = self.port / 'scripts/linux_runtime_check.py'
        helper.write_bytes(helper.read_bytes() + b'\n# Authored changed-source sentinel.\n')
        before = self.inventory()
        with mock.patch.object(build.run_linux, 'plan') as plan:
            with self.assertRaisesRegex(build.BuildError, 'native source differs: scripts/linux_runtime_check.py'):
                self.validate()
        plan.assert_not_called()
        self.assertEqual(self.inventory(), before)

    def test_incomplete_current_receipt_is_not_accepted_as_legacy(self):
        for key in ('sources_before', 'sources_after'):
            del self.document[key]['scripts/linux_runtime_graph.py']
        self.save_receipt()
        before = self.inventory()
        with mock.patch.object(build.run_linux, 'plan') as plan:
            with self.assertRaisesRegex(build.BuildError, 'native source closure differs'):
                self.validate()
        plan.assert_not_called()
        self.assertEqual(self.inventory(), before)


if __name__ == '__main__':
    unittest.main()
