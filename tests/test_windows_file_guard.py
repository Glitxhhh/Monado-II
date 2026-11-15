"""Authored disposable native Win32 leases; no game, profiles or user-file mutations."""
import ctypes as C
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
import windows_file_guard as guard

@unittest.skipUnless(os.name=='nt','Native Win32 only; no production fallback')
class NativeFileGuard(unittest.TestCase):
    def setUp(self):
        private=ROOT/'Monado_Windows/runs/file-guard-fixtures';private.mkdir(parents=True,exist_ok=True)
        self.temporary=tempfile.TemporaryDirectory(prefix='authored-',dir=private)
        self.root=Path(self.temporary.name);self.profile=self.root/'profile-résumé-雪-$()-`';self.profile.mkdir()
    def tearDown(self):self.temporary.cleanup()
    def test_unicode_owned_new_read_stream(self):
        data=b'authored'*300000
        with guard.locked_directory(self.profile,owner=True) as directory:
            directory.write_new('literal-$(text)-`.bin',data)
            with directory.open_regular('literal-$(text)-`.bin',max_bytes=len(data),owner=True) as stream:
                self.assertEqual(hashlib.file_digest(stream,'sha256').digest(),hashlib.sha256(data).digest())
            self.assertEqual(directory.read_regular('literal-$(text)-`.bin',max_bytes=len(data),owner=True),data)
    def test_ancestor_rename_blocked_until_exit(self):
        with guard.locked_directory(self.profile):
            with self.assertRaises(OSError):self.root.rename(str(self.root)+'-renamed')
        moved=self.root.with_name(self.root.name+'-renamed');self.root.rename(moved);moved.rename(self.root)
    def test_hardlink_and_bounds_rejected(self):
        target=self.profile/'authored.bin';target.write_bytes(b'abc');alias=self.profile/'alias.bin';os.link(target,alias)
        with guard.locked_directory(self.profile) as directory:
            with self.assertRaises(guard.FileGuardError):directory.read_regular('authored.bin')
            alias.unlink()
            with self.assertRaises(guard.FileGuardError):directory.read_regular('authored.bin',max_bytes=2)
    def test_parent_junction_rejected(self):
        target=self.root/'authored-target';target.mkdir();(target/'sentinel').write_bytes(b'preserve')
        alias=self.root/'authored-junction'
        result=subprocess.run(['cmd','/d','/c','mklink','/J',str(alias),str(target)],capture_output=True,text=True)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        try:
            with self.assertRaises(guard.FileGuardError):
                with guard.locked_directory(alias):pass
            self.assertEqual((target/'sentinel').read_bytes(),b'preserve')
        finally:alias.rmdir()
    def test_leaf_read_denies_writer_and_rename(self):
        p=self.profile/'file.bin';p.write_bytes(b'fixed')
        with guard.locked_directory(self.profile) as directory:
            with directory.open_regular('file.bin'):
                handle=guard.K.CreateFileW(guard._native(str(p)),0x40000000,3,None,3,guard._NOFOLLOW,None)
                if handle!=guard._INVALID:guard.K.CloseHandle(handle)
                self.assertEqual(handle,guard._INVALID)
                with self.assertRaises(OSError):p.rename(self.profile/'moved.bin')
        self.assertEqual(p.read_bytes(),b'fixed')
    def test_atomic_publish_new_preserves_existing(self):
        with guard.locked_directory(self.profile,owner=True) as directory:
            directory.atomic_write_new('marker.json',b'{"version":1}')
            with self.assertRaises(guard.FileGuardError):directory.atomic_write_new('marker.json',b'overwrite')
            self.assertEqual(directory.read_regular('marker.json'),b'{"version":1}')
    def test_replace_requires_owner_hash_and_preserves_on_failure(self):
        old=b'{"schema":1}';new=b'{"schema":2}';digest=hashlib.sha256(old).hexdigest()
        with guard.locked_directory(self.profile,owner=True) as directory:
            directory.write_new('known.json',old)
            with self.assertRaises(guard.FileGuardError):directory.atomic_replace('known.json',new,'0'*64)
            self.assertEqual(directory.read_regular('known.json'),old)
            with self.assertRaises(ValueError):directory.atomic_replace('known.json',new,None)
            directory.atomic_replace('known.json',new,digest)
            self.assertEqual(directory.read_regular('known.json'),new)
    def test_locked_target_replace_preserves_original(self):
        old=b'original';digest=hashlib.sha256(old).hexdigest()
        with guard.locked_directory(self.profile) as directory:
            directory.write_new('locked.bin',old)
            with directory.open_regular('locked.bin'):
                with self.assertRaises(guard.FileGuardError):directory.atomic_replace('locked.bin',b'new',digest)
            self.assertEqual(directory.read_regular('locked.bin'),old)
    def test_child_create_and_path_rejections(self):
        with guard.locked_directory(self.profile) as directory:
            with directory.child('new/雪',create=True,owner=True) as child:
                child.write_new('owned',b'ok');self.assertEqual(child.read_regular('owned'),b'ok')
            for path in ('../escape','C:/escape','/escape','file:stream','NUL.txt','name.','name ','a//b'):
                with self.subTest(path=path),self.assertRaises(ValueError):directory.read_regular(path)
        for path in ('C:relative','\\relative',r'\\?\C:\anything',r'\\.\NUL'):
            with self.subTest(path=path),self.assertRaises(ValueError):guard.canonical_path(path)
        # Lexical canonicalization does not contact or create an SMB share.
        self.assertEqual(guard.canonical_path(r'\\authored-server\share'),r'\\authored-server\share'+'\\')
        self.assertEqual(guard.canonical_path(r'\\authored-server\share'+'\\'),r'\\authored-server\share'+'\\')
    def test_installation_lease_excludes_second_process(self):
        code="import sys;sys.path.insert(0,sys.argv[1]);import windows_file_guard as g;\ntry:\n with g.installation_lease(sys.argv[2],timeout_ms=0): pass\nexcept TimeoutError: sys.exit(23)"
        command=[sys.executable,"-X","utf8","-B","-c",code,str(ROOT/'tools'),str(self.profile)]
        with guard.installation_lease(self.profile,timeout_ms=0):
            result=subprocess.run(command,capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,23,result.stdout+result.stderr)
        result=subprocess.run(command,capture_output=True,text=True,timeout=10)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_writer_mutex_is_named_and_reentrant_same_thread(self):
        with guard.locked_directory(self.profile) as directory:
            name=directory._path('mutex.bin')
            with guard._mutex(name),guard._mutex(name):directory.write_new('mutex.bin',b'once')
            self.assertEqual(directory.read_regular('mutex.bin'),b'once')

if __name__=='__main__':unittest.main()
