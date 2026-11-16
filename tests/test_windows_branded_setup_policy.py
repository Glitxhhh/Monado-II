"""Headless native fixture of the production branded setup routing policy."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
import uuid

ROOT=Path(__file__).absolute().parents[1]
PORT=ROOT/'Monado_Windows'


@unittest.skipUnless(os.name=='nt','actual Windows native policy fixture')
class BrandedSetupPolicy(unittest.TestCase):
    def test_production_policy_handles_first_run_reopen_cancel_and_checked_handoff(self):
        run=PORT/'runs/temporal-delivery-r2'/('branded-setup-policy-'+uuid.uuid4().hex)
        run.mkdir()
        executable=run/'authored-policy.exe'
        compiler=os.environ.get('MONADO_NATIVE_TEST_COMPILER')
        command=([compiler,'c++'] if compiler else [sys.executable,'-m','ziglang','c++'])+[
            '-std=c++17','-O1','-UNDEBUG','-march=x86_64_v2','-ffp-contract=off','-fno-fast-math',
            '-I',str(PORT/'tools/builder'),str(PORT/'tests/native/test_branded_setup_policy_windows.cpp'),'-o',str(executable)]
        environment=dict(os.environ,ZIG_GLOBAL_CACHE_DIR=str(run/'zig-global'),ZIG_LOCAL_CACHE_DIR=str(run/'zig-local'))
        compiled=subprocess.run(command,cwd=run,env=environment,capture_output=True,timeout=180)
        (run/'compile.out').write_bytes(compiled.stdout);(run/'compile.err').write_bytes(compiled.stderr)
        self.assertEqual(compiled.returncode,0,compiled.stderr.decode('utf-8',errors='replace'))
        completed=subprocess.run([str(executable)],cwd=run,env=environment,capture_output=True,timeout=20)
        (run/'policy.out').write_bytes(completed.stdout);(run/'policy.err').write_bytes(completed.stderr)
        self.assertEqual(completed.returncode,0,completed.stderr.decode('utf-8',errors='replace'))
        self.assertIn(b'PASS production setup policy:',completed.stdout)
        self.assertIn(b'no window/game/profile I/O',completed.stdout)
        # The fixture compiles the same production header. These integration
        # anchors ensure the actual bridge/view call it, rather than leaving a
        # passing helper unused beside the buggy routing/state implementation.
        bridge=(PORT/'tools/builder/windows_builder.cpp').read_text('utf-8')
        view=(PORT/'tools/builder/branded_windows.h').read_text('utf-8')
        for anchor in ('#include "setup_policy_windows.h"','monado_setup_policy::destination_for(',
                       'monado_setup_policy::apply_result(','using monado_setup_policy::finish_checked_settings;',
                       'monado_setup_policy::picker_owner(interaction_window,window)'):
            self.assertIn(anchor,bridge)
        self.assertIn('monado_setup_policy::path_controls_enabled(busy.load())',view)
        self.assertNotIn('current_install=e->installation',bridge)
        receipt={'schema':1,'status':'passed','kind':'headless-native-production-branded-setup-policy',
            'command':command,'source_sha256':hashlib.sha256((PORT/'tools/builder/setup_policy_windows.h').read_bytes()).hexdigest(),
            'output':completed.stdout.decode('utf-8'),'window_game_profile_opened':False}
        (run/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n',encoding='utf-8')
        print('Headless branded setup policy receipt: '+str(run/'receipt.json'),flush=True)


if __name__=='__main__':unittest.main()
