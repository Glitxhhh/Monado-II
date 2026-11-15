#!/usr/bin/env python3
"""[XCDE] LINUX-099: build the authored local Vulkan observer, no driver code run.

Called by the native build owner after compiler/flags have been checked. The
single resource is receipted like other local companions. Source kits contain
only the authored C/header/helper; no compiled module or driver is distributed.
"""
import hashlib
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
RESOURCE = 'libmonado-present-observer.so'
EXPORTS = sorted(('monado_present_GetDeviceProcAddr', 'monado_present_GetInstanceProcAddr',
                  'monado_present_observer_snapshot', 'vkNegotiateLoaderLayerInterfaceVersion'))


def digest(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def build(builder):
    """Use the owning native builder's audited compiler/logs/output transaction."""
    source = ROOT / 'runtime/nvn/vulkan/present_observer_linux.c'
    header = source.with_suffix('.h')
    before = {str(p.relative_to(ROOT)): digest(p) for p in (source, header)}
    partial = builder.output / (RESOURCE + '.partial')
    final = builder.output / RESOURCE
    if partial.exists() or final.exists() or partial.is_symlink() or final.is_symlink():
        raise ValueError('observer module output must be fresh')
    exports = builder.output / 'inc/present-observer-exports.map'
    exports.write_text('{ global: ' + '; '.join(EXPORTS) + '; local: *; };\n')
    row, _ = builder.run('present-observer-module', [builder.cc, *builder.flags,
        '-std=c11', '-shared', '-fPIC', str(source), '-Xlinker', '--version-script',
        '-Xlinker', str(exports), '-Wl,--no-undefined', '-o', str(partial)])
    if row['exit']:
        raise RuntimeError('authored presentation observer module build failed')
    row, symbols = builder.run('present-observer-exports', ['/usr/bin/nm', '-D', '--defined-only', str(partial)])
    if row['exit'] or sorted(line.split()[-1] for line in symbols.splitlines() if line.strip()) != EXPORTS:
        raise RuntimeError('presentation observer exports differ from its authored ABI')
    row, dynamic = builder.run('present-observer-dynamic', ['/usr/bin/readelf', '-dW', str(partial)])
    needed = re.findall(r'\(NEEDED\).*\[(.*?)\]', dynamic)
    if row['exit'] or any(name not in ('libc.so.6', 'libpthread.so.0', 'librt.so.1') for name in needed):
        raise RuntimeError('presentation observer has unexpected runtime dependencies')
    if before != {str(p.relative_to(ROOT)): digest(p) for p in (source, header)}:
        raise RuntimeError('presentation observer source changed while building')
    partial.replace(final)
    builder.receipt.setdefault('runtime_resources', {})[RESOURCE] = {
        'path': str(final), 'sha256': digest(final), 'bytes': final.stat().st_size,
        'scope': 'authored MIT downstream Vulkan presentation observer; no vendor code'}
    builder.receipt['present_observer'] = {'abi': 1, 'sources': before, 'exported_symbols': EXPORTS,
        'needed': needed, 'library_executed': False, 'generated_classification': False,
        'counter': 'accepted downstream requests, not scanout'}
    builder.save()
    return final
