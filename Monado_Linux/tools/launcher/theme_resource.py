#!/usr/bin/env python3
"""[XCDE] Monado II: embed licensed UI fonts and reviewed launcher artwork.

No game data is consumed. Output is build-local C data, not tracked source;
the returned manifest binds both canonical asset bytes and generated includes.
Only Python's standard library is needed and no runtime asset path is added.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


GROUPS = {
    'launcher_art.inc': [('gaur_plain.png', 'monado_art_png')],
    'launcher_fonts.inc': [
        ('fonts/DejaVuSans.ttf', 'monado_body_ttf'),
        ('fonts/DejaVuSerif-Bold.ttf', 'monado_heading_ttf'),
        ('fonts/DejaVuSerif-BoldItalic.ttf', 'monado_brand_ttf')],
}
MAX_ASSET = 8 * 1024 * 1024
FONT_LICENSE = 'fonts/LICENSE-DejaVu.txt'


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_asset(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError('launcher asset must be an ordinary file: ' + str(path))
    if not 0 < path.stat().st_size <= MAX_ASSET:
        raise ValueError('launcher asset size outside 1..8 MiB: ' + str(path))
    data = path.read_bytes()
    if not 0 < len(data) <= MAX_ASSET:
        raise ValueError('launcher asset changed size while reading: ' + str(path))
    if path.suffix == '.png':
        if (len(data) < 33 or data[:8] != b'\x89PNG\r\n\x1a\n' or
                data[8:16] != b'\x00\x00\x00\rIHDR'):
            raise ValueError('launcher artwork needs a PNG IHDR: ' + str(path))
        width, height = struct.unpack('>II', data[16:24])
        if not (0 < width <= 8192 and 0 < height <= 8192 and width * height <= 16777216):
            raise ValueError('launcher artwork dimensions are unreasonable: ' + str(path))
    elif path.suffix == '.ttf' and data[:4] not in (b'\x00\x01\x00\x00', b'OTTO', b'ttcf'):
        raise ValueError('launcher font needs an sfnt signature: ' + str(path))
    return data


def include_bytes(name, data):
    lines = ['static const unsigned char ' + name + '[] = {']
    for offset in range(0, len(data), 24):
        lines.append('    ' + ','.join('0x%02x' % value for value in data[offset:offset + 24]) + ',')
    return '\n'.join([*lines, '};', ''])


def generate(assets, output):
    # Validate every canonical input before publishing any generated include.
    sources, includes = {}, {}
    for target, entries in GROUPS.items():
        # The public source kit includes the exact reviewed generated artwork.
        # Font-only component fixtures can still omit the image.
        if target == 'launcher_art.inc':
            artwork = assets / entries[0][0]
            if not artwork.exists() and not artwork.is_symlink():
                continue
        text = '// [XCDE] LINUX-015: generated launcher resources; do not edit.\n#pragma once\n'
        for source, symbol in entries:
            data = read_asset(assets / source)
            sources[source] = {'sha256': digest(data), 'bytes': len(data), 'symbol': symbol}
            text += include_bytes(symbol, data)
        includes[target] = text.encode('ascii')
    # Keep the complete redistribution notice with every build's resources.
    # No system font paths or downloads are required by the shipped executable.
    licence = read_asset(assets / FONT_LICENSE)
    sources[FONT_LICENSE] = {'sha256': digest(licence), 'bytes': len(licence), 'role': 'licence'}
    includes['DejaVu-font-license.txt'] = licence
    output.mkdir(parents=False, exist_ok=False)
    generated = {}
    for name, data in includes.items():
        path = output / name
        with path.open('xb') as stream:
            stream.write(data)
        generated[name] = {'sha256': digest(data), 'bytes': len(data)}
    return {'schema': 1, 'scope': 'launcher presentation resources only',
            'assets': sources, 'generated': generated,
            'features': {'fonts': True, 'artwork': 'launcher_art.inc' in generated}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        manifest = generate(args.assets, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + '\n')
    print(json.dumps(manifest, sort_keys=True))


if __name__ == '__main__':
    main()
