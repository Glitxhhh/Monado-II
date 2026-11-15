"""Actual private media libraries on authored CPU-only synthetic inputs."""
from pathlib import Path
import importlib.util
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

PORT = Path(__file__).resolve().parents[1] / 'Monado_Linux'


@unittest.skipUnless(sys.platform == 'linux' and PORT.is_dir(), 'native Linux media runtime')
class LinuxMediaRuntime(unittest.TestCase):
    def test_private_vp9_h264_opus_scaling_and_resampling(self):
        compiler = os.environ.get('MONADO_TEST_CC')
        local = PORT / '.deps/bin/clang-cc'
        if not compiler:
            compiler = str(local) if local.is_file() else shutil.which('clang')
        if not compiler:
            self.skipTest('Clang is required for the native media fixture')
        wrapper = PORT / '.deps/bin/pkg-config-graphics'
        active = PORT / '.deps/graphics/active-sysroot.txt'
        if not wrapper.is_file() or not active.is_file():
            self.skipTest('The pinned local graphics/media dependencies are not installed')
        spec = importlib.util.spec_from_file_location('media_runtime_check', PORT / 'scripts/linux_runtime_check.py')
        runtime = importlib.util.module_from_spec(spec); spec.loader.exec_module(runtime)
        sysroot = Path(active.read_text().strip())
        policy, _ = runtime.load_policy(PORT, sysroot)
        self.assertIsNotNone(policy, 'The media fixture requires the closed runtime policy')
        environment = runtime.clean_environment(runtime.library_path(sysroot, policy))
        packages = ['libavcodec', 'libavformat', 'libavutil', 'libswscale', 'libswresample', 'opus']
        result = subprocess.run([str(wrapper), '--cflags', '--libs', *packages],
                                env=runtime.clean_environment(), capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        flags = shlex.split(result.stdout)
        with tempfile.TemporaryDirectory(prefix='monado-media-fixture-') as temporary:
            directory = Path(temporary); binary = directory / 'media-fixture'
            compile_environment = runtime.clean_environment()
            compile_environment.update(TMPDIR=temporary,
                                       ZIG_GLOBAL_CACHE_DIR=str(directory / 'compiler-cache'))
            command = [compiler, '-target', 'x86_64-linux-gnu.2.39', '-std=gnu11', '-O1', '-UNDEBUG', '-Wall', '-Wextra', '-Werror',
                       '-ffp-contract=off', '-fno-strict-aliasing',
                       str(PORT / 'tests/native/test_linux_media_runtime.c'), *flags,
                       '-Xlinker', '-rpath', '-Xlinker', environment['LD_LIBRARY_PATH'], '-o', str(binary)]
            if os.environ.get('MONADO_TEST_UBSAN') == '1':
                command[1:1] = ['-fsanitize=undefined', '-fno-sanitize-recover=all']
            result = subprocess.run(command, env=compile_environment,
                                    capture_output=True, text=True, timeout=120)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            runtime.check_artifacts([binary], sysroot, policy)
            # The private libraries serve the game's decode path. Create our
            # own encoded gradient clips with the host's independent encoder,
            # so removing unused external runtime encoders cannot fake a pass.
            host_encoder = Path('/usr/bin/ffmpeg')
            self.assertTrue(host_encoder.is_file(), 'Host ffmpeg is required to author synthetic encoded test clips')
            raw = bytearray()
            for frame in range(3):
                raw.extend(32+x*4+y*2+frame*5 for y in range(32) for x in range(32))
                raw.extend(bytes([128])*(16*16*2))
            clips = [directory / 'synthetic-vp9.webm', directory / 'synthetic-h264.mkv']
            for clip, codec, options in ((clips[0], 'libvpx-vp9', ['-deadline','realtime','-cpu-used','8','-lag-in-frames','0']),
                                         (clips[1], 'libx264', ['-preset','ultrafast','-tune','zerolatency','-bf','0'])):
                encoded = subprocess.run([str(host_encoder), '-nostdin', '-hide_banner', '-loglevel', 'error',
                    '-f', 'rawvideo', '-pixel_format', 'yuv420p', '-video_size', '32x32', '-framerate', '30',
                    '-i', 'pipe:0', '-frames:v', '3', '-an', '-c:v', codec, '-threads', '1', '-g', '3',
                    *options, str(clip)], input=bytes(raw), env=runtime.clean_environment(), capture_output=True, timeout=60)
                self.assertEqual(encoded.returncode, 0, encoded.stderr.decode(errors='replace'))
            result = subprocess.run([str(binary), *map(str, clips)], env=environment,
                                    capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('native media runtime: PASS vp9=3 h264=3 opus=960 resampled=882', result.stdout)
            # A broken encoded input must not report the positive fixture pass.
            broken = directory / 'bad-input.mkv'; broken.write_bytes(b'authored invalid media')
            rejected = subprocess.run([str(binary), str(clips[0]), str(broken)], env=environment,
                                      capture_output=True, text=True, timeout=60)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertNotIn('native media runtime: PASS', rejected.stdout)
            broken.unlink()
            files = {p.name for p in directory.iterdir()}
            self.assertEqual(files - {'compiler-cache'},
                             {'media-fixture', 'synthetic-vp9.webm', 'synthetic-h264.mkv'})
            self.assertTrue(all((directory / name).stat().st_size > 100
                                for name in ('synthetic-vp9.webm', 'synthetic-h264.mkv')))


if __name__ == '__main__':
    unittest.main()
