"""[XCDE] LINUX-015: deterministic launcher assets, no host image/font decoder."""
import importlib.util
from pathlib import Path
import re
import struct
import tempfile
import unittest
from unittest import mock
import zlib


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('theme_resource', ROOT / 'tools/launcher/theme_resource.py')
resource = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(resource)


def png(width=1, height=1):
    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))
    return (b'\x89PNG\r\n\x1a\n' +
            chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(b'\0\x80\x90\xa0')) + chunk(b'IEND', b''))


class LauncherResources(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='monado-theme-resources-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.assets = self.root / 'assets'
        self.assets.mkdir()
        self.inputs = {}
        for entries in resource.GROUPS.values():
            for name, symbol in entries:
                path = self.assets / name
                path.parent.mkdir(parents=True, exist_ok=True)
                data = png() if name.endswith('.png') else b'\x00\x01\x00\x00' + symbol.encode() + bytes(range(256))
                path.write_bytes(data)
                self.inputs[name] = data
        license_path = self.assets / resource.FONT_LICENSE
        license_path.write_text('Synthetic font licence retained exactly.\n')
        self.inputs[resource.FONT_LICENSE] = license_path.read_bytes()

    def test_independent_output_directories_have_identical_bytes_and_manifests(self):
        first = resource.generate(self.assets, self.root / 'first')
        second = resource.generate(self.assets, self.root / 'second')
        self.assertEqual(first, second)
        self.assertEqual(set(first['assets']), set(self.inputs))
        for name, item in first['generated'].items():
            data = (self.root / 'first' / name).read_bytes()
            self.assertEqual(data, (self.root / 'second' / name).read_bytes())
            self.assertEqual(len(data), item['bytes'])
            self.assertEqual(resource.digest(data), item['sha256'])
        self.assertEqual((self.root / 'first/DejaVu-font-license.txt').read_bytes(),
                         self.inputs[resource.FONT_LICENSE])

    def test_embedded_arrays_preserve_all_bytes_including_zero_and_high_values(self):
        output = self.root / 'includes'
        resource.generate(self.assets, output)
        for target, entries in resource.GROUPS.items():
            text = (output / target).read_text()
            for name, symbol in entries:
                body = re.search(r'\b' + symbol + r'\[\] = \{(.*?)\};', text, re.S).group(1)
                embedded = bytes(int(value, 16) for value in re.findall(r'0x([0-9a-f]{2})', body))
                self.assertEqual(embedded, self.inputs[name])

    def test_invalid_font_refuses_every_generated_output(self):
        (self.assets / 'fonts/DejaVuSerif-BoldItalic.ttf').write_bytes(b'invalid sfnt')
        output = self.root / 'out'
        with self.assertRaisesRegex(ValueError, 'sfnt signature'):
            resource.generate(self.assets, output)
        self.assertFalse(output.exists())

    def test_missing_licence_refuses_every_generated_output(self):
        (self.assets / resource.FONT_LICENSE).unlink()
        with self.assertRaisesRegex(ValueError, 'ordinary file'):
            resource.generate(self.assets, self.root / 'out')
        self.assertFalse((self.root / 'out').exists())

    def test_unbounded_png_dimensions_refused(self):
        (self.assets / 'gaur_plain.png').write_bytes(png(8192, 8192))
        with self.assertRaisesRegex(ValueError, 'dimensions'):
            resource.generate(self.assets, self.root / 'out')

    def test_wrong_artwork_format_refused(self):
        (self.assets / 'gaur_plain.png').write_bytes(b'BMthis is not a png')
        with self.assertRaisesRegex(ValueError, 'PNG IHDR'):
            resource.generate(self.assets, self.root / 'out')

    def test_symlink_asset_refused(self):
        path = self.assets / 'gaur_plain.png'
        target = self.root / 'elsewhere.png'
        path.replace(target)
        path.symlink_to(target)
        with self.assertRaisesRegex(ValueError, 'ordinary file'):
            resource.generate(self.assets, self.root / 'out')

    def test_existing_output_is_not_overwritten(self):
        output = self.root / 'out'
        output.mkdir()
        protected = output / 'launcher_art.inc'
        protected.write_text('preserved previous artifact\n')
        with self.assertRaises(FileExistsError):
            resource.generate(self.assets, output)
        self.assertEqual(protected.read_text(), 'preserved previous artifact\n')

    def test_asset_size_cap_checked_before_read(self):
        with mock.patch.object(resource, 'MAX_ASSET', 8):
            with self.assertRaisesRegex(ValueError, 'asset size'):
                resource.generate(self.assets, self.root / 'out')


if __name__ == '__main__':
    unittest.main()
