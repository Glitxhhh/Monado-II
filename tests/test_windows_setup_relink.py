"""Authored PE-shaped prefixes and portable payloads; never execute a PE."""
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
from unittest import mock

ROOT=Path(__file__).resolve().parents[1]
SCRIPT=ROOT/'Monado_Windows/tools/relink_windows_setup.py'
spec=importlib.util.spec_from_file_location('authored_setup_relink',SCRIPT)
relink=importlib.util.module_from_spec(spec);spec.loader.exec_module(relink)


def pe(size, marker):
    raw=bytearray(size);raw[:2]=b'MZ';struct.pack_into('<I',raw,0x3c,128)
    raw[128:132]=b'PE\0\0';struct.pack_into('<HH',raw,132,0x8664,1)
    struct.pack_into('<HH',raw,148,240,0x22)
    struct.pack_into('<H',raw,152,0x20b);struct.pack_into('<H',raw,220,2)
    raw[1024:1024+len(marker)]=marker
    return bytes(raw)


def payload(data):
    name=b'authored.txt'
    record=struct.pack('<IIQQ32s',len(name),0,len(data),len(data),hashlib.sha256(data).digest())
    return relink.HEADER.pack(b'MONADOP1',1,1,len(data))+record+name+data


def bundle(prefix, data, *, offset=None, count=None, digest=None):
    return prefix+data+relink.FOOTER.pack(relink.MAGIC,len(prefix) if offset is None else offset,
        len(data) if count is None else count,hashlib.sha256(data).digest() if digest is None else digest)


class SetupRelink(unittest.TestCase):
    def setUp(self):
        self.temporary=tempfile.TemporaryDirectory(prefix='monado-authored-relink-')
        self.addCleanup(self.temporary.cleanup)
        self.root=Path(self.temporary.name).resolve()/'Unicode 漢字 $() `'
        self.root.mkdir()
        self.original=self.root/'original setup.exe';self.base=self.root/'rebuilt base.exe';self.output=self.root/'modified setup.exe'
        self.old_base=pe(4096,b'Authored old frontend; not executable test code')
        self.new_base=pe(8192,b'Authored modified frontend; not executable test code')
        self.data=payload(b'Authored payload data\n'*8000)
        self.original_bytes=bundle(self.old_base,self.data)
        self.original.write_bytes(self.original_bytes);self.base.write_bytes(self.new_base)

    def assert_inputs_preserved(self):
        self.assertEqual(self.original.read_bytes(),self.original_bytes)
        self.assertEqual(self.base.read_bytes(),self.new_base)

    def test_exact_payload_new_frontend_and_adjusted_footer_match_independent_oracle(self):
        with mock.patch.object(relink,'CHUNK',4096):
            receipt=relink.repack(self.original,self.base,self.output)
        expected=bundle(self.new_base,self.data)
        self.assertEqual(self.output.read_bytes(),expected)
        self.assertEqual(receipt['new_offset'],8192)
        self.assertEqual(receipt['original_offset'],4096)
        self.assertEqual(receipt['output_sha256'],hashlib.sha256(expected).hexdigest())
        self.assertEqual(receipt['original_setup_sha256'],hashlib.sha256(self.original_bytes).hexdigest())
        self.assertEqual(receipt['payload_sha256'],hashlib.sha256(self.data).hexdigest())
        self.assertTrue(receipt['payload_unchanged']);self.assertFalse(receipt['executable_run'])
        with self.output.open('rb') as stream:
            offset,size,digest=relink._payload(stream,len(expected))
        self.assertEqual((offset,size,digest),(8192,len(self.data),hashlib.sha256(self.data).digest()))
        self.assert_inputs_preserved()

    def test_cli_is_isolated_stdlib_only_and_never_executes_the_pe_inputs(self):
        result=subprocess.run([sys.executable,'-I','-S','-B',str(SCRIPT),'--original-setup',str(self.original),
            '--base-executable',str(self.base),'--output',str(self.output)],capture_output=True,timeout=30)
        self.assertEqual(result.returncode,0,result.stderr.decode('utf-8',errors='replace'))
        self.assertEqual(json.loads(result.stdout)['status'],'repacked')
        self.assertEqual(self.output.read_bytes(),bundle(self.new_base,self.data))
        self.assert_inputs_preserved()

    def test_footer_corruption_bounds_hash_and_truncation_fail_before_output_creation(self):
        corrupt=bytearray(self.original_bytes);corrupt[-relink.FOOTER.size]=0
        bad=[bytes(corrupt),self.original_bytes[:-10],
             bundle(self.old_base,self.data,offset=4095),bundle(self.old_base,self.data,offset=4097),
             bundle(self.old_base,self.data,count=(1<<64)-1),bundle(self.old_base,self.data,count=relink.MAX_PAYLOAD+1),
             bundle(self.old_base,self.data,digest=bytes(32))]
        for raw in bad:
            with self.subTest(kind=hashlib.sha256(raw).hexdigest()):
                self.original.write_bytes(raw)
                with self.assertRaises(ValueError):relink.repack(self.original,self.base,self.output)
                self.assertFalse(self.output.exists())
                self.assertEqual(self.original.read_bytes(),raw)
        self.original.write_bytes(self.original_bytes);self.assert_inputs_preserved()

    def test_payload_header_is_checked_even_with_a_matching_hash(self):
        for header in (relink.HEADER.pack(b'OTHER001',1,1,1),relink.HEADER.pack(b'MONADOP1',2,1,1),
                       relink.HEADER.pack(b'MONADOP1',1,0,1),relink.HEADER.pack(b'MONADOP1',1,100001,1),
                       relink.HEADER.pack(b'MONADOP1',1,1,4*1024*1024*1024+1)):
            with self.subTest(header=header):
                self.original.write_bytes(bundle(self.old_base,header+b'authored body'))
                with self.assertRaises(ValueError):relink.repack(self.original,self.base,self.output)
                self.assertFalse(self.output.exists())

    def test_invalid_foreign_dll_console_or_already_bundled_base_is_rejected(self):
        controls=[b'Authored non-PE input',self.original_bytes]
        for offset,value in ((132,0x14c),(150,0x2022),(220,3),(148,2)):
            raw=bytearray(self.new_base);struct.pack_into('<H',raw,offset,value);controls.append(bytes(raw))
        for raw in controls:
            with self.subTest(kind=hashlib.sha256(raw).hexdigest()):
                self.base.write_bytes(raw)
                with self.assertRaises(ValueError):relink.repack(self.original,self.base,self.output)
                self.assertFalse(self.output.exists())
                self.assertEqual(self.base.read_bytes(),raw)

    def test_in_place_or_existing_output_preserves_unknown_original_bytes(self):
        for output in (self.original,self.base):
            with self.subTest(output=output),self.assertRaises(ValueError):
                relink.repack(self.original,self.base,output)
        with self.assertRaises(ValueError):relink.repack(self.original,self.original,self.output)
        self.output.write_bytes(b'Authored human-owned unknown output; preserve')
        with self.assertRaises(ValueError):relink.repack(self.original,self.base,self.output)
        self.assertEqual(self.output.read_bytes(),b'Authored human-owned unknown output; preserve')
        self.assert_inputs_preserved()

    def test_hardlinked_inputs_and_linked_parent_are_refused(self):
        alias=self.root/'input alias.exe';os.link(self.original,alias)
        with self.assertRaises(ValueError):relink.repack(self.original,self.base,self.output)
        alias.unlink()
        outside=self.root/'authored outside';outside.mkdir();link=self.root/'linked directory'
        if os.name=='nt':
            import _winapi
            _winapi.CreateJunction(str(outside),str(link))
        else:
            link.symlink_to(outside,target_is_directory=True)
        try:
            with self.assertRaises(ValueError):relink.repack(self.original,self.base,link/'modified.exe')
            self.assertEqual(list(outside.iterdir()),[])
        finally:
            if os.name=='nt':link.rmdir()
            else:link.unlink()
        self.assert_inputs_preserved()

    def test_symbolic_input_and_output_links_are_refused_when_supported(self):
        link=self.root/'authored symbolic.exe'
        try:link.symlink_to(self.original)
        except OSError:self.skipTest('Host cannot create file symlinks; native parent junction test remains required')
        with self.assertRaises(ValueError):relink.repack(link,self.base,self.output)
        with self.assertRaises(ValueError):relink.repack(self.original,self.base,link)
        self.assertEqual(self.original.read_bytes(),self.original_bytes)

    def test_changed_base_during_streaming_cannot_report_success_and_preserves_original(self):
        original_copy=relink._copy
        def changed(source,offset,size,destination,complete):
            raw=bytearray(self.new_base);raw[2048]=1;self.base.write_bytes(raw)
            return original_copy(source,offset,size,destination,complete)
        with mock.patch.object(relink,'_copy',side_effect=changed):
            with self.assertRaisesRegex(ValueError,'frontend changed'):relink.repack(self.original,self.base,self.output)
        self.assertEqual(self.original.read_bytes(),self.original_bytes)
        self.assertTrue(self.output.exists())
        self.assertFalse(self.output.read_bytes().endswith(relink.FOOTER.pack(relink.MAGIC,len(self.new_base),len(self.data),hashlib.sha256(self.data).digest())))


if __name__=='__main__':
    unittest.main()
