"""Source-kit regression: compiled relative headers must actually be shipped."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SourceHeaderClosure(unittest.TestCase):
    def test_relative_authored_includes_exist_in_each_shipped_port(self):
        missing = []
        checked = 0
        for name in ('Monado_Linux', 'Monado_Windows'):
            port = ROOT / name
            if not port.is_dir():  # A platform source kit includes only its port.
                continue
            for source in port.rglob('*'):
                if source.suffix.lower() not in ('.c', '.cpp', '.h', '.hpp'):
                    continue
                text = source.read_text(encoding='utf-8')
                for header in re.findall(r'^\s*#\s*include\s*"([.][^\"]+)"', text, re.M):
                    checked += 1
                    if not (source.parent / header).is_file():
                        missing.append(str(source.relative_to(ROOT)) + ': ' + header)
        self.assertGreater(checked, 0, 'no shipped source was inspected')
        self.assertEqual(missing, [], '\n'.join(missing))
