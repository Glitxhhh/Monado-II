"""LINUX-101: bounded real output telemetry and actual driver-clock wrapper."""
from pathlib import Path
import os,shutil,subprocess,sys,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
@unittest.skipUnless(sys.platform=='linux','Linux monotonic output diagnostics')
class OutputDiagnostics(unittest.TestCase):
 def test_actual_format_throttle_invalid_values_and_monotonic_wrapper(self):
  private=ROOT/'Monado_Linux/.deps/bin/clang-cc'
  cc=os.environ.get('MONADO_TEST_CC') or (str(private) if private.is_file() else shutil.which('clang'))
  if not cc:raise unittest.SkipTest('Clang required')
  with tempfile.TemporaryDirectory(prefix='monado-output-diagnostics-') as tmp:
   folder=Path(tmp);device=(ROOT/'Monado_Linux/runtime/nvn/vulkan/device.c').read_text()
   start=device.index('bool vk_external_fps_snapshot(');end=device.index('\n}\n',start)+3
   fragment=folder/'snapshot.inc';fragment.write_text(device[start:end])
   source=folder/'test.c';body=(ROOT/'tests/native/test_fps_output_diagnostics.c').read_text()
   body=body.replace('../../Monado_Linux/runtime/ui/fps_output_diagnostics_linux.h',str(ROOT/'Monado_Linux/runtime/ui/fps_output_diagnostics_linux.h')).replace('SNAPSHOT_SOURCE',str(fragment));source.write_text(body)
   argv=[cc,'-std=c11','-O1','-UNDEBUG','-Wall','-Wextra','-Werror','-ffp-contract=off','-fno-strict-aliasing',str(source),'-lm','-o',str(folder/'test')]
   if os.environ.get('MONADO_TEST_UBSAN')=='1':argv[1:1]=['-fsanitize=undefined','-fno-sanitize-recover=all']
   result=subprocess.run(argv,capture_output=True,text=True,timeout=60);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
   result=subprocess.run([str(folder/'test')],capture_output=True,text=True,timeout=10);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
   self.assertIn('native FPS output diagnostics: PASS',result.stdout)
if __name__=='__main__':unittest.main()
