#!/usr/bin/env python3
"""Native guest FMA edge contracts vs Unicorn; authored operands, no game data."""
from datetime import datetime, timezone
import hashlib
import itertools
import json
import os
import platform
from pathlib import Path
import struct
import subprocess
import sys
import uuid

PORT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PORT / 'tools/lifter'))
from test_simd import Emu, CODE


def main():
    if os.name != 'nt':
        raise RuntimeError('Native Windows required')
    output = PORT / 'runs' / ('fma-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ') + '-' + uuid.uuid4().hex[:8])
    output.mkdir(parents=True)
    sources = [Path(__file__), PORT / 'tests/native/test_windows_fma.c', PORT / 'tests/native/test_windows_fma_dispatch.c',
               PORT / 'runtime/platform/windows_fma.c', PORT / 'third_party/musl_math/PROVENANCE.json',
               PORT / 'runtime/platform/guest_fp_windows.h', PORT / 'runtime/cpu.h', PORT / 'runtime/simd.h',
               PORT / 'runtime/simd_fp.h', PORT / 'tools/lifter/test_simd.py']
    def hashes():
        return {str(path.relative_to(PORT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in sources}
    import winreg
    with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE,r'HARDWARE\DESCRIPTION\System\CentralProcessor\0') as key:
        cpu_name=winreg.QueryValueEx(key,'ProcessorNameString')[0]
    receipt = {'status': 'failed', 'sources_before': hashes(), 'commands': [], 'python': sys.version,
               'host':platform.platform(),'cpu':cpu_name.strip(),'scope':'native FMA helpers, no game FPS or rendering qualification'}
    env = os.environ.copy()
    env.update(ZIG_GLOBAL_CACHE_DIR=str(PORT / '.deps/zig-global-cache'), ZIG_LOCAL_CACHE_DIR=str(output / 'zig-cache'))
    def run(name, argv):
        argv = [str(arg) for arg in argv]
        result = subprocess.run(argv, cwd=output, env=env, capture_output=True, text=True, timeout=120)
        receipt['commands'].append({'argv': argv, 'returncode': result.returncode})
        (output / (name + '.log')).write_text(result.stdout + result.stderr)
        if result.returncode:
            raise RuntimeError(name + ' failed:\n' + result.stdout + result.stderr)
        print(result.stdout, end='')
        return result.stdout
    try:
        if run('compiler', [sys.executable, '-m', 'ziglang', 'version']).strip() != '0.14.1':
            raise RuntimeError('Pinned Zig 0.14.1 required')
        emu = Emu()
        edge64 = [0,1,0x8000000000000000,0x8000000000000001,0xfffffffffffff,0x10000000000000,
                  0x8010000000000000,0x3ff0000000000000,0xbff0000000000000,0x3ff0000000000001,
                  0x7fefffffffffffff,0xffefffffffffffff,0x7ff0000000000000,0xfff0000000000000,
                  0x7ff8000000001234,0x7ff0000000000042]
        edge32 = [0,1,0x80000000,0x80000001,0x7fffff,0x800000,0x80800000,0x3f800000,
                  0xbf800000,0x3f800001,0x7f7fffff,0xff7fffff,0x7f800000,0xff800000,0x7fc01234,0x7f800042]
        cases = output / 'authored-cases.bin'; count = 0
        with cases.open('xb') as stream:
            for precision, edges, word in ((64, edge64, 0x1f420c20), (32, edge32, 0x1f020c20)):
                emu.mu.mem_write(CODE, struct.pack('<I', word)); emu.mu.ctl_remove_cache(CODE, CODE+4)
                # Full factor-pair cross product, eight varied addends, every
                # RMode/DN/FZ combination. Upper V bits must clear as A64 defines.
                addends = [edges[i] for i in (0,2,1,3,7,8,14,15)]
                for rounding, dn, fz in itertools.product(range(4), range(2), range(2)):
                    fpcr = rounding << 22 | dn << 25 | fz << 24
                    for n, m, a in itertools.product(edges, edges, addends):
                        vectors = [0] * 32; vectors[1:4] = [n,m,a]
                        expected = emu.run(CODE, fpcr, [0]*31, 0, 0, vectors)[3][0]
                        stream.write(struct.pack('<6Q', precision,fpcr,n,m,a,expected)); count += 1
        receipt.update(cases=count, case_sha256=hashlib.sha256(cases.read_bytes()).hexdigest(),
                       fpcr_modes=16, reference='Unicorn Cortex-A57, exact FMA register bits; no FPSR exception/QC claim')
        binary = output / 'fma.exe'
        flags=['-std=gnu11','-O1','-UNDEBUG','-march=x86_64_v2',
                       '-ffp-contract=off','-fno-fast-math','-fno-strict-aliasing','-Wall','-Wextra','-Werror',
                       '-fno-builtin-fma','-fno-builtin-fmaf','-DMONADO_FMA_TEST']
        receipt['flags']=flags
        run('compile', [sys.executable,'-B','-m','ziglang','cc',*flags,
                       PORT/'tests/native/test_windows_fma.c',PORT/'runtime/platform/windows_fma.c','-o',binary])
        run('native-auto', [binary,cases,'auto'])
        run('native-software', [binary,cases,'software'])
        dispatch=output/'dispatch.exe'
        run('compile-dispatch',[sys.executable,'-B','-m','ziglang','cc',*flags,
            PORT/'tests/native/test_windows_fma_dispatch.c',PORT/'runtime/platform/windows_fma.c','-o',dispatch])
        receipt['dispatch_and_benchmark']=run('native-dispatch',[dispatch])
        receipt['sources_after'] = hashes()
        if receipt['sources_before'] != receipt['sources_after']:
            raise RuntimeError('Production source closure changed during qualification')
        receipt['status'] = 'passed'
    finally:
        (output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
        print('Native FMA evidence: '+str(output))


if __name__ == '__main__':
    main()
