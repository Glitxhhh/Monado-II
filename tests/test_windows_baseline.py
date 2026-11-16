"""Windows source identity compatibility; no compiler, UI, input or profile."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

ROOT=Path(__file__).resolve().parents[1]
PORT=ROOT/'Monado_Windows'
spec=importlib.util.spec_from_file_location('authored_windows_baseline_driver',PORT/'scripts/build_windows.py')
driver=importlib.util.module_from_spec(spec);spec.loader.exec_module(driver)
LEGACY=dict(schema=1,accepted_baseline='dev104 R5',implementation='Windows development',public_release=False)
EXPERIMENTAL=dict(schema=1,accepted_baseline='dev104 R5',implementation='Windows Experimental',
                  version='0.1.0',release_channel='experimental',public_release=False)


class WindowsSourceBaseline(unittest.TestCase):
    def test_current_baseline_has_an_independent_experimental_version_without_publication_approval(self):
        value,identity=driver.read_windows_baseline(PORT/'windows-baseline.json')
        self.assertEqual(value,EXPERIMENTAL)
        self.assertEqual(identity,hashlib.sha256((PORT/'windows-baseline.json').read_bytes()).hexdigest())
        self.assertEqual(driver.windows_baseline_label(value),'dev104 R5 · Windows Experimental 0.1.0')
        self.assertEqual(driver.windows_baseline_label(LEGACY),'dev104 R5 · Windows development')

    def test_exact_legacy_and_new_forms_only_reject_bad_versions_channels_and_approval(self):
        self.assertEqual(driver.validate_windows_baseline(LEGACY),LEGACY)
        future=dict(EXPERIMENTAL,version='10.2.30')
        self.assertEqual(driver.validate_windows_baseline(future),future)
        rejected=[None,[],dict(LEGACY,version='0.1.0'),dict(EXPERIMENTAL,unknown=True),
            dict(LEGACY,schema=True),dict(EXPERIMENTAL,schema=1.0),dict(EXPERIMENTAL,accepted_baseline='dev104 R4'),
            dict(EXPERIMENTAL,release_channel='stable'),dict(EXPERIMENTAL,public_release=True),
            dict(LEGACY,public_release=0),dict(EXPERIMENTAL,implementation='native-windows')]
        rejected.extend(dict(EXPERIMENTAL,version=value) for value in
                        ('v0.1.0','0.1.0-experimental','01.1.0','0.1','../0.1.0',True,0.1,'9'*65+'.0.0'))
        for missing in EXPERIMENTAL:
            value=dict(EXPERIMENTAL);value.pop(missing);rejected.append(value)
        for value in rejected:
            with self.subTest(value=value),self.assertRaises(RuntimeError):
                driver.validate_windows_baseline(value)

    def test_actual_builder_receipt_and_settings_compile_label_preserve_internal_implementation(self):
        for index,baseline in enumerate((LEGACY,EXPERIMENTAL)):
            with self.subTest(baseline=baseline),tempfile.TemporaryDirectory(prefix='authored-baseline-') as directory:
                root=Path(directory).resolve()/'Monado_Windows';root.mkdir()
                raw=(json.dumps(baseline)+'\n').encode('utf-8')
                (root/'windows-baseline.json').write_bytes(raw)
                prefix=root/'dependency-prefix';prefix.mkdir()
                source=root/'tools/launcher/launcher.cpp';source.parent.mkdir(parents=True)
                source.write_text('const char *authored_label=MONADO_WINDOWS_BASELINE_LABEL;\n',encoding='utf-8')
                args=argparse.Namespace(output=root/'build'/('attempt-'+str(index)),prefix=prefix,
                    stage='launcher',jobs=1,keep_going=False,split_game=False)
                with (mock.patch.object(driver,'ROOT',root),mock.patch.object(driver,'OwnedProcesses') as owned,
                     mock.patch.object(driver.importlib.metadata,'version',return_value='0.14.1'),
                     mock.patch.object(driver,'git_identity',return_value={'git_commit':None,'git_branch':None})):
                    builder=driver.Builder(args)
                    self.assertEqual(builder.receipt['implementation'],'windows-parity-development')
                    self.assertEqual(builder.receipt['baseline'],baseline)
                    self.assertEqual(builder.receipt['sources']['windows-baseline.json'],hashlib.sha256(raw).hexdigest())
                    with mock.patch.object(builder,'run') as run:
                        builder.compile([source])
                    command=run.call_args.args[1]
                    label=[item for item in command if item.startswith('-DMONADO_WINDOWS_BASELINE_LABEL=')]
                    self.assertEqual(len(label),1)
                    self.assertEqual(json.loads(label[0].split('=',1)[1]),driver.windows_baseline_label(baseline))
                    self.assertFalse(builder.receipt['baseline']['public_release'])
                    owned.assert_called_once_with()
                    builder.guard.close()

    def test_malformed_or_duplicate_json_fails_before_build_directory_or_job_creation(self):
        with tempfile.TemporaryDirectory(prefix='authored-baseline-negative-') as directory:
            root=Path(directory).resolve()/'Monado_Windows';root.mkdir()
            args=argparse.Namespace(output=root/'build/never-created',prefix=root/'prefix')
            invalid=[b'{',b'[]',b'{}',b'x'*4097,
                (json.dumps(EXPERIMENTAL)[:-1]+',"public_release":true,"public_release":false}').encode(),
                json.dumps(dict(EXPERIMENTAL,public_release=True)).encode()]
            with mock.patch.object(driver,'ROOT',root),mock.patch.object(driver,'OwnedProcesses') as owned:
                for raw in invalid:
                    with self.subTest(raw=raw[:100]):
                        (root/'windows-baseline.json').write_bytes(raw)
                        with self.assertRaises(RuntimeError):driver.Builder(args)
                        self.assertFalse(args.output.exists())
                owned.assert_not_called()


if __name__=='__main__':
    unittest.main()
