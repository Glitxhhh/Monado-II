"""Owned portable-build session teardown, including TERM-resistant children."""
import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'Monado_Linux/scripts'))
try:
    spec=importlib.util.spec_from_file_location('portable_build_process',ROOT/'Monado_Linux/scripts/build_portable_dependencies.py')
    recipe=importlib.util.module_from_spec(spec);spec.loader.exec_module(recipe)
finally:sys.path.pop(0)

@unittest.skipUnless(sys.platform=='linux','Linux process-group fixture')
class PortableBuildProcess(unittest.TestCase):
    def test_term_parent_exit_still_finishes_its_term_resistant_child(self):
        with tempfile.TemporaryDirectory() as directory:
            marker=Path(directory)/'ready'
            child_code='import os,signal,sys,time;signal.signal(signal.SIGTERM,signal.SIG_IGN);open(sys.argv[1],"w").write(str(os.getpid()));time.sleep(30)'
            parent_code='import subprocess,sys,time;subprocess.Popen([sys.executable,"-c",sys.argv[2],sys.argv[1]]);time.sleep(30)'
            parent=subprocess.Popen([sys.executable,'-c',parent_code,str(marker),child_code],start_new_session=True)
            unrelated=subprocess.Popen([sys.executable,'-c','import time;time.sleep(30)'])
            try:
                deadline=time.monotonic()+5
                while not marker.exists() and time.monotonic()<deadline:time.sleep(.01)
                self.assertTrue(marker.exists());pid=int(marker.read_text())
                recipe.terminate_owned_group(parent,grace=.2)
                self.assertIsNotNone(parent.poll());self.assertIsNone(unrelated.poll())
                while time.monotonic()<deadline:
                    try:state=(Path('/proc')/str(pid)/'stat').read_text().rsplit(')',1)[1].split()[0]
                    except FileNotFoundError:break
                    if state=='Z':break
                    time.sleep(.01)
                else:self.fail('TERM-resistant compiler descendant remained alive')
            finally:
                if parent.poll() is None:os.killpg(parent.pid,signal.SIGKILL);parent.wait()
                unrelated.terminate();unrelated.wait(timeout=5)

if __name__=='__main__':unittest.main()
