"""Authored native PE reuse fixtures; neither synthetic EXE nor DLL is run."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("reuse_fixture", PORT / "tools/game_library_reuse_windows.py")
reuse = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = reuse
spec.loader.exec_module(reuse)


@unittest.skipUnless(os.name == "nt", "native Windows PE/ownership fixture")
class NativeGameLibraryReuse(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="authored-gamelib-reuse-", dir=PORT / "build")
        cls.root = Path(cls.temp.name)
        cls.host = cls.root / "host.c"
        cls.host.write_text("int authored_counter=7; int authored_call(void){return authored_counter;} int main(void){return 0;}\n", encoding="utf-8")
        cls.guest = cls.root / "guest.c"
        cls.guest.write_text("__declspec(dllimport) extern int authored_counter; __declspec(dllimport) int authored_call(void); __declspec(dllexport) int lasgame_tables(void){return authored_call()+authored_counter;}\n", encoding="utf-8")
        cls.definition = cls.root / "host.def"
        cls.definition.write_text("LIBRARY xcde.exe\nEXPORTS\n authored_call\n authored_counter DATA\n", encoding="ascii")
        env = os.environ.copy()
        env["ZIG_GLOBAL_CACHE_DIR"] = str(PORT / ".deps/zig-global-cache")
        for command in ([sys.executable, "-m", "ziglang", "cc", str(cls.host), str(cls.definition), "-o", str(cls.root / "xcde.exe"), "-Wl,--out-implib," + str(cls.root / "host.dll.a")],
                        [sys.executable, "-m", "ziglang", "cc", str(cls.guest), str(cls.root / "host.dll.a"), "-shared", "-o", str(cls.root / "authored-game.dll")]):
            result = subprocess.run(command, env=env, capture_output=True, timeout=300)
            if result.returncode:
                raise RuntimeError(result.stderr.decode("utf-8", "replace"))
        cls.dll = (cls.root / "authored-game.dll").read_bytes()

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="version ü $() ` ", dir=self.root)
        self.previous = Path(self.temp.name)
        self.binary = self.previous / "build"
        self.binary.mkdir()
        self.identity = {"schema": 1, "format": "windows-amd64-pe", "abi": 1,
                         "compiler": {"version": "0.14.1", "sha256": "a" * 64},
                         "flags": ["-O1", "-UNDEBUG", "-ffp-contract=off"],
                         "input": {"main": {"sha256": "b" * 64, "bytes": 7}},
                         "generator_closure": {"runtime/cpu.h": "c" * 64},
                         "entry_sha256": "d" * 64, "native_game_helpers": {"runtime/platform/windows_fma.c": "e" * 64},
                         "host_imports": {"authored_call": "function", "authored_counter": "data"}}
        self.host_definitions = dict(self.identity["host_imports"])
        self.write_receipt()

    def tearDown(self):
        self.temp.cleanup()

    def write_receipt(self):
        identifier = reuse.canonical_id(self.identity)
        dll_sha = hashlib.sha256(self.dll).hexdigest()
        self.files = {"lasgame.dll": self.dll, "lasgame.dll.id": (identifier + "\n").encode(), "lasgame.dll.sha256": (dll_sha + "\n").encode()}
        for name, data in self.files.items():
            (self.binary / name).write_bytes(data)
        self.record = {"status": "success", "implementation": "windows-parity-development", "installation": {"state_root": str(self.previous)},
                       "flags": self.identity["flags"], "zig": self.identity["compiler"]["version"], "input": {"exefs": self.identity["input"]},
                       "generator_closure": self.identity["generator_closure"], "outputs": {"build/" + n: hashlib.sha256(v).hexdigest() for n, v in self.files.items()},
                       "split_game": {"id": identifier, "identity": self.identity, "dll_sha256": dll_sha,
                                      "sidecars": {suffix: hashlib.sha256(self.files["lasgame.dll" + suffix]).hexdigest() for suffix in (".id", ".sha256")}}}
        self.save_record()

    def save_record(self):
        (self.previous / "build-receipt.json").write_text(json.dumps(self.record), encoding="utf-8")

    def inspect(self, identity=None, definitions=None):
        expected = {k: v for k, v in (identity or self.identity).items() if k != "host_imports"}
        return reuse.inspect(self.previous, expected, self.host_definitions if definitions is None else definitions)

    def test_actual_native_pe_copy_between_unrelated_roots_without_execution(self):
        candidate = self.inspect()
        destination = self.root / ("fresh-output-" + self.previous.name)
        destination.mkdir()
        try:
            reuse.copy(candidate, destination)
            self.assertEqual((destination / "lasgame.dll").read_bytes(), self.dll)
            self.assertEqual(candidate.identifier, reuse.canonical_id(self.identity))
            self.assertEqual(candidate.imports, self.host_definitions)
            with self.assertRaises(OSError):
                reuse.copy(candidate, destination)
        finally:
            import shutil
            shutil.rmtree(destination)

    def test_input_compiler_flags_abi_guest_header_and_helper_changes_reject(self):
        for field in ("input", "compiler", "flags", "abi", "generator_closure", "native_game_helpers", "format", "entry_sha256"):
            with self.subTest(field=field):
                identity = copy.deepcopy(self.identity)
                identity[field] = "changed authored negative control"
                with self.assertRaisesRegex(reuse.IncompatibleGameLibrary, "differs"):
                    self.inspect(identity)

    def test_current_host_function_data_contract_must_match(self):
        for definitions in ({}, {"authored_call": "data", "authored_counter": "data"}, {"authored_call": "function", "authored_counter": "function"}):
            with self.assertRaisesRegex(reuse.IncompatibleGameLibrary, "function/data"):
                self.inspect(definitions=definitions)

    def test_corruption_is_fatal_even_when_current_identity_also_differs(self):
        identity = copy.deepcopy(self.identity)
        identity["abi"] += 1
        (self.binary / "lasgame.dll").write_bytes(self.dll + b"authored corruption")
        try:
            self.inspect(identity)
        except reuse.IncompatibleGameLibrary:
            self.fail("Corruption must never be classified as source fallback")
        except ValueError as error:
            self.assertIn("hash/size changed", str(error))
        else:
            self.fail("Corrupt library was accepted")

    def test_modified_dll_receipt_and_sidecar_rejected_without_repair(self):
        (self.binary / "lasgame.dll.id").write_bytes(b"0" * 64 + b"\n")
        with self.assertRaisesRegex(ValueError, "hash/size changed"):
            self.inspect()
        self.assertEqual((self.binary / "lasgame.dll.id").read_bytes(), b"0" * 64 + b"\n")
        self.record["outputs"]["build/lasgame.dll.id"] = hashlib.sha256(b"0" * 64 + b"\n").hexdigest()
        self.record["split_game"]["sidecars"][".id"] = self.record["outputs"]["build/lasgame.dll.id"]
        self.save_record()
        with self.assertRaisesRegex(ValueError, "content is corrupt"):
            self.inspect()

    def test_canonical_id_and_actual_pe_host_contract_must_match(self):
        self.record["split_game"]["id"] = "0" * 64
        self.save_record()
        with self.assertRaisesRegex(ValueError, "canonical ID"):
            self.inspect()
        self.identity["host_imports"].pop("authored_counter")
        self.write_receipt()
        with self.assertRaisesRegex(ValueError, "PE imports contradict"):
            self.inspect()

    def test_previous_reparse_root_and_hardlinked_receipt_rejected(self):
        import _winapi
        alias = self.root / ("junction-" + self.previous.name)
        _winapi.CreateJunction(str(self.previous), str(alias))
        try:
            with self.assertRaises(OSError):
                reuse.inspect(alias, {k: v for k, v in self.identity.items() if k != "host_imports"}, self.host_definitions)
        finally:
            alias.rmdir()
        link = self.previous / "receipt-hardlink.json"
        os.link(self.previous / "build-receipt.json", link)
        with self.assertRaises(OSError):
            self.inspect()


if __name__ == "__main__":
    unittest.main()
