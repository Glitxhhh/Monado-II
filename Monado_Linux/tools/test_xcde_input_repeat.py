#!/usr/bin/env python3
"""Selected-input repeat component test; never launches game/UI or reads saves.

Compile every production checked patch and helper with synthetic clock objects,
then feed their exact results into the selected ARM64 held-duration updater and
two existing repeat consumers. Derived fixtures/results stay under runs/.
"""
import argparse
import hashlib
import json
import math
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
BASE, DATA = 0x7000000000, 0x7100000000
MAIN_SHA256 = '76ffa21b99c0385cfb14d1f90bbeddcdc98c17350b8edd79dfdbbb7341e70c48'


def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def f32(value): return struct.unpack('<f', struct.pack('<f', value))[0]
def bits(value): return struct.unpack('<I', struct.pack('<f', value))[0]


def snapshot():
    pending = [Path(__file__).resolve(), ROOT / 'runtime/hle/framepace.c', ROOT / 'games/xcde/patches.py']
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


NATIVE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <sys/mman.h>
#include "runtime/hle/framepace.c"
/*PATCH_FUNCTIONS*/
static void (*const patches[])(Cpu *) = { /*PATCH_TABLE*/ };
int main(void) {
    void *page = (void *)0x7000b93000ull;
    assert(mmap(page, 4096, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == page);
    unsigned char clock_object[0x2100], clock_before[0x2100], update[20], update_before[20];
    uint64_t slot, *got = (uint64_t *)0x7000b93458ull;
    uint32_t record[3], result;
    unsigned calls = 0, records = 0;
    while (fread(record, sizeof record, 1, stdin) == 1) {
        assert(record[2] <= 2);
        for (unsigned i = 0; i < sizeof update; i++) update[i] = (unsigned char)(i * 11 + records);
        for (unsigned i = 0; i < sizeof clock_object; i++) clock_object[i] = (unsigned char)(i * 13 + 7);
        memcpy(update + 4, &record[1], 4);
        memcpy(clock_object + 0x2034, &record[0], 4);
        *got = record[2] ? (uintptr_t)&slot : 0;
        slot = record[2] == 2 ? (uintptr_t)clock_object : 0;
        memcpy(update_before, update, sizeof update);
        memcpy(clock_before, clock_object, sizeof clock_object);
        float elapsed, reference, expected_delta;
        memcpy(&elapsed, &record[0], 4); memcpy(&reference, &record[1], 4);
        expected_delta = record[2] == 2 ? fminf(elapsed, 2 * reference) : 0;
        uint32_t expected_bits; memcpy(&expected_bits, &expected_delta, 4);
        for (unsigned site = 0; site < sizeof patches / sizeof patches[0]; site++) {
            Cpu cpu, expected;
            for (unsigned i = 0; i < sizeof cpu; i++) ((unsigned char *)&cpu)[i] = (unsigned char)(i * 7 + calls);
            cpu.x[20] = (uintptr_t)update;
            memcpy(&expected, &cpu, sizeof cpu);
            expected.v[1] = expected_bits; // Scalar LDR clears all upper 96 bits.
            patches[site](&cpu);
            assert(!memcmp(&cpu, &expected, sizeof cpu));
            assert(!memcmp(update_before, update, sizeof update));
            assert(!memcmp(clock_before, clock_object, sizeof clock_object));
            assert(*got == (record[2] ? (uintptr_t)&slot : 0));
            assert(slot == (record[2] == 2 ? (uintptr_t)clock_object : 0));
            result = (uint32_t)cpu.v[1];
            assert(fwrite(&result, sizeof result, 1, stdout) == 1);
            calls++;
        }
        records++;
    }
    assert(feof(stdin));
    fprintf(stderr, "PASS %u native checked-patch calls; CPU, scalar upper bits, clock and borrowed UpdateInfo preserved\n", calls);
    return 0;
}
'''


def scenarios():
    rows = []

    def add(name, times, buttons=None, sticks=None, modes=None, pad_mode=0):
        rows.append(dict(name=name, times=[f32(t) for t in times],
                         buttons=buttons or [0x1000] * len(times),
                         sticks=sticks or [(0., 0., 0., 0.)] * len(times),
                         modes=modes or [2] * len(times), pad_mode=pad_mode))

    for fps in (30, 60, 120, 144, 165): add(f'{fps}fps-hold', [1 / fps] * (fps * 2))
    add('variable', [1 / 165, 1 / 60, 1 / 144, 1 / 30, 1 / 120] * 30)
    add('zero-and-hitches', [0., .4, 1 / 165, 1 / 30, .01, 1.] * 12)
    add('absent-clock', [1 / 165] * 165, modes=[0] * 5 + [1] * 5 + [2] * 155)
    for fps in (30, 165):
        add(f'{fps}fps-taps', [1 / fps] * 10, buttons=[0, 0x1000, 0, 0, 0x1000, 0x1000, 0, 0, 0x2000, 0])
    add('release-and-repress', [1 / 165] * 500,
        buttons=[0x1000] * 180 + [0] * 20 + [0x2000] * 200 + [0] * 100)
    add('chords-and-direction-changes', [1 / 165] * 500,
        buttons=[0x1000] * 100 + [0x3000] * 100 + [0x2000] * 100 + [0x8000] * 100 + [0] * 100)
    add('long-hold', [1 / 165] * 1650)
    # Every direction of both analog sticks plus digital buttons exercises the
    # actual updater branches; the mode-1 D-pad vector adds its aggregate timer.
    axes = [(1., 0.), (0., 1.), (-1., 0.), (0., -1.)]
    sticks = [(*axis, *axis) for axis in axes for _ in range(100)]
    add('analog-cardinals', [1 / 165] * len(sticks), buttons=[0] * len(sticks), sticks=sticks)
    add('all-button-durations', [1 / 165] * 100, buttons=[0xffff] * 100)
    add('dpad-vector-mode', [1 / 165] * 200, pad_mode=1)
    return rows


def selected_runs(nso, sites, rows, native):
    import unicorn
    from unicorn import arm64_const as a
    uc = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
    uc.mem_map(BASE, nso.size); uc.mem_write(BASE, bytes(nso.image))
    uc.mem_map(DATA, 0x20000)
    repeater, raw, update, callback, zero = [DATA + i * 0x1000 for i in range(5)]
    stop = DATA + 0x1f000
    events, reached = [], set()
    active_patch = False
    patch_value = frame_number = 0
    elapsed_total = 0.

    def put(addr, fmt, *values): uc.mem_write(addr, struct.pack('<' + fmt, *values))
    def get(addr, fmt): return struct.unpack('<' + fmt, uc.mem_read(addr, struct.calcsize('<' + fmt)))

    def hook(machine, address, size, ignored):
        nonlocal events
        if address in sites:
            reached.add(address)
            if active_patch:
                machine.reg_write(a.UC_ARM64_REG_Q1, patch_value)
                machine.reg_write(a.UC_ARM64_REG_PC, address + 4)
            return
        if address == BASE + 0x25b90:
            machine.reg_write(a.UC_ARM64_REG_X0, raw)
        elif address == BASE + 0x8b6dc8:
            dst, value, count = [machine.reg_read(r) for r in (a.UC_ARM64_REG_X0, a.UC_ARM64_REG_X1, a.UC_ARM64_REG_X2)]
            if count > 0x1000: raise AssertionError('memset adapter bounds')
            machine.mem_write(dst, bytes([value & 255]) * count)
        elif address == BASE + 0x8b6cc8: # selected import is atan2f
            y, x = [struct.unpack('<f', struct.pack('<I', machine.reg_read(r)))[0]
                    for r in (a.UC_ARM64_REG_S0, a.UC_ARM64_REG_S1)]
            machine.reg_write(a.UC_ARM64_REG_Q0, bits(math.atan2(y, x)))
        elif address == BASE + 0x8b6c28: # synthetic, single-thread __cxa_guard_acquire
            guard = machine.reg_read(a.UC_ARM64_REG_X0)
            machine.reg_write(a.UC_ARM64_REG_X0, int(machine.mem_read(guard, 1)[0] == 0))
        elif address == BASE + 0x8b6c38:
            machine.mem_write(machine.reg_read(a.UC_ARM64_REG_X0), b'\1')
        elif address == BASE + 0x578ab0:
            ptr = machine.reg_read(a.UC_ARM64_REG_X1)
            events.append(dict(frame=frame_number, time=elapsed_total,
                               code=get(ptr, 'I')[0], count=get(ptr + 4, 'I')[0]))
            machine.reg_write(a.UC_ARM64_REG_X0, 0)
        else:
            return
        machine.reg_write(a.UC_ARM64_REG_PC, machine.reg_read(a.UC_ARM64_REG_X30))

    uc.hook_add(unicorn.UC_HOOK_CODE, hook)
    put(BASE + 0xb8a960, 'Q', zero)

    def call(address, *args):
        for index, value in enumerate(args): uc.reg_write(a.UC_ARM64_REG_X0 + index, value)
        uc.reg_write(a.UC_ARM64_REG_X30, stop)
        uc.reg_write(a.UC_ARM64_REG_SP, DATA + 0x1e000)
        uc.emu_start(BASE + address, stop, count=10000)
        if uc.reg_read(a.UC_ARM64_REG_PC) != stop:
            raise AssertionError(f'instruction limit at {uc.reg_read(a.UC_ARM64_REG_PC):#x}')

    results, position = [], 0
    reference = f32(1 / 30)
    for row in rows:
        delta_bits = native[position:position + len(row['times'])]
        position += len(row['times'])
        for consumer in (0x164930, 0x1c1ef0):
            variants = {}
            for variant in ('legacy', 'oracle', 'patched'):
                active_patch = variant == 'patched'
                events, state_hashes = [], []
                uc.mem_write(repeater, bytes(0x400)); uc.mem_write(callback, bytes(0x100))
                call(0x578940, repeater)
                if get(repeater + 0x40, 'f')[0] != .5 or get(repeater + 0x54, 'f')[0] != 10:
                    raise AssertionError('selected repeat defaults changed')
                put(repeater + 0x50, 'I', row['pad_mode'])
                put(callback + 0x68, 'Q', 1); put(callback + 0x10, 'I', 1)
                previous = 0
                elapsed_total = 0.
                for frame_number, (dt, held, sticks, mode, patch_value) in enumerate(
                        zip(row['times'], row['buttons'], row['sticks'], row['modes'], delta_bits, strict=True), 1):
                    elapsed_total += dt
                    uc.mem_write(raw, bytes(32))
                    put(raw, 'IIII', held & ~previous, previous & ~held, held, (~held) & 0xffffffff)
                    put(raw + 16, '4f', *sticks)
                    correct = min(dt, f32(2 * reference)) if mode == 2 else 0.
                    if patch_value != bits(correct): raise AssertionError('native hook delta disagrees with clock/cap')
                    put(update, 'Iffff', 0, correct if variant == 'oracle' else reference, 0., 0., 0.)
                    before_raw = bytes(uc.mem_read(raw, 32))
                    before_update = bytes(uc.mem_read(update, 20))
                    call(0x578b50, repeater, update, zero, 0)
                    if consumer == 0x164930: call(consumer, callback, repeater)
                    else: call(consumer, repeater)
                    if bytes(uc.mem_read(raw, 32)) != before_raw or bytes(uc.mem_read(update, 20)) != before_update:
                        raise AssertionError('selected code mutated borrowed input/update fixture')
                    state_hashes.append(hashlib.sha256(bytes(uc.mem_read(repeater, 0x400))).hexdigest())
                    previous = held
                variants[variant] = dict(events=list(events), state_hashes=state_hashes)
            if variants['patched'] != variants['oracle']:
                raise AssertionError(f"{row['name']} consumer {consumer:#x}: patched state/events differ from elapsed oracle")
            if row['name'] == '30fps-hold' and variants['patched'] != variants['legacy']:
                raise AssertionError('nominal original 30 FPS state/events changed')
            if row['name'].endswith('fps-taps'):
                if [event['frame'] for event in variants['patched']['events']] != [2, 5, 9]:
                    raise AssertionError('tap/release/repress edges changed')
            if row['name'] == '165fps-hold' and len(variants['patched']['events']) >= len(variants['legacy']['events']):
                raise AssertionError('high-FPS repeat acceleration did not decrease')
            results.append(dict(name=row['name'], consumer=hex(BASE + consumer), frames=len(row['times']),
                                elapsed=elapsed_total, oracle_exact=True,
                                legacy_events=variants['legacy']['events'], patched_events=variants['patched']['events'],
                                state_sequence_sha256=hashlib.sha256(''.join(variants['patched']['state_hashes']).encode()).hexdigest()))
    if reached != set(sites):
        raise AssertionError(f'unexercised timer loads: {sorted(set(sites) - reached)}')
    return results, sorted(map(hex, reached))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--main', required=True, type=Path, help='selected extracted DE 2.0.0 exefs/main (read only)')
    parser.add_argument('--output', type=Path, help='fresh directory under Monado_Linux/runs')
    args = parser.parse_args()
    if sys.platform != 'linux' or platform.machine() != 'x86_64': parser.error('native Linux x86-64 required')
    main_path = args.main.resolve(strict=True)
    if digest(main_path) != MAIN_SHA256: parser.error('selected main differs from verified DE 2.0.0')
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0)); os.umask(0o077)
    runs = ROOT / 'runs'
    if runs.is_symlink(): parser.error('runs must not be a symlink')
    runs.mkdir(exist_ok=True)
    if args.output:
        output = args.output.absolute()
        if output != output.resolve() or not output.is_relative_to(runs) or output.exists():
            parser.error('--output must be fresh and non-symlink under Monado_Linux/runs')
        output.mkdir(parents=True)
    else: output = Path(tempfile.mkdtemp(prefix='xcde-input-repeat.', dir=runs))
    sys.path.insert(0, str(ROOT / 'tools/lifter'))
    import unicorn
    from gen import Module
    from patchfmt import PatchMismatch, load_patches
    receipt = dict(status='running', scope=__doc__, cwd=str(ROOT), argv=sys.argv,
                   input_path=str(main_path), input_sha256=digest(main_path), host=list(platform.uname()),
                   python=sys.version, unicorn=unicorn.__version__, sources_before=snapshot(), commands=[],
                   limitations=['Synthetic raw pad and UpdateInfo; no HID, real game/UI/replay, saves or worker scheduling.',
                                '25b90 raw-pad pointer, bounded memset, single-thread C++ guards and atan2f are adapters.',
                                '578ab0 records events instead of invoking real visual/game callbacks.',
                                'Exact selected updater/consumer behavior does not establish every title/menu/battle caller.',
                                'Unicorn is only a component oracle, never runtime game-CPU fallback.'])

    def save(): (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(name, argv, data=None):
        env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'TMPDIR': str(output)}
        command = dict(name=name, argv=list(map(str, argv)), cwd=str(ROOT), env=env, timeout_seconds=60)
        receipt['commands'].append(command); save()
        child = subprocess.Popen(command['argv'], cwd=ROOT, env=env, stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        try: stdout, stderr = child.communicate(data, timeout=60)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            os.killpg(child.pid, signal.SIGKILL); stdout, stderr = child.communicate(timeout=5)
            command['timed_out_or_interrupted'] = True
        (output / (name + '.stdout')).write_bytes(stdout); (output / (name + '.stderr')).write_bytes(stderr)
        command['exit'] = child.returncode
        if child.returncode or command.get('timed_out_or_interrupted'): raise RuntimeError(f'{name} failed; see {output}')
        return stdout

    save()
    try:
        module = Module('main', str(main_path), BASE, simd='c')
        patches = load_patches(str(ROOT / 'games/xcde/patches.py'))
        sites = sorted(addr for addr, patch in patches.items() if patch.req == 'LINUX-012')
        actual_loads = [addr for addr in range(BASE + 0x578b50, BASE + 0x5794bc, 4) if module.word(addr) == 0xbd400681]
        if len(sites) != 27 or sites != actual_loads: raise AssertionError('repeat patch set differs from all 27 reference loads')
        functions = []
        for index, address in enumerate(sites):
            functions.append(f'static void patch_{index}(Cpu *c) {{ {patches[address].apply(module, address)} }}')
            word = module.word
            module.word = lambda addr, word=word: word(addr) ^ 1
            try: patches[address].apply(module, address)
            except PatchMismatch: pass
            else: raise AssertionError(f'{address:#x}: mismatch guard accepted changed input')
            finally: module.word = word
        receipt.update(build_id=module.nso.build_id, sites=list(map(hex, sites)), all_mismatch_guards_passed=True)
        fixture = output / 'selected_repeat_hooks.c'
        fixture.write_text(NATIVE.replace('/*PATCH_FUNCTIONS*/', '\n'.join(functions))
                          .replace('/*PATCH_TABLE*/', ', '.join(f'patch_{i}' for i in range(len(sites)))))
        compiler = ROOT / '.deps/bin/clang-cc'; receipt['compiler_sha256'] = digest(compiler)
        run('compiler-version', [compiler, '--version'])
        binary = output / 'selected_repeat_hooks'
        run('compile', [compiler, '-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off',
                        '-fno-strict-aliasing', '-ffunction-sections', '-fdata-sections',
                        '-Wl,--gc-sections', '-I', ROOT, fixture, '-lm', '-o', binary])
        rows = scenarios()
        records = b''.join(struct.pack('<III', bits(dt), bits(1 / 30), mode)
                           for row in rows for dt, mode in zip(row['times'], row['modes'], strict=True))
        returned = run('native-checked-hooks', [binary], records)
        count = len(records) // 12
        if len(returned) != count * 27 * 4: raise AssertionError('native result count mismatch')
        native = []
        for offset in range(0, len(returned), 27 * 4):
            values = struct.unpack_from('<27I', returned, offset)
            if len(set(values)) != 1: raise AssertionError('patch sites disagree')
            native.append(values[0])
        receipt['results'], receipt['selected_loads_exercised'] = selected_runs(module.nso, sites, rows, native)
        receipt['native_patch_calls'] = count * 27
        receipt['sources_after'] = snapshot()
        if receipt['sources_after'] != receipt['sources_before']: raise RuntimeError('sources changed during verification')
        if digest(main_path) != receipt['input_sha256']: raise RuntimeError('selected input changed during verification')
        receipt['status'] = 'passed'
    except BaseException as error:
        receipt.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        receipt['artifacts'] = {p.name: digest(p) for p in sorted(output.iterdir()) if p.is_file() and p.name != 'receipt.json'}
        save()
    print(f"PASS {len(receipt['results'])} selected repeat cases; {receipt['native_patch_calls']} native patch calls; {output / 'receipt.json'}")


if __name__ == '__main__': main()
