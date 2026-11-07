"""[XCDE] LINUX-004/006: launcher plans cannot escape port state or bypass build receipts.

Synthetic ELF headers are never executed; these tests cover preflight only.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('run_linux', ROOT / 'scripts/run_linux.py')
launch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(launch)


@unittest.skipUnless(sys.platform == 'linux', 'native Linux launch preflight')
class LaunchPlanTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        self.workspace = Path(tmp.name)
        self.root = self.workspace / 'Monado_Linux'; self.root.mkdir()
        (self.workspace / 'game_files').mkdir()
        self.binary = self.root / 'build' / 'candidate' / 'xcde-linux'
        self.binary.parent.mkdir(parents=True)
        header = bytearray(64); header[:6] = b'\x7fELF\x02\x01'; header[18:20] = (62).to_bytes(2, 'little')
        self.binary.write_bytes(header); self.binary.chmod(0o700)
        self.receipt = self.binary.parent / 'receipt.json'
        self.sysroot = self.root / '.deps/graphics/sysroots/fixture'
        (self.sysroot / 'usr/lib/x86_64-linux-gnu').mkdir(parents=True)
        (self.sysroot / '.monado-deps.json').write_text(json.dumps({'lock_sha256': 'fixture-lock'}))
        self.document = {'status': 'passed', 'executable': {'path': str(self.binary),
                         'sha256': hashlib.sha256(header).hexdigest()},
                         'dependencies': {'sysroot': str(self.sysroot)},
                         'sources_before': {'graphics-deps-linux.lock.json': 'fixture-lock'}}
        self.receipt.write_text(json.dumps(self.document))
        self.state = self.root / 'runs' / 'session' / 'state'
        p = patch.object(launch, 'ROOT', self.root); p.start(); self.addCleanup(p.stop)

    def test_precompile_plan_uses_selected_input_and_creates_nothing(self):
        result = launch.plan(self.binary, self.state, True)
        self.assertEqual(result['argv'], [str(self.binary), '--state-root', str(self.state), '--precompile',
                                         str(self.workspace / 'game_files')])
        self.assertFalse(self.state.exists())
        self.assertEqual(result['library_path'], str(self.sysroot / 'usr/lib/x86_64-linux-gnu'))

    def test_wrong_sysroot_identity_rejected(self):
        (self.sysroot / '.monado-deps.json').write_text(json.dumps({'lock_sha256': 'different-lock'}))
        with self.assertRaisesRegex(ValueError, 'sysroot identity'): launch.plan(self.binary, self.state)

    def test_tampered_elf_rejected(self):
        with self.binary.open('ab') as stream: stream.write(b'changed')
        with self.assertRaisesRegex(ValueError, 'receipt'): launch.plan(self.binary, self.state)

    def test_failed_build_rejected(self):
        self.document['status'] = 'failed'; self.receipt.write_text(json.dumps(self.document))
        with self.assertRaisesRegex(ValueError, 'receipt'): launch.plan(self.binary, self.state)

    def test_windows_or_external_state_rejected(self):
        with self.assertRaisesRegex(ValueError, 'allowed Linux'): launch.plan(self.binary, self.workspace / 'Monado_Windows' / 'save')

    def test_state_parent_symlink_rejected(self):
        (self.root / 'runs').symlink_to(self.workspace / 'game_files', target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'allowed Linux'): launch.plan(self.binary, self.state)

    def test_state_cannot_contain_build(self):
        parent = self.root / 'runs' / 'output'; parent.mkdir(parents=True)
        binary = parent / self.binary.name; binary.write_bytes(self.binary.read_bytes()); binary.chmod(0o700)
        with self.assertRaisesRegex(ValueError, 'separate'): launch.plan(binary, parent)

    def test_wrong_architecture_rejected(self):
        data = bytearray(self.binary.read_bytes()); data[18:20] = (183).to_bytes(2, 'little'); self.binary.write_bytes(data)
        with self.assertRaisesRegex(ValueError, 'x86-64 ELF'): launch.plan(self.binary, self.state)


if __name__ == '__main__': unittest.main()
