"""LINUX-099: actual optional counter client, authored module/errors, no GPU/game."""
from pathlib import Path
import os,shutil,subprocess,sys,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
@unittest.skipUnless(sys.platform=='linux','Linux optional observer client')
class ObserverClient(unittest.TestCase):
 @classmethod
 def setUpClass(cls):
  cc=os.environ.get('MONADO_TEST_CC') or shutil.which('clang')
  if not cc:raise unittest.SkipTest('Clang required')
  cls.tmp=tempfile.TemporaryDirectory(prefix='monado-observer-client-');cls.addClassCleanup(cls.tmp.cleanup);cls.root=Path(cls.tmp.name)
  flags=['-std=c11','-O1','-UNDEBUG','-ffp-contract=off','-fno-strict-aliasing']
  if os.environ.get('MONADO_TEST_UBSAN')=='1':flags+=['-fsanitize=undefined','-fno-sanitize-recover=all']
  cls.binary=cls.root/'client';cls.module=cls.root/'counter.so';cls.missing=cls.root/'missing.so';cls.invalid=cls.root/'invalid.so';cls.invalid.write_text('authored non-ELF fixture\n')
  for argv in ([cc,*flags,str(ROOT/'tests/native/test_present_observer_client.c'),'-ldl','-lm','-o',str(cls.binary)],[cc,*flags,'-shared','-fPIC',str(ROOT/'tests/native/present_observer_module_fixture.c'),'-o',str(cls.module)],[cc,*flags,'-DMISSING_SYMBOL','-shared','-fPIC',str(ROOT/'tests/native/present_observer_module_fixture.c'),'-o',str(cls.missing)]):
   result=subprocess.run(argv,capture_output=True,text=True,timeout=60);assert result.returncode==0,result.stdout+result.stderr
 def run_case(self,name,module=None):
  env={k:v for k,v in os.environ.items() if not k.startswith(('MONADO_PRESENT_','NVPRESENT_','LD_'))};r=subprocess.run([str(self.binary),name,str(module or self.module)],cwd=self.root,env=env,capture_output=True,text=True,timeout=10);self.assertEqual(r.returncode,0,r.stdout+r.stderr)
 def test_off_and_uncontrolled_do_not_load_or_probe_any_resource(self):
  self.run_case('off');self.run_case('uncontrolled')
 def test_optional_missing_or_invalid_module_preserves_unknown_counter(self):
  self.run_case('missing-resource');self.run_case('invalid-module',self.invalid);self.run_case('missing-symbol',self.missing)
 def test_actual_abi_values_warmup_faults_and_disable_never_invent_output(self):self.run_case('actual')
