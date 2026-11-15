"""Attachable report archive boundaries; authored fixtures only."""
from pathlib import Path
import json
import os
import shutil
import subprocess
import tempfile
import unittest
import sys
import zipfile

ROOT=Path(__file__).resolve().parents[1]
@unittest.skipUnless(sys.platform == 'linux', 'Linux platform fixture')
class SupportReport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t=tempfile.TemporaryDirectory(prefix='monado-report-tests-');cls.addClassCleanup(cls.t.cleanup)
        cls.exe=Path(cls.t.name)/'report';local=ROOT/'Monado_Linux/.deps/bin/clang-cxx'
        cc=os.environ.get('MONADO_TEST_CXX') or (str(local) if local.is_file() else shutil.which('clang++') or shutil.which('g++'))
        if not cc:raise unittest.SkipTest('C++ compiler needed')
        command=[cc,'-std=c++17','-O1','-UNDEBUG',str(ROOT/'tests/native/test_support_report.cpp'),'-o',str(cls.exe)]
        if os.environ.get('MONADO_TEST_UBSAN')=='1':command[1:1]=['-fsanitize=undefined','-fno-sanitize-recover=all']
        p=subprocess.run(command,capture_output=True,text=True,timeout=60)
        if p.returncode:raise AssertionError(p.stdout+p.stderr)
    def invoke(self,state,ok=True):
        p=subprocess.run([str(self.exe),str(state)],capture_output=True,text=True,timeout=15)
        self.assertEqual(p.returncode==0,ok,p.stdout+p.stderr)
        return Path(p.stdout.strip()) if ok else p.stderr
    def test_archive_is_valid_and_correlates_known_runs(self):
        with tempfile.TemporaryDirectory(dir=self.t.name) as td:
            s=Path(td);(s/'logs/game-runs').mkdir(parents=True);(s/'crash').mkdir()
            (s/'xcde.ini').write_text('[General]\nfps=120\n');(s/'logs/monado.log').write_text('[run-settings] abc\n')
            (s/'logs/game-runs/run-abc.json').write_text('{"run_id":"abc"}')
            (s/'crash/crash_1.txt').write_text('authored crash fixture');(s/'crash/crash_1.dmp').write_bytes(b'dump')
            (s/'save_0').write_bytes(b'private save');(s/'xcde_env.ini').write_text('private arbitrary environment')
            a=self.invoke(s);self.assertEqual(a.stat().st_mode&0o777,0o600)
            with zipfile.ZipFile(a) as z:
                self.assertIsNone(z.testzip());self.assertEqual(z.read('logs/monado.log'),b'[run-settings] abc\n')
                self.assertIn('logs/game-runs/run-abc.json',z.namelist());self.assertIn('crash/crash_1.txt',z.namelist())
                self.assertNotIn('save_0',z.namelist());self.assertNotIn('xcde_env.ini',z.namelist());self.assertNotIn('crash/crash_1.dmp',z.namelist())
                manifest=json.loads(z.read('report-manifest.json'));self.assertFalse(manifest['uploaded'])
            b=self.invoke(s);self.assertNotEqual(a,b);self.assertTrue(a.exists())
    def test_bounds_symlinks_fifo_and_binary_exclusion(self):
        with tempfile.TemporaryDirectory(dir=self.t.name) as td:
            s=Path(td);(s/'logs').mkdir();(s/'outside').write_text('do not include')
            (s/'logs/launcher.log').symlink_to(s/'outside');os.mkfifo(s/'logs/launcher-console.log')
            (s/'logs/monado.log').write_text('A'*(2*1024*1024)+'tail')
            (s/'logs/monado.prev.log').write_bytes(b'binary\0content')
            a=self.invoke(s)
            with zipfile.ZipFile(a) as z:
                self.assertNotIn('logs/launcher.log',z.namelist());self.assertNotIn('logs/launcher-console.log',z.namelist())
                self.assertNotIn('logs/monado.prev.log',z.namelist())
                data=z.read('logs/monado.log');self.assertTrue(data.endswith(b'tail'));self.assertLess(len(data),1050000)
    def test_report_directory_symlink_rejected(self):
        with tempfile.TemporaryDirectory(dir=self.t.name) as td:
            s=Path(td);(s/'xcde.ini').write_text('fps=120');(s/'target').mkdir();(s/'reports').symlink_to(s/'target')
            self.invoke(s,False);self.assertEqual(list((s/'target').iterdir()),[])
    def test_empty_state_fails_without_creating_report(self):
        with tempfile.TemporaryDirectory(dir=self.t.name) as td:
            s=Path(td);self.invoke(s,False);self.assertFalse((s/'reports').exists())
