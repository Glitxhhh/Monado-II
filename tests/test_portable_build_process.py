"""Owned portable-build session teardown, including TERM-resistant children."""
import importlib.util
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import textwrap
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


@unittest.skipUnless(sys.platform == 'linux', 'Linux process-local descriptor limits')
class PortableFileLimits(unittest.TestCase):
    def run_case(self, body, *, minimum_hard=4096):
        import resource
        original = resource.getrlimit(resource.RLIMIT_NOFILE)
        if original[1] != resource.RLIM_INFINITY and original[1] < minimum_hard:
            self.skipTest('fixture needs a hard descriptor limit of ' + str(minimum_hard))
        # Resource-limit changes occur in a fresh fixture process, never in the
        # test runner or desktop session. No compiler/game/dependency is needed.
        setup = """
import importlib.util, json, os, resource, subprocess, sys
from pathlib import Path
from unittest import mock
path = Path(sys.argv[1])
sys.path.insert(0, str(path.parent))
spec = importlib.util.spec_from_file_location('portable_limits_child', path)
recipe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recipe)
hard = resource.getrlimit(resource.RLIMIT_NOFILE)[1]
"""
        result = subprocess.run([sys.executable, '-B', '-c', setup + textwrap.dedent(body),
                                 str(ROOT / 'Monado_Linux/scripts/build_portable_dependencies.py')],
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(resource.getrlimit(resource.RLIMIT_NOFILE), original)
        return json.loads(result.stdout)

    def test_low_soft_limit_reaches_child_and_restores_without_hard_change(self):
        row = self.run_case("""
resource.setrlimit(resource.RLIMIT_NOFILE, (1024, hard))
receipt = {}
with recipe.portable_file_limit(receipt):
    command = 'import json,os,resource; handles=[open(os.devnull,"rb") for _ in range(2048)]; print(json.dumps(resource.getrlimit(resource.RLIMIT_NOFILE)))'
    child = subprocess.run([sys.executable, '-c', command], capture_output=True, text=True, check=True)
    seen = json.loads(child.stdout)
    assert seen[0] >= 4096 and seen[1] == hard
assert resource.getrlimit(resource.RLIMIT_NOFILE) == (1024, hard)
print(json.dumps(receipt))
""")
        limits = row['file_descriptor_limits']
        self.assertEqual(limits['initial']['soft'], 1024)
        self.assertEqual(limits['initial'], limits['restored'])
        self.assertEqual(limits['effective']['hard'], limits['initial']['hard'])
        self.assertGreaterEqual(limits['effective']['soft'], 4096)

    def test_failure_and_cancellation_restore_and_keep_original_exception(self):
        row = self.run_case("""
resource.setrlimit(resource.RLIMIT_NOFILE, (1024, hard))
receipts = []
for failure in (RuntimeError('authored linker failure'), KeyboardInterrupt('cancelled')):
    receipt = {}
    try:
        with recipe.portable_file_limit(receipt):
            subprocess.run([sys.executable, '-c', 'raise SystemExit(7)'], check=False)
            raise failure
    except BaseException as caught:
        assert caught is failure
    else:
        raise AssertionError('original failure was lost')
    assert resource.getrlimit(resource.RLIMIT_NOFILE) == (1024, hard)
    receipts.append(receipt)
print(json.dumps(receipts))
""")
        for receipt in row:
            limits = receipt['file_descriptor_limits']
            self.assertEqual(limits['restored'], limits['initial'])

    def test_already_high_soft_limit_is_retained(self):
        row = self.run_case("""
resource.setrlimit(resource.RLIMIT_NOFILE, (65536, hard))
receipt = {}
with recipe.portable_file_limit(receipt):
    assert resource.getrlimit(resource.RLIMIT_NOFILE) == (65536, hard)
assert resource.getrlimit(resource.RLIMIT_NOFILE) == (65536, hard)
print(json.dumps(receipt))
""", minimum_hard=65536)
        limits = row['file_descriptor_limits']
        self.assertEqual(limits['initial'], limits['effective'])
        self.assertEqual(limits['initial'], limits['restored'])

    def test_lower_sufficient_hard_cap_is_not_changed(self):
        row = self.run_case("""
resource.setrlimit(resource.RLIMIT_NOFILE, (1024, 4096))
receipt = {}
with recipe.portable_file_limit(receipt):
    assert resource.getrlimit(resource.RLIMIT_NOFILE) == (4096, 4096)
assert resource.getrlimit(resource.RLIMIT_NOFILE) == (1024, 4096)
print(json.dumps(receipt))
""")
        limits = row['file_descriptor_limits']
        self.assertEqual(limits['effective'], {'soft': 4096, 'hard': 4096})
        self.assertEqual(limits['restored'], {'soft': 1024, 'hard': 4096})

    def test_hard_1024_stops_before_entering_build_with_clear_diagnostic(self):
        row = self.run_case("""
resource.setrlimit(resource.RLIMIT_NOFILE, (1024, 1024))
receipt = {}
entered = False
try:
    with recipe.portable_file_limit(receipt):
        entered = True
except RuntimeError as error:
    diagnostic = str(error)
else:
    raise AssertionError('insufficient hard limit accepted')
assert not entered
assert 'soft=1024, hard=1024' in diagnostic and '4096' in diagnostic and 'sudo' in diagnostic
assert resource.getrlimit(resource.RLIMIT_NOFILE) == (1024, 1024)
print(json.dumps({'receipt': receipt, 'diagnostic': diagnostic}))
""", minimum_hard=1024)
        limits = row['receipt']['file_descriptor_limits']
        self.assertEqual(limits['initial'], limits['effective'])
        self.assertEqual(limits['initial'], limits['restored'])
        self.assertIn('hard limit is too low', row['diagnostic'])

    def test_restoration_error_keeps_the_original_linker_error(self):
        row = self.run_case("""
resource.setrlimit(resource.RLIMIT_NOFILE, (1024, hard))
real_set = resource.setrlimit
calls = []
def fail_restore(kind, value):
    calls.append(value)
    if len(calls) == 2:
        raise OSError('authored restoration failure')
    return real_set(kind, value)
receipt = {}
failure = RuntimeError('authored linker failure')
try:
    with mock.patch.object(resource, 'setrlimit', side_effect=fail_restore):
        try:
            with recipe.portable_file_limit(receipt):
                raise failure
        except BaseException as caught:
            assert caught is failure
            assert any('restoration also failed' in note for note in caught.__notes__)
        else:
            raise AssertionError('original linker failure was lost')
    assert resource.getrlimit(resource.RLIMIT_NOFILE)[1] == hard
finally:
    real_set(resource.RLIMIT_NOFILE, (1024, hard))
print(json.dumps(receipt))
""")
        self.assertEqual(row['file_descriptor_limits']['restoration_error'], 'authored restoration failure')

if __name__=='__main__':unittest.main()
