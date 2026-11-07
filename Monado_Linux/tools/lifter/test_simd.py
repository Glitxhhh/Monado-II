"""Differential test of the portable FP/SIMD lowering (lifter --simd=c) against Unicorn (Cortex-A57).

No NSO needed: the FP/SIMD data-processing space (bits 27..25 = 111), the FP/SIMD load/store space and CRC32
are enumerated (every opcode bit combination, random register fields), each encoding's validity is compared
between Unicorn and the lifter, then a stratified sample of valid encodings is compiled (clang) and run with
random interesting operands (NaNs, infinities, denormals, +-0, saturation edges, random FPCR.DN/FZ/RMode/AHP)
against Unicorn: X0-X30, SP, NZCV, V0-V31 and (for stores) memory must match. FPSR is not compared (the
lowering doesn't model the cumulative exception bits or QC).

Usage: test_simd.py [--encodings N] [--cases K] [--seed S] [--jobs J] [--no-validity] [--filter HEX/MASK]
                    [--work FRESH_IGNORED_DIRECTORY]
Linux uses the local GNU Clang wrapper, x86-64-v2, -O1, -UNDEBUG and -ffp-contract=off.
Each run retains commands, synthetic inputs, outputs and source/tool hashes. No game input is used.
"""
import argparse
import concurrent.futures
import functools
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import shlex
import signal
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from collections import Counter, defaultdict

import unicorn as uc
from unicorn import arm64_const as A

from lifter import Lifter, Unsupported

print = functools.partial(print, flush=True)
# [XCDE] LINUX-002: repository-root paths, actual binary harness and fail-closed evidence.
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
BUF, BUF_SIZE = 0x7200000000, 0x20000
BASE_AREA = BUF + 0x8000
CODE, CODE_SIZE = 0x7210000000, 0x400000
IN_FMT = '<QQ32QQQ'
OUT_SIZE = 31 * 8 + 3 * 8 + 32 * 16
UC_INVALID = (uc.UC_ERR_INSN_INVALID, uc.UC_ERR_EXCEPTION)


class HarnessError(RuntimeError):
    pass


def hash_file(path):
    with open(path, 'rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def source_hashes():
    names = list(Path(ROOT, 'tools/lifter').glob('*.py'))
    names += list(Path(ROOT, 'runtime').glob('*.h'))
    names += [Path(ROOT, 'tools/tests/test_simd_harness.c'), Path(ROOT, 'requirements-linux.lock'),
              Path(ROOT, '.deps/bin/clang-cc')]
    return {str(p.relative_to(ROOT)): hash_file(p) for p in sorted(names)}


def fresh_work(requested):
    root = Path(ROOT)
    if requested:
        target = Path(requested).absolute()
        if target.resolve() != target or not any(target.is_relative_to(root / name) for name in ('build', 'runs')):
            raise HarnessError('work must be a fresh real directory under this Linux port build/ or runs/')
        target.mkdir(parents=True, exist_ok=False)
    else:
        parent = root / 'build'
        if parent.resolve() != parent:
            raise HarnessError('build directory cannot be a symlink')
        parent.mkdir(exist_ok=True)
        target = Path(tempfile.mkdtemp(prefix='test_simd-', dir=parent))
    (target / 'tmp').mkdir()
    return str(target)


def run_logged(work, name, argv, timeout=180):
    """Record every compiler/harness exit; a killed child cannot be a partial pass."""
    work = Path(work)
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'TMPDIR': str(work / 'tmp')}
    record = {'argv': list(map(str, argv)), 'cwd': ROOT, 'env': env, 'timeout_seconds': timeout}
    path = work / (name + '.command.json')
    start = time.monotonic()
    path.write_text(json.dumps(record, indent=2) + '\n')
    with (work / (name + '.stdout.log')).open('wb') as stdout, (work / (name + '.stderr.log')).open('wb') as stderr:
        try:
            proc = subprocess.Popen(record['argv'], cwd=ROOT, env=env, stdout=stdout, stderr=stderr,
                                    stdin=subprocess.DEVNULL, start_new_session=True)
            record['pid'] = proc.pid
            path.write_text(json.dumps(record, indent=2) + '\n')
            try:
                proc.wait(timeout=timeout)
            except BaseException:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
                raise
            record['exit'] = proc.returncode
        except (OSError, subprocess.TimeoutExpired) as error:
            record.update(exit=127, error=str(error))
        finally:
            record['seconds'] = time.monotonic() - start
            path.write_text(json.dumps(record, indent=2) + '\n')
    if record.get('exit') != 0:
        raise HarnessError(f'{name} failed; see {work / (name + ".stderr.log")}')
    return (work / (name + '.stdout.log')).read_text(errors='replace')


def validate_output(data, count):
    if count <= 0:
        raise HarnessError('no executable differential cases; cannot pass an empty sample')
    expected = count * OUT_SIZE
    if len(data) != expected:
        raise HarnessError(f'harness output length {len(data)} != expected {expected}; truncated or surplus records')

F32 = [0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, 0xffc00000, 0x7fa00001, 0xff812345, 0x7fd00abc,
       0x00000001, 0x80000001, 0x007fffff, 0x807fffff, 0x00400000, 0x00800000, 0x80800000, 0x00800001, 0x7f7fffff,
       0xff7fffff, 0x3f800000, 0xbf800000, 0x3f000000, 0xbf000000, 0x40000000, 0x40400000, 0x3fc00000, 0xbfc00000,
       0x40200000, 0x40100000, 0x4f000000, 0xcf000000, 0x4f800000, 0x5f000000, 0xdf000000, 0x5f800000, 0x4effffff,
       0x4f7fffff, 0x5effffff, 0x5f7fffff, 0x3effffff, 0x3f7fffff, 0x7effffff, 0x01000000, 0x3e800000, 0x7e800000,
       0x7e7fffff, 0x00200000, 0x001fffff, 0x00200001, 0x47800000, 0x477fe000, 0x477ff000, 0x38800000, 0x33800000,
       0x33000000, 0x387fc000, 0x3f7ff000, 0x46fffe00, 0x80000000 | 0x00200000, 0x7f000000, 0x7f7ffffe]
F64 = [0, 1 << 63, 0x7ff0000000000000, 0xfff0000000000000, 0x7ff8000000000000, 0xfff8000000000000,
       0x7ff4000000000001, 0xfff0000000012345, 0x7ffc00000abcdef0, 1, (1 << 63) | 1, 0x000fffffffffffff,
       0x0008000000000000, 0x0010000000000000, 0x8010000000000000, 0x7fefffffffffffff, 0xffefffffffffffff,
       0x3ff0000000000000, 0xbff0000000000000, 0x3fe0000000000000, 0x4000000000000000, 0x4008000000000000,
       0x3ff8000000000000, 0xbff8000000000000, 0x4004000000000000, 0x41e0000000000000, 0xc1e0000000000000,
       0x41f0000000000000, 0x43e0000000000000, 0xc3e0000000000000, 0x43f0000000000000, 0x41dfffffffc00000,
       0x41efffffffe00000, 0x43dfffffffffffff, 0x43efffffffffffff, 0x3fdfffffffffffff, 0x7fdfffffffffffff,
       0x7fd0000000000000, 0x0004000000000000, 0x0003ffffffffffff, 0x47efffffe0000000, 0x47efffff00000000,
       0x36a0000000000000, 0x3810000000000000, 0x380fffffffffffff, 0x47f0000000000000, 0x3e70000000000000,
       0x3f0ffc0000000000, 0x40effc0000000000, 0x40effe0000000000, 0x3e6ffffffff00000, 0x369fffffffffffff,
       0x3810000010000000, 0x381000000fffffff, 0x3e80000000000000]
F16 = [0, 0x8000, 0x7c00, 0xfc00, 0x7e00, 0xfe00, 0x7d00, 0x7e55, 0x0001, 0x8001, 0x03ff, 0x0400, 0x7bff, 0xfbff,
       0x3c00, 0xbc00, 0x3800, 0x4000, 0x3e00, 0x7fff, 0xffff]


def rand_lane(r, sz):
    """Random interesting integer lane of 8 << sz bits."""
    bits_ = 8 << sz
    k = r.random()
    if k < 0.35:
        return r.choice([0, 1, 2, 3, (1 << bits_) - 1, (1 << bits_) - 2, 1 << (bits_ - 1), (1 << (bits_ - 1)) - 1,
                         (1 << (bits_ - 1)) + 1, 0x80 % (1 << bits_), 0x7f, bits_ - 1, bits_, bits_ + 1,
                         (1 << bits_) - bits_, 1 << (bits_ // 2)])
    if k < 0.5:
        return r.getrandbits(r.randint(1, bits_))
    if k < 0.6:
        return ((1 << bits_) - r.getrandbits(r.randint(1, bits_ - 1))) & ((1 << bits_) - 1)
    return r.getrandbits(bits_)


def rand_f32(r):
    k = r.random()
    if k < 0.4:
        return r.choice(F32)
    if k < 0.8:
        e = r.choice([r.randint(110, 145), r.randint(0, 255), r.randint(120, 135)])
        return (r.getrandbits(1) << 31) | (e << 23) | (r.getrandbits(23) if r.random() < 0.7 else
                                                        r.getrandbits(4) << 19)
    return struct.unpack('<I', struct.pack('<f', r.choice([r.uniform(-4, 4), float(r.randint(-300, 300)) + 0.5,
                                                           float(r.randint(-5, 5)), r.uniform(-1e6, 1e6)])))[0]


def rand_f64(r):
    k = r.random()
    if k < 0.4:
        return r.choice(F64)
    if k < 0.8:
        e = r.choice([r.randint(1000, 1050), r.randint(0, 2047), r.randint(1020, 1030), r.randint(0, 40),
                      r.randint(2000, 2047), r.randint(860, 920)])
        return (r.getrandbits(1) << 63) | (e << 52) | (r.getrandbits(52) if r.random() < 0.7 else
                                                        r.getrandbits(6) << 46)
    return struct.unpack('<Q', struct.pack('<d', r.choice([r.uniform(-4, 4), float(r.randint(-300, 300)) + 0.5,
                                                           r.uniform(-1e12, 1e12)])))[0]


def rand_v(r):
    k = r.random()
    out = 0
    if k < 0.3:
        for i in range(4):
            out |= rand_f32(r) << (32 * i)
    elif k < 0.55:
        for i in range(2):
            out |= rand_f64(r) << (64 * i)
    elif k < 0.6:
        for i in range(8):
            out |= (r.choice(F16) if r.random() < 0.5 else r.getrandbits(16)) << (16 * i)
    elif k < 0.9:
        sz = r.randrange(4)
        for i in range(16 >> sz):
            out |= rand_lane(r, sz) << ((8 << sz) * i)
    else:
        out = r.getrandbits(128)
    if r.random() < 0.15:  # upper half zero, like most 64-bit results
        out &= (1 << 64) - 1
    return out


def rand_x(r):
    k = r.random()
    if k < 0.3:
        return r.choice([0, 1, 2, 0xFFFFFFFFFFFFFFFF, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFF,
                         0x7FFFFFFF, 0x80000000, 0x100000000, 0xFFFFFFFF80000000, 0x8000000000000001, 0xFFFFFF,
                         0x1000001, 0x20000000000001, 0xFFFFFFFFFF000001])
    if k < 0.45:
        return rand_f32(r) | (r.getrandbits(32) << 32 if r.random() < 0.3 else 0)
    if k < 0.6:
        return rand_f64(r)
    if k < 0.75:
        return r.getrandbits(r.randint(1, 64))
    if k < 0.8:
        return (-r.getrandbits(r.randint(1, 63))) & 0xFFFFFFFFFFFFFFFF
    return r.getrandbits(64)


def rand_fpcr(r):
    k = r.random()
    if k < 0.55:
        return 0
    v = 0
    if r.random() < 0.4:
        v |= 1 << 25  # DN
    if r.random() < 0.4:
        v |= 1 << 24  # FZ
    if r.random() < 0.4:
        v |= r.randrange(4) << 22  # RMode
    if r.random() < 0.15:
        v |= 1 << 26  # AHP
    return v


def is_ldst(w):
    return (w >> 25) & 1 == 0 and (w >> 27) & 1 == 1


def is_store(w):
    if not is_ldst(w):
        return False
    if (w & 0x3B000000) == 0x18000000:  # literal
        return False
    if (w & 0xBE000000) == 0x0C000000:  # structures: L = bit 22
        return not (w >> 22) & 1
    if (w >> 27) & 7 == 0b101:  # pair
        return not (w >> 22) & 1
    return not (w >> 22) & 1


def is_literal(w):
    return (w & 0x3F000000) == 0x1C000000


def class_key(w):
    """Opcode signature: register fields (Rd, Rn, Rm) cleared."""
    if is_ldst(w):
        return w & 0xFFE0FC00 if (w & 0xBE000000) == 0x0C000000 else w & 0xFFFFFC00
    return w & 0xFFE0FC00


def candidates(r):
    """Every opcode bit pattern of the FP/SIMD data-processing and load/store spaces, random registers."""
    out = []
    for p in range(1 << 19):
        hi, mid = p >> 15, p & 0x7FFF
        rr = r.getrandbits(10)
        out.append((hi << 28) | (0b111 << 25) | (mid << 10) | rr)  # data processing
        out.append((hi << 28) | (1 << 27) | (1 << 26) | (mid << 10) | rr)  # loads/stores
    for sf in (0, 1):
        for C in (0, 1):
            for sz in range(4):
                for _ in range(4):
                    out.append((sf << 31) | (0b0011010110 << 21) | (r.getrandbits(5) << 16) | (0b010 << 13) |
                               (C << 12) | (sz << 10) | r.getrandbits(10))
    return out


class Emu:
    def __init__(self):
        mu = uc.Uc(uc.UC_ARCH_ARM64, uc.UC_MODE_ARM)
        mu.ctl_set_cpu_model(A.UC_CPU_ARM64_A57)
        mu.mem_map(BUF, BUF_SIZE)
        mu.mem_map(CODE, CODE_SIZE)
        mu.reg_write(A.UC_ARM64_REG_CPACR_EL1, 0x300000)
        self.mu = mu
        self.xregs = [A.UC_ARM64_REG_X0 + i for i in range(29)] + [A.UC_ARM64_REG_X29, A.UC_ARM64_REG_X30]
        self.qregs = [A.UC_ARM64_REG_Q0 + i for i in range(32)]
        self.out_regs = self.xregs + [A.UC_ARM64_REG_SP, A.UC_ARM64_REG_NZCV] + self.qregs

    def run(self, pc, fpcr, x, sp, nzcv, v):
        mu = self.mu
        mu.reg_write_batch(list(zip(self.xregs, x[:31])) + list(zip(self.qregs, v)) +
                           [(A.UC_ARM64_REG_SP, sp), (A.UC_ARM64_REG_NZCV, nzcv), (A.UC_ARM64_REG_FPCR, fpcr)])
        mu.emu_start(pc, pc + 4, count=1)
        o = mu.reg_read_batch(self.out_regs)
        return o[:31], o[31], o[32] & 0xF0000000, o[33:]


def validity(lifter, ws, emu):
    """Map word -> (lifter ok, unicorn ok); words are placed at CODE + 4i."""
    mu = emu.mu
    res = {}
    step = CODE_SIZE // 4
    for base in range(0, len(ws), step):
        chunk = ws[base:base + step]
        mu.mem_write(CODE, struct.pack(f'<{len(chunk)}I', *chunk))
        mu.ctl_remove_cache(CODE, CODE + CODE_SIZE)
        state = list(zip(emu.xregs, [BASE_AREA] * 31)) + [(A.UC_ARM64_REG_SP, BASE_AREA)]
        for i, w in enumerate(chunk):
            pc = CODE + 4 * i
            try:
                lifter.lift(w, pc)
                lok = True
            except Unsupported:
                lok = False
            if i % 32 == 0:
                mu.reg_write_batch(state)
            try:
                mu.emu_start(pc, pc + 4, count=1)
                uok = True
            except uc.UcError as e:
                uok = e.errno not in UC_INVALID
            res[w] = (lok, uok)
    return res


def make_case(r, w):
    fpcr = rand_fpcr(r)
    v = [rand_v(r) for _ in range(32)]
    nzcv = r.getrandbits(4) << 28
    sp = BASE_AREA + r.randrange(0, 0x100, 16)
    if is_ldst(w):
        x = [BASE_AREA + r.randrange(0, 0x100) for _ in range(31)]
        rm = (w >> 16) & 31
        if rm < 31:
            reg_off = (w & 0x3B200C00) == 0x38200800  # register offset
            post_reg = (w & 0xBF800000) in (0x0C800000, 0x0D800000)
            if reg_off and rm == (w >> 5) & 31:
                # [XCDE] LINUX-002: aliased base/index still needs a mapped EA.
                # 32-bit extension sees BUF's zero low word; 64-bit extension
                # requires solving base + (base << shift) inside the buffer.
                scale = 4 if (w >> 23) & 1 else (w >> 30) & 3
                shift = scale if (w >> 12) & 1 else 0
                x[rm] = BASE_AREA // (1 + (1 << shift)) if (w >> 13) & 1 else BUF
            elif rm != (w >> 5) & 31 and (reg_off or post_reg):
                x[rm] = r.randrange(0, 0x100)
    else:
        x = [rand_x(r) for _ in range(31)]
    return fpcr, x, sp, nzcv, v


def compile_tests(work, bodies, jobs, cflags):
    if not bodies:
        raise HarnessError('no lifted instruction bodies')
    cc = shlex.split(os.environ.get('XCDE_CC', os.path.join(ROOT, '.deps/bin/clang-cc')))
    flags = ['-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-strict-aliasing']
    flags += shlex.split(cflags) + ['-Wno-unused-label', '-I',
             os.path.join(ROOT, 'runtime')]
    run_logged(work, 'compiler-version', cc + ['--version'])
    macros = run_logged(work, 'compiler-predefines', cc + flags + ['-dM', '-E', '-x', 'c', '/dev/null'])
    if any(line.startswith(('#define NDEBUG ', '#define __FAST_MATH__ ')) for line in macros.splitlines()):
        raise HarnessError('compiler disables assertions or enables fast math')
    n = max(1, min(jobs, len(bodies) // 200 + 1))
    per = (len(bodies) + n - 1) // n
    srcs = []
    for j in range(n):
        path = os.path.join(work, f'tests_{j}.c')
        with open(path, 'w') as f:
            f.write('#include "cpu.h"\n')
            for k in range(j * per, min(len(bodies), (j + 1) * per)):
                f.write(f'void t{k}(Cpu *restrict c) {{ {bodies[k]} }}\n')
        srcs.append(path)
    with open(os.path.join(work, 'table.c'), 'w') as f:
        f.write('#include "cpu.h"\n')
        f.write(''.join(f'void t{k}(Cpu *c);\n' for k in range(len(bodies))))
        f.write('GuestFn tests[] = {' + ','.join(f't{k}' for k in range(len(bodies))) + '};\n')
        f.write(f'int ntests = {len(bodies)};\n')
    srcs += [os.path.join(work, 'table.c'), os.path.join(ROOT, 'tools', 'tests', 'test_simd_harness.c')]
    objs = []
    for s in srcs:
        o = s[:-2] + '.o' if s.startswith(work) else os.path.join(work, 'harness.o')
        objs.append(o)
    def compile_one(pair):
        source, obj = pair
        return run_logged(work, 'compile-' + Path(obj).stem, cc + flags + ['-c', source, '-o', obj])
    with concurrent.futures.ThreadPoolExecutor(max_workers=min(6, jobs)) as pool:
        list(pool.map(compile_one, zip(srcs, objs)))
    exe = os.path.join(work, 'harness.exe' if os.name == 'nt' else 'harness')
    run_logged(work, 'link', cc + ['-march=x86_64_v2', '-o', exe] + objs + ['-lm'])
    return exe


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--encodings', type=int, default=20000)
    ap.add_argument('--cases', type=int, default=8)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--jobs', type=int, choices=range(1, 7), default=4)
    ap.add_argument('--no-validity', action='store_true', help='skip the full validity enumeration')
    ap.add_argument('--filter', help='only encodings with (w & MASK) == VAL, as VAL/MASK in hex')
    ap.add_argument('--show', type=int, default=40)
    ap.add_argument('--work', help='fresh ignored directory below this Linux port build/ or runs/')
    ap.add_argument('--cflags', default='',
                    help='extra compiler flags for the lifted tests')
    args = ap.parse_args()
    if not 1 <= args.encodings <= (CODE_SIZE - 0x2000) // 4 or not 1 <= args.cases <= 1024:
        ap.error('encodings/cases must be positive and fit the bounded code/case space')
    work = fresh_work(args.work)
    receipt = {'scope': 'synthetic single-instruction portable SIMD vs Unicorn Cortex-A57; no game input',
               'argv': sys.argv, 'cwd': ROOT, 'args': vars(args), 'host': list(platform.uname()),
               'python': sys.version, 'unicorn': uc.__version__, 'sources_before': source_hashes(),
               'status': 'running', 'work': work, 'limitations': ['FPSR exception/QC bits are not modeled or compared',
               'store memory compared by CRC32; no multi-instruction timing/synchronization or game coverage']}
    receipt_path = Path(work, 'receipt.json')
    receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
    status = 2
    try:
        status = run_suite(args, work, receipt)
    except (HarnessError, OSError, ValueError, uc.UcError) as error:
        receipt['error'] = str(error)
        print(f'ERROR: {error}')
    finally:
        receipt['sources_after'] = source_hashes()
        if receipt['sources_before'] != receipt['sources_after']:
            receipt['error'] = 'source/header inputs changed during validation'
            status = 2
        receipt['status'] = 'passed' if status == 0 else 'failed'
        receipt['artifacts'] = {p.name: hash_file(p) for p in Path(work).iterdir()
                                if p.is_file() and p != receipt_path}
        receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
        print(f'Receipt: {receipt_path}')
    return status


def run_suite(args, work, receipt):
    r = random.Random(args.seed)
    t0 = time.time()
    lifter = Lifter(0, 'c')
    emu = Emu()
    cands = candidates(r)
    if args.filter:
        val, mask = (int(x, 16) for x in args.filter.split('/'))
        cands = [w for w in cands if (w & mask) == val]
    if args.no_validity:
        sample = r.sample(cands, min(len(cands), args.encodings * 4))
        val = validity(lifter, sample, emu)
    else:
        val = validity(lifter, cands, emu)
    agree = Counter()
    only = defaultdict(list)
    for w, (lok, uok) in val.items():
        agree[(lok, uok)] += 1
        if lok != uok:
            only['lifter' if lok else 'unicorn'].append(w)
    print(f'validity: {len(val)} encodings, both valid {agree[(True, True)]}, both invalid {agree[(False, False)]}, '
          f'lifter-only {agree[(True, False)]}, unicorn-only {agree[(False, True)]} ({time.time() - t0:.0f}s)')
    receipt['validity'] = {'enumeration': 'sampled' if args.no_validity else 'full', 'checked': len(val),
                          'both_valid': agree[(True, True)], 'both_invalid': agree[(False, False)],
                          'lifter_only': only['lifter'], 'unicorn_only': only['unicorn']}
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)

    def dis(w):
        d = next(md.disasm(struct.pack('<I', w), 0), None)
        return f'{d.mnemonic} {d.op_str}' if d else '?'

    for kind in ('lifter', 'unicorn'):
        ws = only[kind]
        if ws:
            byname = Counter(dis(w).split(' ')[0] for w in ws)
            print(f'  {kind}-only by mnemonic: {dict(byname.most_common(30))}')
            for w in ws[:args.show // 2]:
                print(f'    {w:08x} {dis(w)}')

    # stratified sample of valid encodings: one per opcode signature first
    valid = [w for w, (lok, uok) in val.items() if lok and uok]
    r.shuffle(valid)
    seen, pick = set(), []
    for w in valid:
        k = class_key(w)
        if k not in seen:
            seen.add(k)
            pick.append(w)
    picked = set(pick)
    rest = [w for w in valid if w not in picked]
    pick = (pick + rest)[:args.encodings]
    if not pick:
        raise HarnessError('no mutually valid encodings selected')
    # place at CODE + 4i; literal loads get an offset into the code region
    code = []
    for i, w in enumerate(pick):
        pc = CODE + 4 * i
        if is_literal(w):
            tgt = CODE + r.randrange(0, (len(pick) * 4 + 0x1000) & ~3, 4)
            w = (w & ~(0x7FFFF << 5)) | ((((tgt - pc) >> 2) & 0x7FFFF) << 5)
            pick[i] = w
        code.append(w)
    code_bytes = struct.pack(f'<{len(code)}I', *code) + bytes(0x1000 + 64)
    bodies = []
    for i, w in enumerate(pick):
        ins = lifter.lift(w, CODE + 4 * i)
        if ins.kind != 'op':
            raise HarnessError(f'unexpected lifted instruction kind {ins.kind}')
        bodies.append(ins.code)
    receipt['encodings'] = [{'word': f'{w:08x}', 'disassembly': dis(w)} for w in pick]
    t1 = time.time()
    exe = compile_tests(work, bodies, args.jobs, args.cflags)
    print(f'compiled {len(bodies)} tests ({time.time() - t1:.0f}s)')

    mu = emu.mu
    mu.mem_write(CODE, code_bytes)
    mu.ctl_remove_cache(CODE, CODE + CODE_SIZE)
    buf0 = bytes(r.getrandbits(8) for _ in range(4096)) * (BUF_SIZE // 4096)
    mu.mem_write(BUF, buf0)
    with open(os.path.join(work, 'code.bin'), 'wb') as f:
        f.write(code_bytes)
    with open(os.path.join(work, 'buf.bin'), 'wb') as f:
        f.write(buf0)
    cases, expect, skipped = [], [], []
    t2 = time.time()
    for k, w in enumerate(pick):
        pc = CODE + 4 * k
        st = is_store(w)
        for _ in range(args.cases):
            fpcr, x, sp, nzcv, v = make_case(r, w)
            try:
                res = emu.run(pc, fpcr, x, sp, nzcv, v)
            except uc.UcError as error:
                skipped.append({'encoding_index': k, 'word': f'{w:08x}', 'error': error.errno})
                if st:
                    mu.mem_write(BUF, buf0)
                continue
            mh = 0
            if st:
                mh = zlib.crc32(bytes(mu.mem_read(BUF, BUF_SIZE)))
                mu.mem_write(BUF, buf0)
            cases.append((k, fpcr, x, sp, nzcv, v, st))
            expect.append(res + (mh,))
    receipt['unicorn_skipped'] = skipped
    receipt['executed_encodings'] = len({case[0] for case in cases})
    receipt['executed_cases'] = len(cases)
    receipt['fpcr_values'] = {f'{value:08x}': count for value, count in sorted(Counter(case[1] for case in cases).items())}
    receipt['store_cases'] = sum(case[-1] for case in cases)
    receipt['sampled_mnemonics'] = dict(Counter(dis(w).split(' ')[0] for w in pick))
    if not cases:
        raise HarnessError('Unicorn produced no executable cases')
    print(f'unicorn: {len(cases)} cases ({time.time() - t2:.0f}s)')
    with open(os.path.join(work, 'cases.bin'), 'wb') as f:
        for k, fpcr, x, sp, nzcv, v, st in cases:
            f.write(struct.pack(IN_FMT, k, fpcr, *(x + [0]), sp, nzcv))
            for q in v:
                f.write(q.to_bytes(16, 'little'))
            f.write(struct.pack('<QQ', int(st), 0))
    outp = os.path.join(work, 'out.bin')
    t3 = time.time()
    run_logged(work, 'run-harness', [exe] + [os.path.join(work, n) for n in ('code.bin', 'buf.bin', 'cases.bin')] + [outp])
    data = Path(outp).read_bytes()
    validate_output(data, len(cases))
    print(f'harness: {len(data) // OUT_SIZE} cases ({time.time() - t3:.0f}s)')
    fails = {}
    nfail = 0
    for i, ((k, fpcr, x, sp, nzcv, v, st), (ex, esp, enz, ev, emh)) in enumerate(zip(cases, expect)):
        rec = data[i * OUT_SIZE:(i + 1) * OUT_SIZE]
        hx_ = struct.unpack_from('<31Q', rec, 0)
        hsp, hnz, hmh = struct.unpack_from('<QQQ', rec, 248)
        hv = [int.from_bytes(rec[272 + 16 * j:288 + 16 * j], 'little') for j in range(32)]
        diffs = [f'x{j}: got {hx_[j]:x} want {ex[j]:x}' for j in range(31) if hx_[j] != ex[j]]
        if hsp != esp:
            diffs.append(f'sp: got {hsp:x} want {esp:x}')
        if hnz != enz:
            diffs.append(f'nzcv: got {hnz:x} want {enz:x}')
        if st and hmh != emh:
            diffs.append('memory differs')
        for j in range(32):
            if hv[j] != ev[j]:
                diffs.append(f'v{j}: got {hv[j]:032x} want {ev[j]:032x} (in {v[j]:032x})')
        if diffs:
            nfail += 1
            w = pick[k]
            if w not in fails:
                ins = [f'v{j}={v[j]:032x}' for j in sorted({(w >> 5) & 31, (w >> 16) & 31, w & 31})]
                fails[w] = (fpcr, diffs[:3], ins)
    print(f'{len(pick)} encodings, {len(cases)} cases: {nfail} failing cases, {len(fails)} failing encodings, '
          f'{len(skipped)} reference execution errors '
          f'({time.time() - t0:.0f}s total)')
    byname = Counter(dis(w).split(' ')[0] for w in fails)
    if fails:
        print('  failing by mnemonic:', dict(byname.most_common(40)))
    for w, (fpcr, d, ins) in list(fails.items())[:args.show]:
        print(f'  {w:08x} {dis(w):40} fpcr={fpcr:x} {"; ".join(d)}')
        print(f'      {" ".join(ins)}')
    receipt['failing_cases'] = nfail
    receipt['failures'] = {f'{w:08x}': {'fpcr': fpcr, 'differences': d, 'input_vectors': ins}
                           for w, (fpcr, d, ins) in fails.items()}
    receipt['seconds'] = time.time() - t0
    # [XCDE] LINUX-007: an error in the reference run leaves requested cases
    # unvalidated; a matching subset must never become a successful sample.
    return 1 if fails or skipped or only['lifter'] or only['unicorn'] else 0


if __name__ == '__main__':
    sys.exit(main())
