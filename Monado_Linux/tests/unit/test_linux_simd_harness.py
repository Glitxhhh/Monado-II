"""[XCDE] LINUX-002: binary SIMD harness rejects incomplete or failed validation."""
import importlib.util
import os
from pathlib import Path
import struct
import sys
import types
import unittest
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/lifter'))
spec = importlib.util.spec_from_file_location('monado_test_simd', ROOT / 'tools/lifter/test_simd.py')
simd = importlib.util.module_from_spec(spec)
try:
    spec.loader.exec_module(simd)
except ModuleNotFoundError as error:
    if error.name not in ('unicorn', 'capstone'):
        raise
    simd = None


@unittest.skipUnless(sys.platform == 'linux' and simd is not None and (ROOT / '.deps/bin/clang-cc').is_file(),
                     'native SIMD protocol tests require Linux, local GNU Clang wrapper and Unicorn tooling')
class LinuxSimdHarness(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = Path(simd.fresh_work(os.environ.get('XCDE_SIMD_TEST_OUTPUT')))
        cls.exe = simd.compile_tests(str(cls.work), [
            'c->x[0] += c->x[1]; c->v[0] = c->v[1];',
            'ST32(c->x[0], c->x[1]);',
            'rt_udf(c, c->pc);',
        ], 2, '')
        print('SIMD harness unit evidence:', cls.work)

    def case(self, name, records, *, code=b'\0' * 12, buffer=None, preserve_output=False):
        work = self.work / name
        work.mkdir(); (work / 'tmp').mkdir()
        (work / 'code.bin').write_bytes(code)
        (work / 'buf.bin').write_bytes(bytes(simd.BUF_SIZE) if buffer is None else buffer)
        (work / 'cases.bin').write_bytes(records)
        out = work / 'out.bin'
        if preserve_output:
            out.write_bytes(b'original evidence')
        return work, out, [self.exe, *[str(work / name) for name in ('code.bin', 'buf.bin', 'cases.bin', 'out.bin')]]

    def record(self, *, k=0, store=0, reserved=0, x0=3, x1=7):
        return (struct.pack(simd.IN_FMT, k, 0, x0, x1, *([0] * 30), simd.BASE_AREA, 0xa0000000)
                + (123).to_bytes(16, 'little') + (456).to_bytes(16, 'little') + bytes(30 * 16)
                + struct.pack('<QQ', store, reserved))

    def test_valid_records_and_store_reset_crc(self):
        records = self.record() + self.record(k=1, store=1, x0=simd.BUF + 4, x1=0x78563412)
        records += self.record(k=1, store=1, x0=simd.BUF + 12, x1=0xdeadbeef)
        work, out, argv = self.case('valid', records)
        simd.run_logged(work, 'run', argv)
        data = out.read_bytes(); simd.validate_output(data, 3)
        self.assertEqual(struct.unpack_from('<Q', data)[0], 10)
        self.assertEqual(int.from_bytes(data[272:288], 'little'), 456)
        for index, offset, value in ((1, 4, 0x78563412), (2, 12, 0xdeadbeef)):
            expected = bytearray(simd.BUF_SIZE); struct.pack_into('<I', expected, offset, value)
            self.assertEqual(struct.unpack_from('<Q', data, index * simd.OUT_SIZE + 264)[0], zlib.crc32(expected))

    def test_truncated_input_record_fails(self):
        work, out, argv = self.case('truncated', self.record()[:-1])
        with self.assertRaises(simd.HarnessError): simd.run_logged(work, 'run', argv)
        self.assertIn('truncated input record', (work / 'run.stderr.log').read_text())

    def test_empty_records_fail(self):
        work, out, argv = self.case('empty', b'')
        with self.assertRaises(simd.HarnessError): simd.run_logged(work, 'run', argv)
        self.assertIn('empty case file', (work / 'run.stderr.log').read_text())

    def test_invalid_record_fields_fail(self):
        for name, kwargs in (('index', {'k': 3}), ('store', {'store': 2}), ('reserved', {'reserved': 1})):
            with self.subTest(name=name):
                work, out, argv = self.case(name, self.record(**kwargs))
                with self.assertRaises(simd.HarnessError): simd.run_logged(work, 'run', argv)
                self.assertIn('invalid input record', (work / 'run.stderr.log').read_text())

    def test_wrong_sized_memory_input_fails(self):
        work, out, argv = self.case('short-buffer', self.record(), buffer=b'\0')
        with self.assertRaises(simd.HarnessError): simd.run_logged(work, 'run', argv)
        self.assertFalse(out.exists())

    def test_truncated_code_image_fails(self):
        work, out, argv = self.case('short-code', self.record(), code=b'\0')
        with self.assertRaises(simd.HarnessError): simd.run_logged(work, 'run', argv)
        self.assertFalse(out.exists())

    def test_missing_input_fails(self):
        work, out, argv = self.case('missing', self.record())
        (work / 'cases.bin').unlink()
        with self.assertRaises(simd.HarnessError): simd.run_logged(work, 'run', argv)
        self.assertFalse(out.exists())

    def test_existing_output_is_preserved(self):
        work, out, argv = self.case('existing', self.record(), preserve_output=True)
        with self.assertRaises(simd.HarnessError): simd.run_logged(work, 'run', argv)
        self.assertEqual(out.read_bytes(), b'original evidence')

    def test_aborting_harness_cannot_pass(self):
        work, out, argv = self.case('abort', self.record(k=2))
        with self.assertRaises(simd.HarnessError): simd.run_logged(work, 'run', argv)
        with self.assertRaises(simd.HarnessError): simd.validate_output(out.read_bytes(), 1)

    def test_truncated_surplus_and_empty_output_fail(self):
        for data, count in ((b'', 1), (bytes(simd.OUT_SIZE - 1), 1),
                            (bytes(simd.OUT_SIZE + 1), 1), (b'', 0)):
            with self.subTest(length=len(data), count=count), self.assertRaises(simd.HarnessError):
                simd.validate_output(data, count)

    def test_missing_compiler_cannot_pass(self):
        work = self.work / 'missing-compiler'; work.mkdir(); (work / 'tmp').mkdir()
        with self.assertRaises(simd.HarnessError): simd.run_logged(work, 'compile', ['/no/such/monado-compiler'])

    def test_failed_child_with_complete_sized_output_cannot_pass(self):
        work = self.work / 'failed-with-output'; work.mkdir(); (work / 'tmp').mkdir()
        output = work / 'out.bin'
        code = 'import pathlib,sys; pathlib.Path(sys.argv[1]).write_bytes(bytes(784)); sys.exit(1)'
        with self.assertRaises(simd.HarnessError):
            simd.run_logged(work, 'run', [sys.executable, '-c', code, str(output)])
        self.assertEqual(output.stat().st_size, simd.OUT_SIZE)

    def test_timed_out_child_cannot_pass(self):
        work = self.work / 'timeout'; work.mkdir(); (work / 'tmp').mkdir()
        with self.assertRaises(simd.HarnessError):
            simd.run_logged(work, 'run', [sys.executable, '-c', 'import time; time.sleep(60)'], timeout=0.1)

    def test_reference_execution_error_cannot_pass(self):
        # Execute one real differential case and inject a reference execution
        # failure in the other. A passing subset cannot validate the requested sample.
        work = self.work / 'reference-error'; work.mkdir(); (work / 'tmp').mkdir()
        emu = simd.Emu()
        real_run = emu.run
        def inject_reference_error(pc, *args):
            if pc == simd.CODE + 4:
                raise simd.uc.UcError(simd.uc.UC_ERR_READ_UNMAPPED)
            return real_run(pc, *args)
        emu.run = inject_reference_error
        args = types.SimpleNamespace(seed=1, filter=None, no_validity=False,
                                     encodings=2, cases=1, jobs=1, cflags='', show=2)
        receipt = {}
        with mock.patch.object(simd, 'Emu', return_value=emu), \
             mock.patch.object(simd, 'candidates', return_value=[0x1e222820, 0x1e221820]):
            status = simd.run_suite(args, str(work), receipt)
        self.assertEqual(receipt['executed_cases'], 1)
        self.assertEqual(receipt['failing_cases'], 0)
        self.assertEqual(len(receipt['unicorn_skipped']), 1)
        self.assertEqual(status, 1, 'reference error must fail even when the executed subset matches')


if __name__ == '__main__':
    unittest.main()
