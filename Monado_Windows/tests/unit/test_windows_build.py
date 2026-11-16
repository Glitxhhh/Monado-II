"""Authored native regression probes for the Windows build boundary."""
import argparse
import importlib.util
import os
import json
import shutil
import subprocess
import sys
from pathlib import Path
import tempfile
import unittest
from unittest import mock

PORT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("windows_build", PORT / "scripts/build_windows.py")
build = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build)


@unittest.skipUnless(os.name == "nt", "native Windows compiler/process fixture")
class NativeBuildBoundary(unittest.TestCase):
    def setUp(self):
        (PORT / "build").mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix="authored-build-", dir=PORT / "build")
        self.base = Path(self.temp.name)
        args = argparse.Namespace(output=self.base / "output ü $() ` space", prefix=None,
                                  stage="runtime", jobs=1, keep_going=False, split_game=False)
        self.builder = build.Builder(args)

    def tearDown(self):
        self.builder.guard.close()
        self.temp.cleanup()

    def test_strict_flags_and_literal_unicode_source_argument(self):
        source = self.base / "literal ü $() ` space.c"
        source.write_text("#ifdef NDEBUG\n#error assertions disabled\n#endif\n#ifdef __FAST_MATH__\n#error fast math enabled\n#endif\nint authored_fixture(void) { return 7; }\n", encoding="utf-8")
        objects = self.builder.compile([source])
        self.assertEqual(len(objects), 1)
        self.assertTrue(objects[0].is_file())
        command = self.builder.receipt["commands"][-1]
        self.assertIn(str(source), command["argv"])
        self.assertEqual(command["exit"], 0)

    def test_actual_compiler_failure_is_fatal_and_retains_diagnostics(self):
        source = self.base / "deliberately-invalid.c"
        source.write_text("#error authored negative control\n", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "failed with exit"):
            self.builder.compile([source])
        command = self.builder.receipt["commands"][-1]
        self.assertNotEqual(command["exit"], 0)
        self.assertIn("authored negative control", Path(command["stderr"]).read_text(encoding="utf-8"))
        self.assertEqual(list((self.builder.output / "obj").glob("*.o")), [])

    def test_existing_output_is_preserved(self):
        marker = self.builder.output / "human.txt"
        marker.write_text("preserve", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "existing build preserved"):
            build.Builder(self.builder.args)
        self.assertEqual(marker.read_text(encoding="utf-8"), "preserve")

    def test_cleanup_authority_is_exact_recipe_and_excludes_human_state(self):
        self.builder.preflight()
        extras = ("human-note.txt", "xcde.ini", "saves/human.save", "build/foreign.dll")
        for name in extras:
            target = self.builder.output / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(b"human data: retain and refuse cleanup")
        self.builder.receipt["outputs"] = self.builder.output_inventory()
        identities = self.builder.retention_inventory()
        self.assertIn("dependency-probe.exe", identities)
        self.assertIn("preflight.c", identities)
        self.assertNotIn("build-receipt.json", identities)
        for name in extras:
            self.assertNotIn(name, identities)
            self.assertEqual((self.builder.output / name).read_bytes(), b"human data: retain and refuse cleanup")

    def test_linked_manifest_enables_utf8_ansi_file_arguments(self):
        source = self.base / "utf8-manifest.c"
        source.write_text('#include <windows.h>\n#include <stdio.h>\nint main(int argc,char **argv) { if(GetACP()!=CP_UTF8 || argc!=2) return 2; FILE *f=fopen(argv[1],"wb"); if(!f) return 3; fputs("authored UTF8",f); return fclose(f)!=0; }\n', encoding="utf-8")
        objects = self.builder.compile([source])
        self.builder.libs = []
        exe = self.builder.link("authored-utf8.exe", objects)
        target = self.base / "authored ü literal $() `.txt"
        self.builder.run("run-authored-utf8", [str(exe), str(target)])
        self.assertEqual(target.read_text(encoding="utf-8"), "authored UTF8")

    def test_native_coff_exact_host_import_inventory_and_truncation(self):
        coff_spec = importlib.util.spec_from_file_location("coff_fixture", PORT / "tools/coff_symbols.py")
        coff = importlib.util.module_from_spec(coff_spec)
        coff_spec.loader.exec_module(coff)
        host, guest = self.base / "authored-host.c", self.base / "authored-guest.c"
        host.write_text("int authored_counter=7; int authored_call(int x){return x+1;} int unused_host(void){return 0;}\n", encoding="utf-8")
        guest.write_text("extern int authored_counter; extern int authored_call(int); int authored_guest(void){return authored_call(authored_counter);}\n", encoding="utf-8")
        host_object = self.builder.compile([host])[0]
        guest_object = self.builder.compile([guest])[0]
        self.assertEqual(coff.host_imports([guest_object], [host_object]), {"authored_call": "function", "authored_counter": "data"})
        malformed = self.base / "truncated.o"
        malformed.write_bytes(guest_object.read_bytes()[:24])
        with self.assertRaisesRegex(ValueError, "outside object"):
            coff.symbols(malformed)


class RecursiveGuestClosure(unittest.TestCase):
    def test_actual_nested_generator_disables_bytecode_even_when_environment_is_ignored(self):
        with tempfile.TemporaryDirectory(dir=PORT / "build") as directory:
            root = Path(directory)
            tools = root / "tools"
            lifter = tools / "lifter"
            package = lifter / "authored_dependency"
            package.mkdir(parents=True)
            (package / "__init__.py").write_text("VALUE = 7\n", encoding="utf-8")
            generator = lifter / "gen.py"
            generator.write_text(
                "import sys\nfrom pathlib import Path\nfrom authored_dependency import VALUE\n"
                "if not sys.dont_write_bytecode: raise SystemExit(17)\n"
                "out=Path(sys.argv[4]); out.mkdir(parents=True)\n"
                "(out/'authored.c').write_text('int fixture = %d;\\n' % VALUE)\n", encoding="utf-8")
            copied_driver = tools / "gen_all.py"
            shutil.copyfile(PORT / "tools/gen_all.py", copied_driver)
            profile = root / "profile.json"
            profile.write_text(json.dumps({"modules": [{"name": "authored", "base": "0", "lift": True}]}), encoding="utf-8")
            environment = os.environ.copy()
            environment["PYTHONDONTWRITEBYTECODE"] = "1"
            result = subprocess.run([sys.executable, "-B", str(copied_driver), "--profile", str(profile), "--out", str(root / "generated")],
                                    env=environment, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue((root / "generated/authored/authored.c").is_file())
            self.assertFalse(list(root.rglob("__pycache__")))
            # Negative control proves inherited PYTHONDONTWRITEBYTECODE alone
            # is ineffective when the actual nested child is launched with -E.
            result = subprocess.run([sys.executable, "-E", "-s", str(generator), "authored", "unused", "0", str(root / "negative-output")],
                                    env=environment, capture_output=True, timeout=30)
            self.assertEqual(result.returncode, 17, result.stdout + result.stderr)
            self.assertTrue((package / "__pycache__").is_dir())

    def test_source_export_does_not_query_an_enclosing_repository(self):
        with tempfile.TemporaryDirectory(dir=PORT / "build") as directory:
            root = Path(directory) / "source/Monado_Windows"
            root.mkdir(parents=True)
            with mock.patch.object(build, "ROOT", root), mock.patch.object(build.subprocess, "check_output") as git:
                self.assertEqual(build.git_identity(), {"git_commit": None, "git_branch": None})
                git.assert_not_called()

    def test_git_identity_requires_its_own_repository_marker(self):
        with tempfile.TemporaryDirectory(dir=PORT / "build") as directory:
            root = Path(directory) / "Monado_Windows"
            root.mkdir()
            (root.parent / ".git").mkdir()
            with mock.patch.object(build, "ROOT", root), mock.patch.object(build.subprocess, "check_output", side_effect=["authored-hash\n", "codex/authored\n"]) as git:
                self.assertEqual(build.git_identity(), {"git_commit": "authored-hash", "git_branch": "codex/authored"})
                for call in git.call_args_list:
                    self.assertEqual(call.kwargs["cwd"], root.parent)
                    self.assertEqual(call.kwargs["stderr"], build.subprocess.DEVNULL)

    def test_nested_and_conditional_guest_headers_are_bound(self):
        original = build.ROOT
        with tempfile.TemporaryDirectory() as directory:
            try:
                build.ROOT = Path(directory).resolve()
                for name in ("tools/lifter/gen.py", "tools/gen_all.py", "games/xcde/profile.json", "games/xcde/patches.py", "games/xcde/hle_symbols.txt"):
                    p = build.ROOT / name
                    p.parent.mkdir(parents=True, exist_ok=True)
                    p.write_text("authored fixture", encoding="utf-8")
                runtime = build.ROOT / "runtime"
                runtime.mkdir()
                (runtime / "platform").mkdir()
                (runtime / "platform/windows_fma.c").write_text("authored helper identity fixture", encoding="utf-8")
                (runtime / "cpu.h").write_text('#include "simd.h"\n', encoding="utf-8")
                (runtime / "gamelib.h").write_text('#include "cpu.h"\n#define GAMELIB_ABI 1\n', encoding="utf-8")
                (runtime / "simd.h").write_text('#if 0\n#include "nested.h"\n#endif\n#include "cpu.h"\n', encoding="utf-8")
                (runtime / "nested.h").write_text("first", encoding="utf-8")
                (runtime / "unrelated.h").write_text("first", encoding="utf-8")
                before = build.generator_closure()
                (runtime / "unrelated.h").write_text("second", encoding="utf-8")
                self.assertEqual(before, build.generator_closure())
                (runtime / "nested.h").write_text("second", encoding="utf-8")
                self.assertFalse(build.compatible_generator_closure(before, build.generator_closure()))
                (runtime / "nested.h").unlink()
                with self.assertRaisesRegex(RuntimeError, "Invalid guest header"):
                    build.generator_closure()
            finally:
                build.ROOT = original


if __name__ == "__main__":
    unittest.main()
