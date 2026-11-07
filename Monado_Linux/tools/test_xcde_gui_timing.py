#!/usr/bin/env python3
"""Optional selected-input GUI timing component test; never launches the game.

Compiles the actual checked patch emission and production helper, then executes
the selected ARM64 animation updater/getters in test-time Unicorn with synthetic
objects and a no-op property callback. Derived code and results stay under runs/.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import resource
import struct
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x7000000000
DATA = 0x7100000000
HOOK = BASE + 0x74BFC0
MAIN_SHA256 = '76ffa21b99c0385cfb14d1f90bbeddcdc98c17350b8edd79dfdbbb7341e70c48'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def float32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def float_bits(value):
    return struct.unpack('<I', struct.pack('<f', value))[0]


def source_snapshot():
    pending = [Path(__file__), ROOT / 'runtime/hle/framepace.c',
               ROOT / 'games/xcde/patches.py']
    pending += list((ROOT / 'tools/lifter').glob('*.py'))
    found = {}
    while pending:
        path = pending.pop().resolve()
        name = str(path.relative_to(ROOT))
        if name in found:
            continue
        found[name] = digest(path)
        if path.suffix in ('.c', '.h'):
            for include in re.findall(r'^\s*#\s*include\s+"([^"]+)"', path.read_text(), re.M):
                pending.append(path.parent / include)
    return dict(sorted(found.items()))


NATIVE_FIXTURE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include "runtime/hle/framepace.c"

// PATCH is emitted from selected input by the real patch and portable lifter.
static void patched_instruction(Cpu *c) { /*PATCH*/ }
int main(void) {
    void *page = (void *)0x7000b93000ull;
    assert(mmap(page, 4096, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == page);
    unsigned char gui[256], clock_object[0x2100], before[256], clock_before[0x2100];
    uint64_t slot = 0, *got = (uint64_t *)0x7000b93458ull;
    uint32_t record[3], result;
    unsigned records = 0;
    while (fread(record, sizeof record, 1, stdin) == 1) {
        for (unsigned i = 0; i < sizeof gui; i++) gui[i] = (unsigned char)(i * 17 + records);
        for (unsigned i = 0; i < sizeof clock_object; i++) clock_object[i] = (unsigned char)(i * 13 + 9);
        memcpy(gui + 0x6c, &record[1], 4);
        memcpy(clock_object + 0x2034, &record[0], 4);
        *got = record[2] ? (uintptr_t)&slot : 0;
        slot = record[2] == 2 ? (uintptr_t)clock_object : 0;
        memcpy(before, gui, sizeof gui);
        memcpy(clock_before, clock_object, sizeof clock_object);
        Cpu cpu, expected;
        for (unsigned i = 0; i < sizeof cpu; i++) ((unsigned char *)&cpu)[i] = (unsigned char)(i * 7 + records);
        cpu.x[0] = (uintptr_t)gui;
        memcpy(&expected, &cpu, sizeof cpu);
        memcpy(&expected.x[1], gui + 0x10, 8);
        memcpy(&expected.x[8], gui + 0x18, 8);
        patched_instruction(&cpu);
        assert(!memcmp(&cpu, &expected, sizeof cpu));
        memcpy(&result, gui + 0x70, 4);
        memcpy(before + 0x70, &result, 4);
        assert(!memcmp(before, gui, sizeof gui));
        assert(!memcmp(clock_before, clock_object, sizeof clock_object));
        assert(*got == (record[2] ? (uintptr_t)&slot : 0));
        assert(slot == (record[2] == 2 ? (uintptr_t)clock_object : 0));
        assert(fwrite(&result, sizeof result, 1, stdout) == 1);
        records++;
    }
    assert(feof(stdin));
    fprintf(stderr, "PASS %u checked-patch calls; guest CPU and neighboring bytes preserved\n", records);
    return 0;
}
'''


def scenarios():
    rows = []

    def add(name, steps, patched=True, global_scale=1., node_scale=1., paused=False,
            alternate_getter=False, parent_paused=False, clock_modes=None):
        rows.append(dict(name=name, steps=[float32(s) for s in steps], patched=patched,
                         global_scale=global_scale, node_scale=node_scale, paused=paused,
                         alternate_getter=alternate_getter, parent_paused=parent_paused,
                         clock_modes=clock_modes or [2] * len(steps)))

    for fps in (30, 60, 120, 144, 165):
        for patched in (False, True):
            add(f'{fps}fps', [1 / fps] * fps, patched)
    variable = [1 / 165, 1 / 60, 1 / 120, 1 / 30, 1 / 144] * 12
    for patched in (False, True):
        add('varying', variable, patched)
    for name, gs, ns, paused in [('global-paused', 0., 1., False),
                                 ('scaled-unity', .5, 2., False),
                                 ('scaled-half', 2., .25, False),
                                 ('node-paused', 1., 1., True)]:
        add(name, [1 / 165] * 165, global_scale=gs, node_scale=ns, paused=paused)
    add('hitch-cap', [0., .4, 1 / 165, 1 / 30, 1 / 120] * 4)
    add('startup-absent-clock', [1 / 30, 1 / 30, 1 / 165, 1 / 60], clock_modes=[0, 1, 2, 2])
    add('zero-elapsed', [0.] * 20)
    add('alternate-getter', [1 / 165] * 165, alternate_getter=True)
    add('parent-paused', [1 / 165] * 165, alternate_getter=True, parent_paused=True)
    return rows


def selected_consumer(nso, rows, native_steps, constructor_step):
    import unicorn
    from unicorn import arm64_const as a

    emulator = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
    emulator.mem_map(BASE, nso.size)
    emulator.mem_write(BASE, bytes(nso.image))
    emulator.mem_map(DATA, 0x10000)
    obj, vtable, gui, anim, entries = [DATA + i * 0x1000 for i in range(5)]
    stub, stop, parent = DATA + 0x5000, DATA + 0x5004, DATA + 0x7000

    def word(addr, value):
        emulator.mem_write(addr, struct.pack('<Q', value))

    def put_float(addr, value):
        emulator.mem_write(addr, struct.pack('<f', value))

    # Authored synthetic RET callbacks; no game property evaluation or rendering.
    emulator.mem_write(stub, struct.pack('<II', 0xD65F03C0, 0xD65F03C0))
    word(BASE + 0xB8A278, DATA + 0x6000)
    word(DATA + 0x6000, gui)
    word(BASE + 0xB949E8, DATA + 0x6008)
    word(obj, vtable)
    word(vtable + 0x60, stub)
    word(obj + 0x280, anim)
    word(obj + 0x2E0, parent)
    word(anim + 8, entries)
    put_float(entries + 0x18, 10000.)
    results = []
    position = 0
    for row in rows:
        expected = 0.
        emulator.mem_write(obj + 0xA0, bytes(16))
        emulator.mem_write(obj + 0xAC, struct.pack('<I', 0x240 if row['paused'] else 0x40))
        emulator.mem_write(parent, bytes([2 if row['parent_paused'] else 0]))
        getter = BASE + (0x74F640 if row['alternate_getter'] else 0x74EF00)
        word(vtable + 0x68, getter)
        put_float(obj + 0x218, row['node_scale'])
        put_float(gui + 0x80, row['global_scale'])
        put_float(anim + 0x14, 0.)
        intended_sum = 0.
        for elapsed, mode in zip(row['steps'], row['clock_modes'], strict=True):
            intended = min(elapsed, float32(2 / 30)) if mode == 2 else 0.
            intended_sum += intended
            step = native_steps[position] if row['patched'] else constructor_step
            position += 1
            if row['patched'] and float_bits(step) != float_bits(intended):
                raise AssertionError(f"{row['name']}: production hook delta differs from elapsed/cap")
            if row['name'] == '30fps' and float_bits(step) != float_bits(constructor_step):
                raise AssertionError('native 30 Hz timestep changed')
            delta = (0. if row['paused'] or row['parent_paused'] else
                     float32(float32(step * row['global_scale']) * row['node_scale']))
            expected = float32(expected + delta)
            put_float(gui + 0x70, step)
            emulator.reg_write(a.UC_ARM64_REG_X0, obj)
            emulator.reg_write(a.UC_ARM64_REG_X30, stop)
            emulator.reg_write(a.UC_ARM64_REG_SP, DATA + 0xF000)
            emulator.emu_start(BASE + 0x74EB10, stop, count=1000)
            if emulator.reg_read(a.UC_ARM64_REG_PC) != stop:
                raise AssertionError('selected animation component exceeded instruction bound')
            cursor = struct.unpack('<f', emulator.mem_read(anim + 0x14, 4))[0]
            if float_bits(cursor) != float_bits(expected):
                raise AssertionError(f"{row['name']}: actual ARM cursor disagrees with float32 accumulation")
        intended_cursor = (0. if row['paused'] or row['parent_paused'] else
                           intended_sum * row['global_scale'] * row['node_scale'])
        follows_elapsed = abs(cursor - intended_cursor) < 2e-5
        expect_elapsed = row['patched'] or row['name'] == '30fps'
        if follows_elapsed != expect_elapsed:
            raise AssertionError(f"{row['name']}: missing fix or baseline reproducer no longer fails")
        results.append({key: value for key, value in row.items() if key not in ('steps', 'clock_modes')} |
                       dict(frames=len(row['steps']), elapsed_seconds=sum(row['steps']),
                            intended_clamped_seconds=intended_sum, cursor_seconds=cursor,
                            expected_float32_cursor=expected, intended_cursor=intended_cursor,
                            follows_elapsed=follows_elapsed, expected_follows_elapsed=expect_elapsed,
                            passed=True))
    if position != len(native_steps):
        raise AssertionError('native/selected-component record count mismatch')
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--main', required=True, type=Path, help='selected extracted DE 2.0.0 exefs/main (read only)')
    parser.add_argument('--output', type=Path, help='fresh directory under Monado_Linux/runs')
    args = parser.parse_args()
    if sys.platform != 'linux' or platform.machine() != 'x86_64':
        parser.error('native Linux x86-64 required')
    main_path = args.main.resolve(strict=True)
    if digest(main_path) != MAIN_SHA256:
        parser.error('input differs from the verified DE 2.0.0 main; refusing address-specific tests')
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    os.umask(0o077)
    runs = ROOT / 'runs'
    if runs.is_symlink():
        parser.error('runs must not be a symlink')
    runs.mkdir(exist_ok=True)
    if args.output:
        output = args.output.absolute()
        if output != output.resolve() or not output.is_relative_to(runs) or output.exists():
            parser.error('--output must be fresh and non-symlink under Monado_Linux/runs')
        output.mkdir(parents=True)
    else:
        output = Path(tempfile.mkdtemp(prefix='xcde-gui-timing.', dir=runs))
    sys.path.insert(0, str(ROOT / 'tools/lifter'))
    import unicorn
    from gen import Module
    from patchfmt import PatchMismatch, load_patches

    receipt = dict(status='running', scope=__doc__, cwd=str(ROOT), argv=sys.argv,
                   main_path=str(main_path), main_sha256=digest(main_path),
                   host=list(platform.uname()), python=sys.version, unicorn=unicorn.__version__,
                   sources_before=source_snapshot(), commands=[], artifacts={},
                   limitations=['Synthetic GUI/node graph; visual property callback is a no-op.',
                                'No launcher/game/UI, real input repeat, worker scheduling, or save test.',
                                'Unicorn is a test oracle only, never a runtime execution fallback.',
                                'Cursor correctness does not prove all equipment/menu/animation paths are fixed.'])

    def save():
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(name, argv, data=None):
        command = dict(name=name, argv=list(map(str, argv)), timeout_seconds=60)
        receipt['commands'].append(command)
        save()
        child = subprocess.run(command['argv'], input=data, cwd=ROOT,
                               env={'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8',
                                    'LC_ALL': 'C.UTF-8', 'TMPDIR': str(output)},
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        (output / (name + '.stdout')).write_bytes(child.stdout)
        (output / (name + '.stderr')).write_bytes(child.stderr)
        command['exit'] = child.returncode
        if child.returncode:
            raise RuntimeError(f'{name} failed ({child.returncode}); see {output}')
        return child.stdout

    save()
    try:
        module = Module('main', str(main_path), BASE, simd='c')
        patch = load_patches(str(ROOT / 'games/xcde/patches.py'))[HOOK]
        emitted = patch.apply(module, HOOK)
        # Exercise the real expected-word guard, with the original lifter retained.
        original_word = module.word
        module.word = lambda address: original_word(address) ^ 1
        try:
            patch.apply(module, HOOK)
        except PatchMismatch:
            pass
        else:
            raise AssertionError('checked hook accepted a different input instruction')
        finally:
            module.word = original_word
        reference, constructor_step = struct.unpack_from('<ff', module.nso.image, 0x9D0AD8)
        if reference != 30 or float_bits(constructor_step) != float_bits(1 / 30):
            raise AssertionError('selected GUI constructor constants changed')
        receipt.update(build_id=module.nso.build_id, hook_address=hex(HOOK),
                       original_word=hex(module.word(HOOK)), mismatch_guard_passed=True,
                       constructor_reference_fps=reference, constructor_step_seconds=constructor_step)
        fixture = output / 'selected_gui_hook.c'
        fixture.write_text(NATIVE_FIXTURE.replace('/*PATCH*/', emitted))
        compiler = ROOT / '.deps/bin/clang-cc'
        receipt['compiler_sha256'] = digest(compiler)
        run('compiler-version', [compiler, '--version'])
        binary = output / 'selected_gui_hook'
        run('compile', [compiler, '-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off',
                        '-fno-strict-aliasing', '-ffunction-sections', '-fdata-sections',
                        '-Wl,--gc-sections', '-I', ROOT, fixture, '-lm', '-o', binary])
        rows = scenarios()
        records = b''.join(struct.pack('<III', float_bits(delta), float_bits(reference), mode)
                           for row in rows for delta, mode in zip(row['steps'], row['clock_modes'], strict=True))
        native = run('native-checked-hook', [binary], records)
        if len(native) != len(records) // 3:
            raise AssertionError('native hook did not return one delta per record')
        native_steps = list(struct.unpack(f'<{len(native) // 4}f', native))
        receipt['results'] = selected_consumer(module.nso, rows, native_steps, constructor_step)
        receipt['native_patch_calls'] = len(native_steps)
        receipt['sources_after'] = source_snapshot()
        if receipt['sources_after'] != receipt['sources_before']:
            raise RuntimeError('test sources changed during verification; rerun after integration settles')
        if digest(main_path) != receipt['main_sha256']:
            raise RuntimeError('selected input changed during test')
        receipt['status'] = 'passed'
    except BaseException as error:
        receipt.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        receipt['artifacts'] = {p.name: digest(p) for p in sorted(output.iterdir())
                                if p.is_file() and p.name != 'receipt.json'}
        save()
    print(f"PASS {len(receipt['results'])} selected GUI timing cases; "
          f"{receipt['native_patch_calls']} native checked-patch calls; {output / 'receipt.json'}")


if __name__ == '__main__':
    main()
