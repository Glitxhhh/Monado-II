"""Authored launcher preparation fixtures; no SDK, Proton, models or GPU run."""
import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'Monado_Linux'

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module

sys.path.insert(0, str(PORT / 'scripts'))
try:
    worker = load('fixture_fsr4_worker', PORT / 'scripts/launcher_fsr4_worker.py')
    prep = load('fixture_fsr4_broker', PORT / 'scripts/launcher_fsr4_broker.py')
    host = load('fixture_fsr4_game_broker', PORT / 'scripts/launcher_game_broker.py')
finally:
    sys.path.pop(0)

@unittest.skipUnless(sys.platform == 'linux', 'Linux process lifecycle fixture')
class Fsr4Launcher(unittest.TestCase):
    def test_proton_discovery_reads_only_installed_bounded_steam_locations(self):
        with tempfile.TemporaryDirectory() as folder:
            home = Path(folder)
            with self.assertRaisesRegex(ValueError, 'complete Proton'):
                worker.find_proton(home)
            root = home / '.local/share/Steam/steamapps/common/Proton Experimental'
            (root / 'files/share/default_pfx').mkdir(parents=True)
            (root / 'files/bin').mkdir(); (root / 'files/bin/wineserver').write_text('authored fixture')
            (root / 'proton').write_text('authored fixture; never executed')
            self.assertEqual(worker.find_proton(home), root / 'proton')
            (root / 'files/share/default_pfx').rmdir()
            with self.assertRaisesRegex(ValueError, 'complete Proton'):
                worker.find_proton(home)

    def test_proton_discovery_without_home_uses_same_account_lookup(self):
        with tempfile.TemporaryDirectory() as folder:
            home=Path(folder)
            root=home/'.local/share/Steam/steamapps/common/Proton Experimental'
            (root/'files/share/default_pfx').mkdir(parents=True)
            (root/'files/bin').mkdir();(root/'files/bin/wineserver').write_text('authored fixture; never executed')
            (root/'proton').write_text('authored fixture; never executed')
            with mock.patch.dict(os.environ,{},clear=True),mock.patch('pwd.getpwuid',return_value=mock.Mock(pw_dir=str(home))) as lookup:
                self.assertEqual(worker.find_proton(),root/'proton')
                self.assertNotIn('HOME',os.environ);lookup.assert_called_once_with(os.getuid())

    def test_progress_and_cancel_keep_unrelated_process_alive(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); (root / 'scripts').mkdir(); (root / 'state/logs').mkdir(parents=True)
            script = root / 'scripts/launcher_fsr4_worker.py'
            script.write_text('import json,signal,time,sys\n'
                'signal.signal(signal.SIGTERM,lambda *_:sys.exit(130))\n'
                'print(\'MONADO_FSR4_PROGRESS \'+json.dumps(dict(stage="capture",current=2,total=4,message="Authored capture")),flush=True)\n'
                'time.sleep(30)\n')
            frames=[]
            unrelated = subprocess.Popen([sys.executable, '-c', 'import time;time.sleep(30)'])
            try:
                item = prep.Preparation(root=root, state=root/'state', environment={'PATH':'/usr/bin:/bin'},
                    descriptors=(), send=lambda *x:frames.append(x), request=9,
                    process_tree=host.process_tree, process_identity=host.process_identity)
                deadline=time.monotonic()+5
                while time.monotonic()<deadline and not any('2/4' in str(f) for f in frames):
                    self.assertFalse(item.poll());time.sleep(.01)
                self.assertTrue(any('2/4' in str(f) for f in frames))
                item.cancel()
                while time.monotonic()<deadline:
                    if item.poll():break
                    time.sleep(.01)
                else:self.fail('owned preparation did not stop')
                self.assertEqual(frames[-1][2], 'cancelled')
                self.assertIsNone(unrelated.poll())
                self.assertIn(b'Authored capture', (root/'state/logs/launcher-fsr4.log').read_bytes())
            finally:
                unrelated.terminate();unrelated.wait(timeout=5)

    def test_tampered_or_linked_worker_is_rejected_before_spawn(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);(root/'scripts').mkdir();(root/'state/logs').mkdir(parents=True)
            target=root/'other.py';target.write_text('raise AssertionError("must not run")')
            (root/'scripts/launcher_fsr4_worker.py').symlink_to(target)
            with self.assertRaisesRegex(ValueError, 'ordinary'):
                prep.Preparation(root=root,state=root/'state',environment={},descriptors=(),send=lambda *x:None,request=1,
                    process_tree=host.process_tree,process_identity=host.process_identity)

if __name__=='__main__':unittest.main()
