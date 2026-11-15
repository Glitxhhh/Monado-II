"""Decode authored synthetic media with the actual old-baseline source libraries."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
PORT=ROOT/'Monado_Linux'


@unittest.skipUnless(sys.platform=='linux', 'native Linux source libraries')
class PortableMedia(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        marker=PORT/'.deps/portable/active-build.json'
        if not marker.is_file() or not shutil.which('ffmpeg'):
            raise unittest.SkipTest('explicit portable source build and synthetic media encoder required')
        selected=json.loads(marker.read_text()); receipt=json.loads(Path(selected['receipt']).read_text())
        if receipt['status']!='passed':raise AssertionError('portable dependency build failed')
        cls.prefix=Path(selected['receipt']).parent/'prefix'
        cls.temp=tempfile.TemporaryDirectory(prefix='monado-portable-media-');cls.addClassCleanup(cls.temp.cleanup)
        cls.out=Path(cls.temp.name);cls.binary=cls.out/'codec-fixture'
        cc=os.environ.get('MONADO_TEST_CC') or str(PORT/'.deps/bin/clang-cc')
        r=subprocess.run([cc,'-O1','-UNDEBUG','-target','x86_64-linux-gnu.2.39','-I'+str(cls.prefix/'include'),
            ROOT/'tests/native/test_portable_media.c','-L'+str(cls.prefix/'lib'),'-Wl,-rpath,'+str(cls.prefix/'lib'),
            '-lavformat','-lavcodec','-lavutil','-lswresample','-lswscale','-lm','-o',cls.binary],capture_output=True,text=True,timeout=120)
        if r.returncode:raise AssertionError(r.stderr)

    def test_vp9_vorbis_six_channels(self):self.decode('libvpx-vp9','libvorbis','webm',6)
    def test_vp8_opus(self):self.decode('libvpx','libopus','webm',2)
    def test_h264_aac(self):self.decode('libx264','aac','mp4',2)

    def decode(self,video,audio,extension,channels):
        clip=self.out/(video+'.'+extension)
        # Pixel/audio content comes entirely from FFmpeg's authored generators.
        command=['/usr/bin/ffmpeg','-nostdin','-hide_banner','-loglevel','error','-f','lavfi','-i','testsrc2=size=32x32:rate=30',
            '-f','lavfi','-i','sine=frequency=440:sample_rate=48000','-t','0.5','-c:v',video,'-threads','2','-c:a',audio,'-ac',str(channels),str(clip)]
        r=subprocess.run(command,capture_output=True,text=True,timeout=60)
        self.assertEqual(r.returncode,0,r.stderr)
        r=subprocess.run([self.binary,clip],capture_output=True,text=True,timeout=20)
        self.assertEqual(r.returncode,0,r.stderr)
        sysroots=sorted((PORT/'.deps/portable/sysroots').glob('*/.monado-portable.json'))
        self.assertTrue(sysroots,'pinned GNU 2.39 development root is required')
        native=sysroots[-1].parent/'usr/lib/x86_64-linux-gnu'
        # Execute only this authored codec fixture with the actual pinned old
        # loader/libc, not a game or graphical process. A hidden new symbol need
        # would fail here even when the development host has a newer glibc.
        r=subprocess.run([native/'ld-linux-x86-64.so.2','--library-path',str(self.prefix/'lib')+':'+str(native),self.binary,clip],
                         capture_output=True,text=True,timeout=20)
        self.assertEqual(r.returncode,0,r.stderr)
