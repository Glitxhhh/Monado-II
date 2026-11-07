import os, subprocess, sys, tempfile, unittest

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import hash_diff  # noqa: E402
import hle_audit  # noqa: E402


class HashDiffTest(unittest.TestCase):
    def test_identical_and_shifted(self):
        a = {i: f'{i:016x}' for i in range(1, 11)}
        self.assertEqual(hash_diff.compare(a, dict(a))['exact'], 10)
        b = {i + 1: h for i, h in a.items()}  # run b is one frame behind
        r = hash_diff.compare(a, b, max_shift=2)
        self.assertEqual(r['prefix'], 0)
        self.assertGreaterEqual(r['shifted'], 9)

    def test_load_skips_comments(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 'h.txt')
            with open(p, 'w') as f:
                f.write('# header\n3 00000000000000ff\n\n4 0000000000000100\n')
            self.assertEqual(hash_diff.load(p), {3: '00000000000000ff', 4: '0000000000000100'})


class HleAuditTest(unittest.TestCase):
    def test_audit_is_current(self):
        r = subprocess.run([sys.executable, '-X', 'utf8', os.path.join(ROOT, 'tools', 'hle_audit.py'), '--check'],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)

    def test_nested_name_demangling(self):
        self.assertEqual(hle_audit.demangle('_ZN2nn2vi10InitializeEv'), 'nn::vi::Initialize')
        self.assertEqual(hle_audit.demangle('plain_symbol'), 'plain_symbol')

    def test_manifest_classes_are_valid(self):
        for kind, name, cls, note in hle_audit.load_manifest():
            self.assertIn(cls, hle_audit.CLASSES)
            self.assertIn(kind, ('svc', 'service', 'func'))

    def test_every_svc_is_inventoried(self):
        svcs, services, funcs = hle_audit.scan()
        ids = {i for i, _, _ in svcs}
        for needed in (0x01, 0x1a, 0x1b, 0x1c, 0x1d, 0x21):
            self.assertIn(needed, ids)
        self.assertTrue(any(n == 'audout:u' for n, _, _ in services))


if __name__ == '__main__':
    unittest.main()
