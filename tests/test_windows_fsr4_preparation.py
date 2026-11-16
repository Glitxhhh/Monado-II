"""Authored native preparation boundaries; no SDK/models/game or GPU dispatch."""
from pathlib import Path
import contextlib, ctypes as C, hashlib, io, json, os, subprocess, sys, tarfile, tempfile, time, unittest
ROOT=Path(__file__).resolve().parents[1];PORT=ROOT/'Monado_Windows'
sys.path.insert(0,str(PORT/'tools'))
import setup_windows_fsr4_tools as tools
import prepare_windows_fsr4 as prepare
from fsr4_process_windows import run,Cancelled

class MemoryDirectory:
    def __init__(self,files):self.files=files
    def read_regular(self,name,**_):return self.files[name]
    def atomic_replace(self,name,data,expected_sha256):
        assert hashlib.sha256(self.files[name]).hexdigest()==expected_sha256;self.files[name]=data

class PreparationSources(unittest.TestCase):
    def test_exact_patch_has_no_fuzzy_or_repeated_application(self):
        patch=b'--- a/a.cpp\n+++ b/a.cpp\n@@ -1,2 +1,3 @@\n a\n-b\n+c\n+d\n'
        directory=MemoryDirectory({'a.cpp':b'a\nb\n'});tools.patch_class_bindings(directory,patch)
        self.assertEqual(directory.files['a.cpp'],b'a\nc\nd\n')
        with self.assertRaisesRegex(ValueError,'context'):tools.patch_class_bindings(directory,patch)
    def test_source_lock_records_all_tools_and_exact_accepted_dependency_pins(self):
        lock=json.loads((PORT/'fsr4-preparation-windows.lock.json').read_bytes());accepted=json.loads((ROOT/'Monado_Linux/fsr4-linux.lock.json').read_bytes())
        self.assertEqual(lock['dependencies'],accepted['dependencies'])
        for name,item in lock['files'].items():
            data=(PORT/name).read_bytes();self.assertEqual(item,{'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()},name)
        self.assertIn('third_party/fsr4cap/windows_capture_io.h',lock['files'])

@unittest.skipUnless(os.name=='nt','Native Windows fixtures')
class NativePreparation(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(prefix='fsr4 authored 測試 $() ` ',dir=PORT/'runs');self.root=Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)
    def test_actual_child_unicode_literal_arguments_failure_and_existing_log_preservation(self):
        arguments=['spaces here','測試', '$()','`literal`','trailing\\','a"b']
        log=self.root/'literal.log';result=run([sys.executable,'-c','import json,sys;print(json.dumps(sys.argv[1:],ensure_ascii=False));sys.exit(23)',*arguments],cwd=self.root,log=log,environment=dict(os.environ,PYTHONIOENCODING='utf-8'),timeout=10)
        self.assertEqual(result['exit'],23);self.assertEqual(json.loads(log.read_bytes()),arguments);before=log.read_bytes()
        with self.assertRaises(FileExistsError):run([sys.executable,'-c','raise AssertionError()'],cwd=self.root,log=log,environment=os.environ,timeout=10)
        self.assertEqual(log.read_bytes(),before)
    def test_long_capture_work_directory_refuses_before_child_or_log_and_preserves_selection(self):
        directory=self.root
        while len(str(directory).encode('utf-16-le'))//2<310:directory/='authored capture Unicode 測試'
        directory.mkdir(parents=True);selection=directory/'authored-selection.json';selection.write_bytes(b'preserve selected models')
        marker=directory/'MUST-NOT-EXIST';log=directory/'MUST-NOT-CREATE.log'
        with self.assertRaisesRegex(ValueError,'shorter than 260'):
            run([sys.executable,'-c','from pathlib import Path;import sys;Path(sys.argv[1]).write_bytes(b"wrong")',str(marker)],
                cwd=directory,log=log,environment=os.environ,timeout=10)
        self.assertFalse(log.exists());self.assertFalse(marker.exists());self.assertEqual(selection.read_bytes(),b'preserve selected models')
    def test_cancel_kills_only_owned_descendants_and_preserves_unrelated_process(self):
        from ctypes import wintypes as W
        kernel=C.WinDLL('kernel32',use_last_error=True);kernel.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD];kernel.OpenProcess.restype=W.HANDLE
        kernel.WaitForSingleObject.argtypes=[W.HANDLE,W.DWORD];kernel.WaitForSingleObject.restype=W.DWORD;kernel.CloseHandle.argtypes=[W.HANDLE]
        pid=self.root/'grandchild.pid';held=[]
        child="import subprocess,sys,time,pathlib;p=subprocess.Popen([sys.executable,'-c','import time;time.sleep(30)']);pathlib.Path(sys.argv[1]).write_text(str(p.pid));time.sleep(30)"
        unrelated=subprocess.Popen([sys.executable,'-c','import time;time.sleep(30)'])
        def checkpoint():
            if pid.exists():
                handle=kernel.OpenProcess(0x100000|0x1000,False,int(pid.read_text()));self.assertTrue(handle);held.append(handle);raise Cancelled('authored cancel')
        try:
            with self.assertRaises(Cancelled):run([sys.executable,'-c',child,str(pid)],cwd=self.root,log=self.root/'cancel.log',environment=os.environ,timeout=10,checkpoint=checkpoint)
            self.assertTrue(held);self.assertEqual(kernel.WaitForSingleObject(held[0],5000),0);self.assertIsNone(unrelated.poll())
        finally:
            for handle in held:kernel.CloseHandle(handle)
            unrelated.terminate();unrelated.wait(timeout=5)
    def test_timeout_is_failure_without_promotion(self):
        with self.assertRaises(TimeoutError):run([sys.executable,'-c','import time;time.sleep(30)'],cwd=self.root,log=self.root/'timeout.log',environment=os.environ,timeout=.1)
        self.assertFalse((self.root/'models.id').exists())
    def test_live_bounded_progress_callback_and_log_overflow_failure(self):
        messages=[]
        result=run([sys.executable,'-u','-c','import time;print("MONADO_FSR4_PROGRESS ready");time.sleep(.3);print("done")'],cwd=self.root,log=self.root/'progress.log',environment=os.environ,timeout=5,output=messages.append)
        self.assertEqual(result['exit'],0);self.assertEqual(messages,['MONADO_FSR4_PROGRESS ready','done'])
        with self.assertRaisesRegex(RuntimeError,'log size'):
            run([sys.executable,'-u','-c','print("x"*4096)'],cwd=self.root,log=self.root/'overflow.log',environment=os.environ,timeout=5,max_log_bytes=1024)
    def test_bounded_safe_source_unpack_and_reject_parent_or_case_collision(self):
        def archive(entries):
            output=io.BytesIO()
            with tarfile.open(fileobj=output,mode='w:gz') as tf:
                for name,data in entries:
                    info=tarfile.TarInfo(name);info.size=len(data);tf.addfile(info,io.BytesIO(data))
            return output.getvalue()
        with tools.guard.locked_directory(self.root,owner=True) as root:
            with root.child('good',create=True,owner=True) as out:
                tools.unpack(archive([('source/src/a.c',b'int a;\n')]),out);self.assertEqual(out.read_regular('src/a.c'),b'int a;\n')
            for index,entries in enumerate(([('source/../escape',b'x')],[('source/a',b'x'),('source/A',b'y')],[('source/a\\b',b'x')])):
                with root.child('bad'+str(index),create=True,owner=True) as out,self.assertRaises(ValueError):tools.unpack(archive(entries),out)
        self.assertFalse((self.root/'escape').exists())
    def test_missing_completion_and_malformed_capture_never_become_ready(self):
        with self.assertRaises(OSError):prepare.verify_prepared(self.root)
        with tools.guard.locked_directory(self.root,owner=True) as root:
            with root.child('capture',create=True,owner=True) as out:
                out.write_new('trace.txt',b'MARK frame 1\n');out.write_new('human-note',b'preserve')
                with self.assertRaisesRegex(ValueError,'Unknown capture'):
                    with prepare.checked_captures([out.path]):pass
                self.assertEqual(out.read_regular('human-note'),b'preserve')
    def test_real_extract_rejects_bad_dispatch_before_any_translator_execution(self):
        capture=self.root/'capture_1280x720_1920x1080';capture.mkdir()
        (capture/'trace.txt').write_bytes(b'MARK frame 1\nDISPATCH #0 cs=0000000000000000 root=0000000000000000 groups=1,1,1\nMARK end\n')
        (capture/'cs_0000000000000000.dxil').write_bytes(b'AuthoredBadSequence')
        output=self.root/'translated'
        result=run([sys.executable,'-I','-S','-B','-X','utf8',PORT/'third_party/fsr4cap/extract.py',self.root/'MUST-NOT-EXECUTE.exe',self.root,output],cwd=self.root,log=self.root/'bad-trace.log',environment=dict(os.environ,FSR4CAP_MODEL='lite'),timeout=10)
        self.assertEqual(result['exit'],6,(self.root/'bad-trace.log').read_bytes());self.assertIn(b'no assets written',(self.root/'bad-trace.log').read_bytes());self.assertFalse(output.exists())
    def test_actual_native_recorder_unicode_io_without_sdk_or_gpu(self):
        from native_renderer_support import require_pinned_zig
        compiler=require_pinned_zig()
        exe=self.root/'fsr4-capture-測試.exe'
        environment=dict(os.environ,ZIG_LOCAL_CACHE_DIR=str(self.root/'zig-local'),ZIG_GLOBAL_CACHE_DIR=str(PORT/'build/zig-global-cache'))
        built=subprocess.run([str(compiler),'cc','-std=c11','-O1','-UNDEBUG','-municode',str(PORT/'tests/native/test_fsr4_capture_windows.c'),'-o',str(exe)],capture_output=True,env=environment,timeout=120)
        self.assertEqual(built.returncode,0,built.stderr.decode(errors='replace'))
        result=run([exe,self.root],cwd=self.root,log=self.root/'native-capture.log',environment=environment,timeout=10)
        self.assertEqual(result['exit'],0,(self.root/'native-capture.log').read_bytes())

if __name__=='__main__':unittest.main()
