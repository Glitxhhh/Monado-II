#!/usr/bin/env python3
"""Selected XCDE locomotion comparison regression; no game, UI or saves.

Compile actual checked patches and the existing party timing helper, compare
all CPU bytes with independent subtraction flags, then feed those native flags
into selected ARM64 movement predicates and the walk transition dispatcher.
Derived fixtures, binaries and results remain under ignored runs/.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import resource
import signal
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x7000000000
SITES = (BASE + 0x44394c, BASE + 0x443b9c, BASE + 0x45b370)
MAIN_SHA256 = '76ffa21b99c0385cfb14d1f90bbeddcdc98c17350b8edd79dfdbbb7341e70c48'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def snapshot():
    pending = [Path(__file__), ROOT / 'runtime/hle/party.c', ROOT / 'games/xcde/patches.py']
    pending += list((ROOT / 'tools/lifter').glob('*.py'))
    found = {}
    while pending:
        path = pending.pop().resolve()
        name = str(path.relative_to(ROOT))
        if name in found:
            continue
        found[name] = digest(path)
        if path.suffix in ('.c', '.h', '.inc'):
            pending += [path.parent / include for include in
                        re.findall(r'^\s*#\s*include\s+"([^"]+)"', path.read_text(), re.M)]
    return dict(sorted(found.items()))


NATIVE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <limits.h>
#include "runtime/hle/party.c"
Settings g_settings;
static uint64_t clock_ns = 1000000000ull;
uint64_t plat_now_ns(void) { return clock_ns; }
/*PATCHES*/
static void (*patches[])(Cpu *) = {patch0, patch1, patch2};
static void (*originals[])(Cpu *) = {original0, original1, original2};
static uint64_t flags(uint32_t a, uint32_t b) {
    uint32_t result = a - b;
    int64_t signed_result = (int64_t)(int32_t)a - (int64_t)(int32_t)b;
    return ((uint64_t)(result >> 31) << 31) | ((uint64_t)(result == 0) << 30)
        | ((uint64_t)(a >= b) << 29)
        | ((uint64_t)(signed_result < INT32_MIN || signed_result > INT32_MAX) << 28);
}
static void check(uint32_t k, uint32_t counter, uint32_t output[3]) {
    assert(k >= 1 && k <= 8);
    assert(xcde_frames_gt(9) == 10 * k - 1);
    for (unsigned site = 0; site < 3; site++) {
        Cpu cpu, expected, original;
        for (unsigned i = 0; i < sizeof cpu; i++) ((unsigned char *)&cpu)[i] = (unsigned char)(i * 13 + counter);
        cpu.x[8] = 0xfedcba9800000000ull | counter;
        memcpy(&expected, &cpu, sizeof cpu); memcpy(&original, &cpu, sizeof cpu);
        expected.nzcv = flags(counter, 10 * k - (site == 2 ? 0 : 1));
        patches[site](&cpu);
        assert(!memcmp(&cpu, &expected, sizeof cpu));
        originals[site](&original);
        if (k == 1) assert(!memcmp(&cpu, &original, sizeof cpu));
        output[site] = (uint32_t)cpu.nzcv;
    }
}
int main(int argc, char **argv) {
    unsigned calls = 0;
    if (argc == 2 && !strcmp(argv[1], "matrix")) {
        uint32_t row[2], out[3];
        while (fread(row, sizeof row, 1, stdin) == 1) {
            g_frames_per_30 = row[0]; check(row[0], row[1], out);
            assert(fwrite(out, sizeof out, 1, stdout) == 1); calls += 3;
        }
        assert(feof(stdin));
    } else if (argc == 2 && !strcmp(argv[1], "clock")) {
        uint32_t row[3];
        while (fread(row, sizeof row, 1, stdin) == 1) {
            g_settings.fps = (int)row[0]; clock_ns += row[1]; frames30_update(row[2]);
            uint32_t k = g_frames_per_30, out[3], threshold = xcde_frames_gt(9) + 1;
            assert(threshold == 10 * k);
            const uint32_t counters[] = {9, 10, threshold - 1, threshold};
            for (unsigned i = 0; i < 4; i++) { check(k, counters[i], out); calls += 3; }
            // Multiple pad reads in one frame must not update the estimator.
            frames30_update(row[2]); assert(g_frames_per_30 == k);
            assert(fwrite(&threshold, sizeof threshold, 1, stdout) == 1);
        }
        assert(feof(stdin));
    } else abort();
    fprintf(stderr, "PASS %u native checked comparisons; full Cpu and independent flags\n", calls);
}
'''


def native_rows():
    # Full small-counter range includes every relevant off-by-one boundary;
    # signed extremes retain the original CMP/CSET signed semantics.
    counters = list(range(82)) + [9999, 10000, 0x7fffffff, 0x80000000, 0xffffffff]
    return [(k, n) for k in range(1, 9) for n in counters]


def clock_rows():
    rows, stages = [], []
    for fps, actual in ((30, 30), (60, 60), (90, 90), (120, 120), (130, 130), (144, 144), (165, 165),
                        (0, 90), (0, 240), (30, 30), (165, 165), (60, 60)):
        for unused in range(600):
            rows.append((fps, round(1e9 / actual), len(rows)))
        stages.append(dict(setting=fps, actual_hz=actual, end=len(rows) - 1,
                           expected_scale=1 if fps == 30 else 2 if fps == 60 else round(actual / 30)))
    return rows, stages


def selected_components(nso, native_flags):
    import unicorn
    from unicorn import arm64_const as a

    emu = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
    emu.mem_map(BASE, nso.size); emu.mem_write(BASE, bytes(nso.image))
    data, stack, stop = 0x100000, 0x200000, 0x300000
    for address in (data, stack, stop):
        emu.mem_map(address, 0x10000)
    emu.reg_write(a.UC_ARM64_REG_CPACR_EL1, 3 << 20)
    owner, registry, action_comp, actor = [data + offset for offset in (0, 0x1000, 0x2000, 0x3000)]
    movement, vtable, mode_object, registry_types, listener, condition = [
        data + offset for offset in (0x5000, 0x6000, 0x7000, 0x8000, 0x9000, 0xa000)]

    def put(address, fmt, *values):
        emu.mem_write(address, struct.pack('<' + fmt, *values))

    def get(address, fmt):
        return struct.unpack('<' + fmt, emu.mem_read(address, struct.calcsize('<' + fmt)))[0]

    for address in (owner, registry, action_comp, actor):
        put(address, 'Q', vtable)
    put(owner + 0x40, 'Q', registry); put(owner + 0x50, 'Q', mode_object)
    put(BASE + 0xb80bf0, 'Q', registry_types); put(registry_types + 6, 'h', 1)
    put(listener + 0x20, 'Q', owner)
    adapters = {stop + 0x100: action_comp, stop + 0x110: 1, stop + 0x120: actor,
                stop + 0x130: movement, BASE + 0xef0: 1}
    for offset, callback in ((0x38, stop + 0x100), (0, stop + 0x110),
                             (0x50, stop + 0x120), (0xb8, stop + 0x130)):
        put(vtable + offset, 'Q', callback)
    patched = False
    scale = counter = 1
    visits = {}

    def hook(machine, address, size, unused):
        if address in (BASE + 0x456f70, BASE + 0x45b1f0, BASE + 0x3f5570, SITES[2]):
            visits[hex(address)] = visits.get(hex(address), 0) + 1
        if address in adapters:
            machine.reg_write(a.UC_ARM64_REG_X0, adapters[address])
            machine.reg_write(a.UC_ARM64_REG_PC, machine.reg_read(a.UC_ARM64_REG_LR))
        elif address == SITES[2] and patched:
            # Feed the flags from the actual native checked patch/helper;
            # execute the original selected CSET and all later instructions.
            if machine.reg_read(a.UC_ARM64_REG_W8) != counter:
                raise AssertionError('selected counter differs from fixture input')
            machine.reg_write(a.UC_ARM64_REG_NZCV, native_flags[(scale, counter)][2])
            machine.reg_write(a.UC_ARM64_REG_PC, address + 4)
        elif address == BASE + 0x5ab020:
            raise AssertionError('unexpected selected assertion boundary')

    emu.hook_add(unicorn.UC_HOOK_CODE, hook)

    def run(offset, ratio, count, fraction, mode, disabled, code=None):
        nonlocal counter
        counter = count
        put(movement + 0x28, '3f', 1., 1., 1.)
        put(movement + 0x130, '3f', ratio, 0., 0.)
        put(movement + 0x200, 'I', counter)
        # Unrelated navigation/collision fields must remain unchanged.
        put(movement + 0x226, 'H', 0x5a5a); put(movement + 0x250, 'I', 0xa55ac33c)
        put(mode_object + 8, 'i', mode); put(actor + 0x1518, 'B', 4 if disabled else 0)
        initial_flags = 0xa55a123456789abc
        put(actor + 0x1030, 'Q', initial_flags)
        put(condition + 8, 'I', code if code is not None else 0)
        emu.reg_write(a.UC_ARM64_REG_X0, listener if code is not None else owner)
        emu.reg_write(a.UC_ARM64_REG_X1, condition if code is not None else 0)
        emu.reg_write(a.UC_ARM64_REG_S0, bits(fraction))
        emu.reg_write(a.UC_ARM64_REG_SP, stack + 0x8000)
        emu.reg_write(a.UC_ARM64_REG_LR, stop)
        before = bytearray(emu.mem_read(data, 0xb000))
        emu.emu_start(BASE + offset, stop, count=3000)
        if emu.reg_read(a.UC_ARM64_REG_PC) != stop:
            raise AssertionError('selected predicate/dispatcher exceeded instruction bound')
        result = emu.reg_read(a.UC_ARM64_REG_W0)
        signed_counter = struct.unpack('<i', struct.pack('<I', counter))[0]
        limit = 10 * scale if patched else 10
        passed = int(not disabled and bits(ratio) >= bits(fraction)
                     and (mode == 0 or signed_counter < limit))
        expected = passed if code != 49 else 1 - passed
        if result != expected:
            raise AssertionError(('predicate result', scale, ratio, counter, fraction, mode, disabled, code, patched, result, expected))
        if code is not None:
            expected_flags = (initial_flags | (1 << 38)) if passed else (initial_flags & ~(1 << 38))
            struct.pack_into('<Q', before, actor + 0x1030 - data, expected_flags)
        if emu.mem_read(data, 0xb000) != before:
            raise AssertionError('predicate/dispatcher changed unrelated borrowed state')
        return result

    cases, changes = 0, 0
    for scale in range(1, 9):
        for ratio in (.3, .45, .5, .55, .8, 1.):
            for count in sorted({0, 9, 10, 10 * scale - 1, 10 * scale, 10000, 0x80000000, 0xffffffff}):
                for mode in (0, 1, -1):
                    for disabled in (False, True):
                        for code, fraction in ((44, .55), (49, .45)):
                            values = []
                            for patched in (False, True):
                                predicate = run(0x45b1f0, ratio, count, fraction, mode, disabled)
                                transition = run(0x456f70, ratio, count, fraction, mode, disabled, code)
                                if transition != (predicate if code == 44 else 1 - predicate):
                                    raise AssertionError('actual dispatcher lost positive/inverted predicate relationship')
                                values.append(transition)
                            if scale == 1 and values[0] != values[1]:
                                raise AssertionError('native30 behavior changed')
                            changes += values[0] != values[1]
                            cases += 1

    # Actual loop through updates demonstrates the pre-fix raw10 versus
    # corrected10*k decision boundary, without claiming a full gait simulation.
    timelines = []
    for fps, scale in ((30, 1), (60, 2), (90, 3), (120, 4), (130, 4), (144, 5), (165, 6), (240, 8)):
        outcomes = []
        for patched in (False, True):
            values = [run(0x456f70, 1., count, .55, 1, False, 44) for count in range(10 * scale + 1)]
            first_false = values.index(0)
            if first_false != (10 * scale if patched else 10):
                raise AssertionError('transition timeline has wrong boundary')
            outcomes.append(dict(patched=patched, first_false_update=first_false, seconds=first_false / fps))
        timelines.append(dict(fps=fps, scale=scale, outcomes=outcomes))
    return dict(boundary_cases=cases, changed_cases=changes, timelines=timelines, visits=visits,
                adapters='Synthetic object registry, vtables and RTTI only; full45b1f0,3f5570,456f70 execute.',
                scope='Native emitted flags supplied at checkedCMP only; no sound/collision/counter producer changes.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--main', type=Path, required=True)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if sys.platform != 'linux' or platform.machine() != 'x86_64':
        parser.error('native Linux x86-64 required')
    selected = args.main.resolve(strict=True)
    if digest(selected) != MAIN_SHA256:
        parser.error('selected main differs from verified DE 2.0.0')
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0)); os.umask(0o077)
    runs = ROOT / 'runs'
    if runs.is_symlink():
        parser.error('runs must not be a symlink')
    runs.mkdir(exist_ok=True)
    if args.output:
        output = args.output.absolute()
        if output != output.resolve() or not output.is_relative_to(runs) or output.exists():
            parser.error('--output must be fresh and non-symlink beneath runs')
        output.mkdir(parents=True)
    else:
        output = Path(tempfile.mkdtemp(prefix='xcde-locomotion.', dir=runs))
    sys.path.insert(0, str(ROOT / 'tools/lifter'))
    import unicorn
    from gen import Module
    from patchfmt import PatchMismatch, load_patches
    receipt = dict(status='running', cwd=str(ROOT), argv=sys.argv, scope=__doc__,
                   host=list(platform.uname()), python=sys.version, unicorn=unicorn.__version__,
                   input_sha256=digest(selected), sources_before=snapshot(), commands=[],
                   limitations=['Existing rounded FPS estimator is preserved, not an elapsed-time redesign.',
                                'Synthetic guest objects and lookup boundaries; no game/audio/UI/save test.'])

    def save():
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(name, argv, data=None, off=False, expected_exit=0):
        env = dict(PATH='/usr/bin:/bin', LANG='C.UTF-8', LC_ALL='C.UTF-8', TMPDIR=str(output))
        if off:
            env['XCDE_FRAMES30_OFF'] = '1'
        command = dict(name=name, argv=list(map(str, argv)), cwd=str(ROOT), env=env,
                       timeout_seconds=60, expected_exit=expected_exit)
        receipt['commands'].append(command); save()
        proc = subprocess.Popen(command['argv'], cwd=ROOT, env=env, stdin=subprocess.PIPE,
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        try:
            stdout, stderr = proc.communicate(data, timeout=60)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            os.killpg(proc.pid, signal.SIGKILL); stdout, stderr = proc.communicate(timeout=5)
            command['timed_out_or_interrupted'] = True
        (output / (name + '.stdout')).write_bytes(stdout); (output / (name + '.stderr')).write_bytes(stderr)
        command['exit'] = proc.returncode
        if proc.returncode != expected_exit or command.get('timed_out_or_interrupted'):
            raise RuntimeError(f'{name} failed; see {output}')
        return stdout, stderr

    save()
    try:
        module = Module('main', str(selected), BASE, simd='c')
        patches = load_patches(str(ROOT / 'games/xcde/patches.py'))
        functions = []
        for index, site in enumerate(SITES):
            patch = patches[site]; emitted = patch.apply(module, site)
            instruction = module.lift(site)
            if instruction.kind != 'op':
                raise AssertionError('comparison did not lift to a plain instruction')
            functions += [f'static void patch{index}(Cpu *c) {{ {emitted} }}',
                          f'static void original{index}(Cpu *c) {{ {instruction.code} }}']
            saved = module.word; module.word = lambda address: saved(address) ^ 1
            try:
                patch.apply(module, site)
            except PatchMismatch:
                pass
            else:
                raise AssertionError('wrong instruction passed patch guard')
            finally:
                module.word = saved
        receipt['mismatch_guards'] = len(SITES)
        fixture = output / 'locomotion_native.c'; fixture.write_text(NATIVE.replace('/*PATCHES*/', '\n'.join(functions)))
        compiler = ROOT / '.deps/bin/clang-cc'; receipt['compiler_sha256'] = digest(compiler)
        run('compiler-version', [compiler, '--version'])
        binary = output / 'locomotion-native'
        compile_flags = ['-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off',
                         '-fno-strict-aliasing', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                         '-I', ROOT]
        run('compile', [compiler, *compile_flags, fixture, '-pthread', '-lm', '-o', binary])
        rows = native_rows(); data = b''.join(struct.pack('<II', *row) for row in rows)
        (output / 'matrix-input.bin').write_bytes(data)
        stdout, stderr = run('native-matrix', [binary, 'matrix'], data)
        if len(stdout) != 12 * len(rows) or not stderr.startswith(f'PASS {len(rows)*3} native'.encode()):
            raise AssertionError('native matrix output/coverage mismatch')
        flags = {row: struct.unpack_from('<III', stdout, 12 * index) for index, row in enumerate(rows)}
        receipt['native_comparisons'] = len(rows) * 3
        # Demonstrate that the oracle rejects the exact tempting off-by-one
        # regression: using the existing <= threshold directly for signed <.
        text = fixture.read_text()
        if text.count('xcde_frames_gt(9) + 1u') != 1:
            raise AssertionError('off-by-one mutation no longer identifies exactly the new compare')
        mutant_source, mutant_binary = output / 'off_by_one.c', output / 'off-by-one'
        mutant_source.write_text(text.replace('xcde_frames_gt(9) + 1u', 'xcde_frames_gt(9)'))
        run('compile-off-by-one', [compiler, *compile_flags, mutant_source, '-pthread', '-lm', '-o', mutant_binary])
        unused, rejected = run('reject-off-by-one', [mutant_binary, 'matrix'],
                               struct.pack('<II', 1, 9), expected_exit=-signal.SIGABRT)
        if b'!memcmp(&cpu, &expected, sizeof cpu)' not in rejected:
            raise AssertionError('off-by-one mutant failed for an unexpected reason')
        receipt['off_by_one_negative_control'] = 'expected CPU comparison assertion rejected threshold9 at counter9/native30'
        clocks, stages = clock_rows(); clock_data = b''.join(struct.pack('<III', *row) for row in clocks)
        (output / 'clock-input.bin').write_bytes(clock_data)
        for off in (False, True):
            stdout, stderr = run('clock-off' if off else 'clock-on', [binary, 'clock'], clock_data, off)
            if len(stdout) != len(clocks) * 4 or not stderr.startswith(f'PASS {len(clocks)*12} native'.encode()):
                raise AssertionError('clock output/coverage mismatch')
            thresholds = struct.unpack('<' + 'I' * len(clocks), stdout)
            for stage in stages:
                expected = 10 if off else 10 * stage['expected_scale']
                if thresholds[stage['end']] != expected:
                    raise AssertionError(f'estimator stage mismatch: {stage}, {thresholds[stage["end"]]}')
            if off and set(thresholds) != {10}:
                raise AssertionError('off override changed threshold')
        receipt['clock_stages'] = stages; receipt['clock_updates_per_mode'] = len(clocks)
        receipt['selected_results'] = selected_components(module.nso, flags)
        receipt['sources_after'] = snapshot()
        if receipt['sources_before'] != receipt['sources_after'] or digest(selected) != receipt['input_sha256']:
            raise RuntimeError('source/input changed during verification')
        receipt['status'] = 'passed'
    except BaseException as error:
        receipt.update(status='failed', error=f'{type(error).__name__}: {error}'); raise
    finally:
        receipt['artifacts'] = {p.name: digest(p) for p in sorted(output.iterdir()) if p.is_file() and p.name != 'receipt.json'}
        save()
    print(f'PASS locomotion timing components; {output / "receipt.json"}')


if __name__ == '__main__':
    main()
