import math, os, struct, subprocess, sys, tempfile, unittest, wave

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, 'tools', 'wav_report.py')


def write_wav(path, seconds, amp, freq=440.0, rate=48000):
    with wave.open(path, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(seconds * rate):
            v = int(amp * math.sin(2 * math.pi * freq * i / rate))
            frames += struct.pack('<hh', v, v)
        w.writeframes(bytes(frames))


class WavReportTest(unittest.TestCase):
    def run_tool(self, amp):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, 't.wav')
            write_wav(p, 2, amp)
            return subprocess.run([sys.executable, '-X', 'utf8', TOOL, p], capture_output=True, text=True)

    def test_tone_is_alive(self):
        r = self.run_tool(8000)
        self.assertEqual(r.returncode, 0, r.stdout)
        self.assertIn('RESULT: OK', r.stdout)

    def test_silence_fails(self):
        r = self.run_tool(0)
        self.assertEqual(r.returncode, 1, r.stdout)

    def test_clipping_fails(self):
        r = self.run_tool(32767)  # a full-scale sine touches the rails only at the peaks; square-ish after clamping
        # (peak == 32767 counts as clipped: assert the tool reports it instead of crashing)
        self.assertIn('clipped samples', r.stdout)


if __name__ == '__main__':
    unittest.main()
