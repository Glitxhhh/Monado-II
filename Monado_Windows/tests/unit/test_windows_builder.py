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
import zipfile

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
                   str(PORT / "tools/builder/windows_builder.cpp"), "-o", str(cls.exe), "-lbcrypt", "-ladvapi32", "-lcabinet", "-lole32", "-lshell32", "-lcomctl32", "-luuid", "-lgdi32", "-luser32", "-Wl,--subsystem,windows", "-municode"]
        result = subprocess.run(command, env=env, capture_output=True, timeout=300)
        if result.returncode:
            raise RuntimeError(result.stderr.decode("utf-8", "replace"))
        # Exercise the real reader callback while materialization is in progress,
        # without creating or interacting with a visible setup/game session.
        cancel_source = cls.base / "authored-cancel-reader.cpp"
        cancel_source.write_text(r'''
#include "../launcher/windows_preparation.h"
#include "../launcher/windows_precompile.h"
#include <cstdio>
using namespace monado_builder;
static void cancel_after_member(uint32_t, uint32_t) {
    throw std::runtime_error("authored cancellation boundary");
}
int wmain(int argc, wchar_t **argv) {
    try {
        if (argc == 2 && !wcscmp(argv[1], L"--check-fast-preparation-completion")) {
            monado_preparation::Worker worker;monado_preparation::Selection selection;
            for(unsigned attempt=0;attempt<2;++attempt){
                worker.start("authored relative path", "unused", "", selection, true);
                auto deadline=GetTickCount64()+5000;while(worker.snapshot().busy&&GetTickCount64()<deadline)Sleep(1);
                monado_preparation::Snapshot result;
                if(!worker.take_completion(result)||result.busy||result.status!="error"||result.message!="Preparation paths must be absolute")return 38;
                if(worker.take_completion(result))return 39;
            }
            return 0;
        }
        if (argc == 2 && !wcscmp(argv[1], L"--check-precompile-protocol")) {
            using namespace monado_precompile_windows;
            std::string session(32,'a'),identity(64,'b');
            std::string record="monado_precompile_work 1\nsession "+session+"\nidentity "+identity+"\nphase done\nstate complete\ngpu_cache_uuid "+std::string(32,'c')+"\n";
            for(auto key:{"files_total","files_done","shaders","programs","programs_done","seed_total","seed_done","variants_total","variants_done","attempted","built","failed","skipped","unsupported","errors","gpu_vendor","gpu_device","gpu_driver"})record+=std::string(key)+" 0\n";
            if(!parse(record,session,identity).complete(0)||parse(record,session,identity).complete(1))return 30;
            if(parse(record,std::string(32,'d'),identity).valid||parse(record,session,std::string(64,'e')).valid)return 31;
            for(auto key:{"failed","errors","skipped"}){
                auto negative=record;auto at=negative.find(std::string(key)+" 0\n");negative[at+strlen(key)+1]='1';
                if(parse(negative,session,identity).complete(0))return 32;
            }
            for(auto key:{"files_total","programs","seed_total","variants_total","attempted"}){
                auto unfinished=record;auto at=unfinished.find(std::string(key)+" 0\n");unfinished[at+strlen(key)+1]='1';
                if(parse(unfinished,session,identity).complete(0))return 40;
            }
            if(parse(record+"state complete\n",session,identity).valid)return 33;
            auto partial=record;partial.replace(partial.find("state complete"),strlen("state complete"),"state partial");
            return parse(partial,session,identity).complete(0)?34:0;
        }
        if (argc == 4 && !wcscmp(argv[1], L"--check-precompile-session")) {
            monado_precompile_windows::Session session;session.begin(utf8(argv[2]),utf8(argv[3]));
            if(session.session.size()!=32||session.identity.size()!=64||session.snapshot().valid)return 35;
            session.cancel();session.cancel(); // Idempotent, exact session only.
            monado_precompile_windows::Session other;other.begin(utf8(argv[2]),utf8(argv[3]));
            auto name=std::wstring(argv[3])+L"\\cache\\precompile_cancel-"+wide(other.session)+L".txt";
            {Handle unknown(CreateFileW(native(name).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));require(unknown.value!=INVALID_HANDLE_VALUE,"authored unknown cancel");write(unknown.value,"human",5);}
            try{other.cancel();return 36;}catch(const std::exception&){}
            Handle retained(CreateFileW(native(name).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));char data[5];read(retained.value,data,5);
            return memcmp(data,"human",5)?37:0;
        }
        if (argc == 4 && !wcscmp(argv[1], L"--check-preparation-context")) {
            monado_preparation::Context context;
            context.load(utf8(argv[2]), utf8(argv[3]), [](size_t,size_t){});
            return 0;
        }
        if (argc == 6 && !wcscmp(argv[1], L"--run-preparation-worker")) {
            monado_preparation::Worker worker;monado_preparation::Selection selection;
            selection.sdk=argv[5];worker.start(utf8(argv[2]),utf8(argv[3]),utf8(argv[4]),selection);
            auto start=GetTickCount64();bool requested=false;
            while(worker.snapshot().busy&&GetTickCount64()-start<15000){
                if(!wcscmp(argv[5],L"cancel-fixture")&&!requested&&worker.snapshot().message=="authored child ready"){worker.stop();requested=true;}
                Sleep(20);
            }
            auto result=worker.snapshot();
            if(result.busy)return 20;
            if(!wcscmp(argv[5],L"cancel-fixture"))return result.status=="cancelled"?0:21;
            return result.status=="error"&&result.message=="authored worker diagnostic"&&!result.log.empty()?0:22;
        }
        if (argc == 4 && !wcscmp(argv[1], L"--hold-payload")) {
            Handle input(CreateFileW(native(argv[2]).c_str(), GENERIC_READ,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            require(input.value != INVALID_HANDLE_VALUE, "held fixture input");
            auto payload = inspect(input.value); PayloadLease lease;
            materialize(input.value, payload, argv[3], nullptr, &lease);
            if (lease.file_count() != 2 || lease.directory_count() < 2) return 7;
            Handle write_attempt(CreateFileW(native(payload.cache+L"\\python\\authored.txt").c_str(),
                GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (write_attempt.value != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION) return 8;
            if (MoveFileExW(native(payload.cache+L"\\python").c_str(),
                native(payload.cache+L"\\swapped-python").c_str(),0)) return 9;
            return 0;
        }
        if (argc != 3) return 2;
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
    } catch (const std::exception &error) { fprintf(stderr, "%s\n", error.what()); return 6; }
}
''', encoding="utf-8")
        cls.cancel_exe = cls.base / "authored-cancel-reader.exe"
        result = subprocess.run(
            [sys.executable, "-m", "ziglang", "c++", "-std=c++17", "-O1",
             str(cancel_source), "-I", str(PORT / "tools/builder"),
             "-o", str(cls.cancel_exe), "-lbcrypt", "-ladvapi32", "-lcabinet", "-lole32", "-lshell32", "-luuid", "-municode"],
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

    def test_actual_native_bootstrap_leases_block_file_and_ancestor_replacement(self):
        target, sha = self.fixture()
        result = subprocess.run(
            [str(self.cancel_exe), "--hold-payload", str(target), str(self.cache)], timeout=30)
        self.assertEqual(result.returncode, 0)
        self.assertEqual((self.cache / sha / "python/authored.txt").read_bytes(), b"payload fixture\n" * 500)
        # All handles close when the owned fixture ends; it does not leave
        # permanent ACL changes, locks, or cleanup authority behind.
        (self.cache / sha / "python/authored.txt").write_bytes(b"authored post-lease change")
        self.assertNotEqual(self.check(target), 0)

    def preparation_context(self, *, interpreter=False):
        root = self.root / "preparation-runtime"
        root.mkdir()
        profile = self.root / "preparation-profile"
        profile.mkdir()
        files = {}
        if interpreter:
            home = root / "python"
            home.mkdir()
            archive = PORT / ".deps/python-archives/python-3.14.0-embed-amd64.zip"
            pin = json.loads((PORT / "tools/windows-python-runtime.lock.json").read_text())
            self.assertEqual(hashlib.sha256(archive.read_bytes()).hexdigest(), pin["sha256"])
            with zipfile.ZipFile(archive) as source:
                for item in source.infolist():
                    if not item.is_dir():
                        target = home / (item.filename + (".disabled" if item.filename == "python314._pth" else ""))
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(source.read(item))
            python = home / "python.exe"
            files.update({str(p): {"bytes": p.stat().st_size, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                          for p in home.rglob("*") if p.is_file()})
        else:
            python = root / "authored-python.txt"
            python.write_text("authored context data: never executed")
        worker = root / "authored_worker.py"
        worker.write_text('''import argparse,json,sys,time\nfrom pathlib import Path\np=argparse.ArgumentParser();p.add_argument('--progress');p.add_argument('--cancel-file');p.add_argument('--sdk');a,unused=p.parse_known_args()\nassert sys.flags.isolated and sys.flags.no_site and sys.dont_write_bytecode\nstatus='error';message='authored worker diagnostic'\nstream=Path(a.progress).open('x')\nif a.sdk=='cancel-fixture':\n    stream.write(json.dumps({'status':'running','message':'authored child ready','done':0,'total':1})+'\\n');stream.flush()\n    deadline=time.monotonic()+8\n    while not Path(a.cancel_file).exists() and time.monotonic()<deadline:time.sleep(.02)\n    assert Path(a.cancel_file).exists()\n    status='cancelled'\nstream.write(json.dumps({'status':status,'message':message,'done':1,'total':1})+'\\n');stream.flush();stream.close()\n''', encoding="utf-8")
        pipeline, guard = root / "authored_pipeline.txt", root / "authored_guard.txt"
        for path in (pipeline, guard):
            path.write_text("authored context data: never imported")
        for path in (python, worker, pipeline, guard):
            files[str(path)] = {"bytes": path.stat().st_size, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
        value = {"schema": 1, "platform": "windows-amd64", "status": "available", "reason": "",
                 "installation": str(profile), "receipt": str(root / "build-receipt.json"), "source_root": str(root),
                 "python": str(python), "worker": str(worker), "pipeline": str(pipeline), "guard": str(guard), "files": files}
        raw = json.dumps(value, ensure_ascii=True).encode()
        (root / "fsr4-preparation-context.json").write_bytes(raw)
        return root, profile, hashlib.sha256(raw).hexdigest(), worker

    def test_actual_native_preparation_context_rejects_changed_member_and_binding(self):
        root, _, identity, worker = self.preparation_context()
        command = [str(self.cancel_exe), "--check-preparation-context", str(root), identity]
        self.assertEqual(subprocess.run(command, timeout=30).returncode, 0)
        worker.write_text("changed authored helper: preserve and refuse")
        self.assertNotEqual(subprocess.run(command, timeout=30).returncode, 0)
        command[-1] = "0" * 64
        self.assertNotEqual(subprocess.run(command, timeout=30).returncode, 0)

    def test_actual_native_preparation_worker_reports_failure_without_game(self):
        root, profile, identity, _ = self.preparation_context(interpreter=True)
        self.assertEqual(subprocess.run([str(self.cancel_exe), "--run-preparation-worker", str(root), str(profile), identity, "error-fixture"], timeout=30).returncode, 0)

    def test_actual_native_preparation_worker_cancels_owned_child(self):
        root, profile, identity, _ = self.preparation_context(interpreter=True)
        self.assertEqual(subprocess.run([str(self.cancel_exe), "--run-preparation-worker", str(root), str(profile), identity, "cancel-fixture"], timeout=30).returncode, 0)

    def test_actual_native_preparation_completion_before_first_ui_poll(self):
        self.assertEqual(subprocess.run([str(self.cancel_exe), "--check-fast-preparation-completion"], timeout=30).returncode, 0)

    def test_actual_native_precompile_status_requires_session_identity_and_clean_counters(self):
        self.assertEqual(subprocess.run([str(self.cancel_exe), "--check-precompile-protocol"], timeout=30).returncode, 0)

    def test_actual_native_precompile_session_cancel_preserves_unknown_file(self):
        runtime, profile = self.root / "authored-runtime", self.root / "authored-profile"
        (runtime / "build").mkdir(parents=True)
        profile.mkdir()
        (runtime / "build/xcde.exe").write_bytes(b"authored image identity fixture: never executed")
        self.assertEqual(subprocess.run([str(self.cancel_exe), "--check-precompile-session", str(runtime), str(profile)], timeout=30).returncode, 0)
        files = list((profile / "cache").glob("precompile_cancel-*.txt"))
        self.assertEqual(len(files), 2)
        self.assertIn(b"human", [p.read_bytes() for p in files])

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
