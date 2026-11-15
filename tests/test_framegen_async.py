"""LINUX-099 actual bounded presenter ownership and timing, no SDL/GPU/game."""
import os
import sys
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
@unittest.skipUnless(sys.platform == "linux", "Linux pthread presentation worker")
class AsyncPresenter(unittest.TestCase):
    def setUp(self):
        import resource
        resource.setrlimit(resource.RLIMIT_CORE,(0,0))
        self.cc=os.environ.get('MONADO_TEST_CC') or str(ROOT/'Monado_Linux/.deps/bin/clang-cc')
        if not Path(self.cc).is_file():self.cc=shutil.which('clang')
        if not self.cc:self.skipTest('Clang required')
        sysroot=ROOT/'Monado_Linux/.deps/graphics/active-sysroot.txt'
        self.include=os.environ.get('MONADO_TEST_SDL_INCLUDE') or (str(Path(sysroot.read_text().strip())/'usr/include') if sysroot.is_file() else None)
        self.temp=tempfile.TemporaryDirectory(prefix='monado-fg-worker-');self.addCleanup(self.temp.cleanup);self.directory=Path(self.temp.name)
    def compile(self,header=None):
        binary=self.directory/'worker';flags=['-O1','-UNDEBUG','-pthread','-lm','-I'+str(ROOT/'Monado_Linux/runtime/nvn/vulkan')]
        if self.include:flags+=['-I'+self.include]
        if os.environ.get('MONADO_TEST_UBSAN'):flags+=['-fsanitize=undefined','-fno-sanitize-recover=all']
        if header:
            path=self.directory/'authored-negative.h';path.write_text(header);flags+=['-DFRAMEGEN_ASYNC_HEADER="'+str(path)+'"']
        result=subprocess.run([self.cc,*flags,str(ROOT/'Monado_Linux/tests/native/test_framegen_async_linux.c'),'-o',str(binary)],capture_output=True,text=True,timeout=60)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr);return binary
    def test_actual_worker_overlap_ownership_and_recovery(self):
        result=subprocess.run([str(self.compile())],capture_output=True,text=True,timeout=15)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('producer overlap',result.stdout);self.assertIn('SDL-clock separation',result.stdout)
    def test_rejects_unbounded_publication(self):
        source=(ROOT/'Monado_Linux/runtime/nvn/vulkan/framegen_present_linux.h').read_text()
        start=source.index('static bool fg_presenter_enqueue(');before=source[:start];tail=source[start:]
        old='while(p->busy)pthread_cond_wait(&p->complete,&p->lock);';self.assertIn(old,tail)
        binary=self.compile(before+tail.replace(old,'/* authored negative: lost bounded backpressure */',1))
        result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=5)
        self.assertNotEqual(result.returncode,0);self.assertIn('!atomic_load(&enqueued)',result.stderr)
    def test_rejects_pacing_clock_as_meter_clock(self):
        source=(ROOT/'Monado_Linux/runtime/nvn/vulkan/framegen_present_linux.h').read_text()
        old='r.meter_ns[i]=p->ops.meter_now();';self.assertIn(old,source)
        binary=self.compile(source.replace(old,'r.meter_ns[i]=r.call_ns[i];',1))
        result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=5)
        self.assertNotEqual(result.returncode,0);self.assertIn('previous.meter_ns[0]<previous.call_ns[0]',result.stderr)
    def test_rejects_synchronous_gpu_wait_in_publication(self):
        source=(ROOT/'Monado_Linux/runtime/nvn/vulkan/framegen_present_linux.h').read_text()
        start=source.index('static bool fg_presenter_enqueue(');before=source[:start];tail=source[start:]
        old='while(p->busy)pthread_cond_wait(&p->complete,&p->lock);'
        replacement='fprintf(stderr,"negative: synchronous GPU wait in publication\\n");fflush(stderr);p->ops.wait_ready(job);'+old
        binary=self.compile(before+tail.replace(old,replacement,1))
        with self.assertRaises(subprocess.TimeoutExpired) as caught:subprocess.run([str(binary)],capture_output=True,text=True,timeout=1)
        self.assertIn(b'negative: synchronous GPU wait in publication',caught.exception.stderr)
