"""Native authored payload/hidden-window qualification; never starts a game."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("windows_builder_packager", PORT / "tools/build_windows_builder.py")
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)


@unittest.skipUnless(os.name == "nt", "native Windows payload/compiler fixture")
class NativePortableBuilder(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (PORT / "build").mkdir(exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix="authored-builder-", dir=PORT / "build")
        cls.base = Path(cls.temp.name)
        cls.exe = cls.base / "authored-native-builder.exe"
        env = os.environ.copy()
        env["ZIG_GLOBAL_CACHE_DIR"] = str(PORT / ".deps/zig-global-cache")
        command = [sys.executable, "-m", "ziglang", "c++", "-std=c++17", "-O1", "-UNDEBUG", "-march=x86_64_v2", "-ffp-contract=off", "-fno-fast-math",
                   str(PORT / "tools/builder/windows_builder.cpp"), "-o", str(cls.exe), "-lbcrypt", "-lcabinet", "-lole32", "-lshell32", "-lcomctl32", "-luuid", "-lgdi32", "-luser32", "-Wl,--subsystem,windows", "-municode"]
        result = subprocess.run(command, env=env, capture_output=True, timeout=300)
        if result.returncode:
            raise RuntimeError(result.stderr.decode("utf-8", "replace"))
        # Exercise the real reader callback while materialization is in progress,
        # without creating or interacting with a visible setup/game session.
        cancel_source = cls.base / "authored-cancel-reader.cpp"
        cancel_source.write_text(r'''
#include "payload_windows.h"
using namespace monado_builder;
static void cancel_after_member(uint32_t, uint32_t) {
    throw std::runtime_error("authored cancellation boundary");
}
int wmain(int argc, wchar_t **argv) {
    if (argc != 3) return 2;
    try {
        if (!wcscmp(argv[1], L"--canonical-directory")) {
            auto path = utf8(final_directory(argv[2]));
            write(GetStdHandle(STD_OUTPUT_HANDLE), path.data(), path.size());
            return 0;
        }
        Handle input(CreateFileW(native(argv[1]).c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        require(input.value != INVALID_HANDLE_VALUE, "fixture input");
        auto payload = inspect(input.value);
        try {
            materialize(input.value, payload, argv[2], cancel_after_member);
            return 3;
        } catch (const std::runtime_error &error) {
            if (std::string(error.what()) != "authored cancellation boundary") return 4;
        }
        return ordinary(payload.cache + L"\\.complete") ? 5 : 0;
    } catch (...) { return 6; }
}
''', encoding="utf-8")
        cls.cancel_exe = cls.base / "authored-cancel-reader.exe"
        result = subprocess.run(
            [sys.executable, "-m", "ziglang", "c++", "-std=c++17", "-O1",
             str(cancel_source), "-I", str(PORT / "tools/builder"),
             "-o", str(cls.cancel_exe), "-lbcrypt", "-lcabinet", "-municode"],
            env=env, capture_output=True, timeout=300)
        if result.returncode:
            raise RuntimeError(result.stderr.decode("utf-8", "replace"))

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.case = tempfile.TemporaryDirectory(prefix="ü literal $() ` ", dir=self.base)
        self.root = Path(self.case.name)
        self.cache = self.root / "cache ü $() `"
        self.cache.mkdir()

    def tearDown(self):
        self.case.cleanup()

    def fixture(self, pairs=None, corrupt_member=False):
        pairs = pairs or [("payload-manifest.json", b'{"authored":true}\n'), ("python/authored.txt", b"payload fixture\n" * 500)]
        raw = bytearray(pack.HEADER.pack(b"MONADOP1", 1, len(pairs), sum(len(v) for _, v in pairs)))
        codec = pack.Compressor()
        try:
            for key, data in pairs:
                flag, stored = codec.compress(data)
                sha = b"x" * 32 if corrupt_member else hashlib.sha256(data).digest()
                name = key.encode()
                raw += pack.RECORD.pack(len(name), flag, len(data), len(stored), sha) + name + stored
        finally:
            codec.close()
        body = self.exe.read_bytes()
        target = self.root / "authored ü $() `.exe"
        target.write_bytes(body + raw + pack.FOOTER.pack(b"M2BNDL01", len(body), len(raw), hashlib.sha256(raw).digest()))
        return target, hashlib.sha256(raw).hexdigest()

    def check(self, target):
        return subprocess.run([str(self.exe), "--check-payload", str(target), str(self.cache)], timeout=30).returncode

    def test_actual_native_extract_reopen_and_unicode_literal_paths(self):
        target, sha = self.fixture()
        self.assertEqual(self.check(target), 0)
        self.assertEqual((self.cache / sha / "python/authored.txt").read_bytes(), b"payload fixture\n" * 500)
        self.assertEqual(self.check(target), 0)

    def test_actual_native_member_order_uses_canonical_posix_names(self):
        # '/' sorts before '0'; the native '\\' separator sorts after it.
        pairs = [("A/x", b"nested authored member"), ("a0.h", b"sibling authored member")]
        self.assertEqual([key for key, _ in pairs], sorted((key for key, _ in pairs), key=str.lower))
        target, sha = self.fixture(pairs)
        self.assertEqual(self.check(target), 0)
        self.assertEqual((self.cache / sha / "A/x").read_bytes(), pairs[0][1])
        self.assertEqual((self.cache / sha / "a0.h").read_bytes(), pairs[1][1])
        self.assertEqual(self.check(target), 0)
        target, _ = self.fixture(list(reversed(pairs)))
        self.assertNotEqual(self.check(target), 0)

    def test_actual_native_outer_and_member_corruption_fail(self):
        target, _ = self.fixture()
        data = bytearray(target.read_bytes())
        data[-70] ^= 1
        target.write_bytes(data)
        self.assertNotEqual(self.check(target), 0)
        target, _ = self.fixture(corrupt_member=True)
        self.assertNotEqual(self.check(target), 0)

    def test_actual_native_traversal_reserved_and_duplicate_names_fail(self):
        for key in ("../outside.txt", "python/CON.txt", "python/name.", "C:/outside.txt", "python\\outside.txt", "python/é.txt"):
            with self.subTest(key=key):
                target, _ = self.fixture([(key, b"authored")])
                self.assertNotEqual(self.check(target), 0)
        target, _ = self.fixture([("Python/a", b"a"), ("python/A", b"b")])
        self.assertNotEqual(self.check(target), 0)

    def test_actual_native_partial_retry_preserves_valid_members(self):
        target, sha = self.fixture()
        root = self.cache / sha
        (root / "python").mkdir(parents=True)
        (root / "python/authored.txt").write_bytes(b"payload fixture\n" * 500)
        self.assertEqual(self.check(target), 0)
        self.assertTrue((root / ".complete").is_file())

    def test_interrupted_staging_temporary_does_not_block_retry(self):
        target, sha = self.fixture()
        state = self.cache / sha / "state"
        state.mkdir(parents=True)
        partial = state / "extract-authored-interruption.part"
        partial.write_bytes(b"incomplete staged bytes: preserve")
        self.assertEqual(self.check(target), 0)
        self.assertEqual(partial.read_bytes(), b"incomplete staged bytes: preserve")
        self.assertEqual((self.cache / sha / "python/authored.txt").read_bytes(), b"payload fixture\n" * 500)

    def test_actual_native_cancel_during_materialize_preserves_retry(self):
        target, sha = self.fixture()
        result = subprocess.run(
            [str(self.cancel_exe), str(target), str(self.cache)], timeout=30)
        self.assertEqual(result.returncode, 0)
        root = self.cache / sha
        self.assertEqual((root / "payload-manifest.json").read_bytes(), b'{"authored":true}\n')
        self.assertFalse((root / "python/authored.txt").exists())
        self.assertFalse((root / ".complete").exists())
        self.assertEqual(self.check(target), 0)
        self.assertEqual((root / "python/authored.txt").read_bytes(), b"payload fixture\n" * 500)
        self.assertTrue((root / ".complete").is_file())

    def test_actual_native_cache_paths_beyond_legacy_max_path(self):
        self.cache = self.cache / ("long-authored-component-" * 4)
        self.cache.mkdir()
        target, sha = self.fixture()
        self.assertGreater(len(str(self.cache / sha / "python/authored.txt")), 260)
        self.assertEqual(self.check(target), 0)
        self.assertEqual(self.check(target), 0)

    def test_actual_native_appdata_final_path_matches_python_canonical_path(self):
        # Under an MSIX-packaged host, the final ordinary directory may differ
        # from the logical environment value despite having no reparse point.
        for path in (self.cache, Path(os.environ["LOCALAPPDATA"])):
            with self.subTest(path=str(path)):
                result = subprocess.run(
                    [str(self.cancel_exe), "--canonical-directory", str(path)],
                    capture_output=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stderr)
                physical = Path(result.stdout.decode("utf-8"))
                self.assertEqual(physical, path.resolve(strict=True))
                self.assertEqual(physical.resolve(strict=True), physical)

    def test_actual_native_final_directory_rejects_reparse_leaf(self):
        import _winapi
        outside = self.root / "outside"
        outside.mkdir()
        link = self.root / "cache-junction"
        _winapi.CreateJunction(str(outside), str(link))
        try:
            result = subprocess.run(
                [str(self.cancel_exe), "--canonical-directory", str(link)], timeout=30)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(list(outside.iterdir()), [])
        finally:
            link.rmdir()

    def test_actual_native_modified_or_undeclared_cache_preserved(self):
        target, sha = self.fixture()
        self.assertEqual(self.check(target), 0)
        root = self.cache / sha
        (root / "sitecustomize.py").write_text("authored injection negative control")
        self.assertNotEqual(self.check(target), 0)
        self.assertTrue((root / "sitecustomize.py").is_file())
        (root / "sitecustomize.py").unlink()
        (root / "python/authored.txt").write_text("human modification preserved")
        self.assertNotEqual(self.check(target), 0)
        self.assertEqual((root / "python/authored.txt").read_text(), "human modification preserved")

    def test_actual_native_reparse_parent_rejected(self):
        import _winapi
        target, sha = self.fixture()
        root = self.cache / sha
        root.mkdir()
        outside = self.root / "outside"
        outside.mkdir()
        _winapi.CreateJunction(str(outside), str(root / "python"))
        try:
            self.assertNotEqual(self.check(target), 0)
            self.assertEqual(list(outside.iterdir()), [])
        finally:
            (root / "python").rmdir()

    def test_hidden_native_setup_window_defaults_and_controls(self):
        result = subprocess.run([str(self.exe), "--hidden-ui-fixture"], timeout=30)
        self.assertEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
