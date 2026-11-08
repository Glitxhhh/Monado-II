"""Source-kit regression: compiled relative headers must actually be shipped."""
import json
from pathlib import Path
import re
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def relative_header_closure(root, shipped):
    """[XCDE] LINUX-029: inspect declared source, never local SDK/build/input data."""
    root = root.resolve()
    missing, checked = [], 0
    for name in sorted(shipped):
        relative = Path(name)
        if (not relative.parts or relative.parts[0] not in ('Monado_Linux', 'Monado_Windows') or
                relative.suffix.lower() not in ('.c', '.cpp', '.h', '.hpp')):
            continue
        source = root / relative
        text = source.read_text(encoding='utf-8')
        for header in re.findall(r'^\s*#\s*include\s*"([.][^\"]+)"', text, re.M):
            checked += 1
            target = (source.parent / header).resolve()
            try:
                target_name = target.relative_to(root).as_posix()
            except ValueError:
                target_name = None
            if target_name not in shipped or not target.is_file():
                missing.append(name + ': ' + header)
    return missing, checked


class SourceHeaderClosure(unittest.TestCase):
    def test_relative_authored_includes_exist_in_each_shipped_port(self):
        shipped = json.loads((ROOT / 'publication-manifest.json').read_text())['files']
        missing, checked = relative_header_closure(ROOT, shipped)
        self.assertGreater(checked, 0, 'no shipped source was inspected')
        self.assertEqual(missing, [], '\n'.join(missing))

    def test_private_dependencies_and_generated_files_are_not_scanned(self):
        with tempfile.TemporaryDirectory(prefix='monado-header-closure-') as temporary:
            root = Path(temporary).resolve()
            (root / 'Monado_Linux/runtime').mkdir(parents=True)
            (root / 'Monado_Linux/runtime/authored.c').write_text('#include "./authored.h"\n')
            (root / 'Monado_Linux/runtime/authored.h').write_text('/* Authored header. */\n')
            for directory in ('.deps', 'generated', 'build', 'game_files'):
                private = root / 'Monado_Linux' / directory / 'private.h'
                private.parent.mkdir(parents=True)
                private.write_bytes(b'\xff\xa9 Private synthetic bytes, not shipped.\n')
            names = {'Monado_Linux/runtime/authored.c', 'Monado_Linux/runtime/authored.h'}
            self.assertEqual(relative_header_closure(root, names), ([], 1))

    def test_existing_but_unshipped_relative_header_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='monado-header-missing-') as temporary:
            root = Path(temporary).resolve()
            (root / 'Monado_Linux/runtime').mkdir(parents=True)
            source = root / 'Monado_Linux/runtime/authored.c'
            source.write_text('#include "./not-shipped.h"\n')
            (source.parent / 'not-shipped.h').write_text('/* Authored private fixture. */\n')
            missing, checked = relative_header_closure(root, {'Monado_Linux/runtime/authored.c'})
            self.assertEqual(checked, 1)
            self.assertEqual(missing, ['Monado_Linux/runtime/authored.c: ./not-shipped.h'])
