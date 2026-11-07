#!/usr/bin/env python3
"""Optional passive foley trace component test; never launches the game.

Compile all six checked patches and the production helper with tracing enabled
and disabled. Synthetic guest objects test original instruction equivalence,
borrowed-memory preservation, filtering and the concurrent record budget.
Generated fixtures, binaries and trace output remain in ignored runs/.
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
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x7000000000
MAIN_SHA256 = '76ffa21b99c0385cfb14d1f90bbeddcdc98c17350b8edd79dfdbbb7341e70c48'
SITES = (0x3f4b90, 0x3f4c24, 0x466210, 0x482528, 0x4822ec, 0x3f3048)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_snapshot():
    pending = [Path(__file__), ROOT / 'runtime/hle/framepace.c', ROOT / 'games/xcde/patches.py']
    pending += list((ROOT / 'tools/lifter').glob('*.py'))
    found = {}
    while pending:
        path = pending.pop().resolve()
        name = str(path.relative_to(ROOT))
        if name in found:
            continue
        found[name] = digest(path)
        if path.suffix in ('.c', '.h', '.inc'):
            for include in re.findall(r'^\s*#\s*include\s+"([^"]+)"', path.read_text(), re.M):
                pending.append(path.parent / include)
    return dict(sorted(found.items()))


NATIVE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sys/mman.h>
#include "runtime/hle/framepace.c"
/*PATCH_FUNCTIONS*/
static void (*const patches[])(Cpu *) = { /*PATCH_TABLE*/ };
static void (*const originals[])(Cpu *) = { /*ORIGINAL_TABLE*/ };
enum { AREA_SIZE = 0x10000 };
static unsigned char *const area = (void *)0x7004000000ull;
static uint64_t *const got = (void *)0x7000b93458ull;
static const uint64_t component = 0x7004000000ull, marker = 0x7004001000ull;
static const uint64_t actor = 0x7004002000ull, slot = 0x7004003000ull, clock_object = 0x7004004000ull;
static unsigned char before[AREA_SIZE], got_before[4096];
uint64_t plat_now_ns(void) { return 123456789; }
static void p32(uint64_t p, uint32_t v) { memcpy((void *)(uintptr_t)p, &v, 4); }
static void p64(uint64_t p, uint64_t v) { memcpy((void *)(uintptr_t)p, &v, 8); }
static void pf(uint64_t p, float v) { memcpy((void *)(uintptr_t)p, &v, 4); }
static void setup(void) {
    assert(mmap(area, AREA_SIZE, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == area);
    void *page = (void *)0x7000b93000ull;
    assert(mmap(page, 4096, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == page);
    for (unsigned i = 0; i < AREA_SIZE; i++) area[i] = (unsigned char)(i * 19 + 7);
    p64(component + 0x38, actor); p32(component + 0x1c4, 0);
    p32(component + 0xc4, 7); pf(component + 0xc0, 0.25f); p32(component + 0x300, 6);
    p32(component + 0x114, 17); pf(component + 0x110, 0.75f); p32(component + 0x438, 16);
    p64(component + 0x78, (uint64_t)area + 0x9000); p64(component + 0x80, 4);
    for (unsigned layer = 0; layer < 2; layer++) {
        uint64_t state = component + 0xb0 + 0x50 * layer;
        uint64_t entries = (uint64_t)area + 0x5000 + 0x100 * layer;
        uint64_t control = (uint64_t)area + 0x6000 + 0x1000 * layer;
        *(uint8_t *)(uintptr_t)(state + 0x18) = 1 - layer;
        p64(state + 0x20, entries); p64(state + 0x28, 2 - layer);
        p64(entries + 0x30 * (1 - layer) + 0x18, control);
        pf(control + 0x80, 0.375f + 0.5f * layer); pf(control + 0x84, 0.75f);
        pf(control + 0x88, 1.25f); pf(control + 0x90, 0.8f); p32(control + 0x94, 2);
        p32(component + 0x2d0 + 0x138 * layer, layer);
        p32(component + 0x1d0 + 0x138 * layer, 2 + layer);
        p32(component + 0x1d4 + 0x138 * layer, 5 + layer);
        p32(component + 0x200 + 0x138 * layer + 0x78 * layer, 2 + layer);
        p64((uint64_t)area + 0x9000 + 0x20 * (2 + layer) + 8,
            layer ? control : (uint64_t)area + 0x8000);
    }
    pf((uint64_t)area + 0x8080, 0.625f); pf((uint64_t)area + 0x8088, 0.5f);
    p32(actor + 0x30, 2); p32(actor + 0x34, 123);
    p32(marker + 8, 12); p32(marker + 0xe, 0xc9);
    p64(slot, clock_object); pf(clock_object + 0x2034, 0.00625f); *got = slot;
}
static void freeze(void) {
    memcpy(before, area, sizeof before); memcpy(got_before, (void *)0x7000b93000ull, sizeof got_before);
    assert(mprotect(area, AREA_SIZE, PROT_READ) == 0);
    assert(mprotect((void *)0x7000b93000ull, 4096, PROT_READ) == 0);
}
static void thaw(void) {
    assert(!memcmp(before, area, sizeof before));
    assert(!memcmp(got_before, (void *)0x7000b93000ull, sizeof got_before));
    assert(mprotect(area, AREA_SIZE, PROT_READ | PROT_WRITE) == 0);
    assert(mprotect((void *)0x7000b93000ull, 4096, PROT_READ | PROT_WRITE) == 0);
}
static Cpu cpu_for(unsigned kind) {
    Cpu c;
    for (unsigned i = 0; i < sizeof c; i++) ((unsigned char *)&c)[i] = (unsigned char)(i * 11 + kind);
    c.x[0] = component; c.x[1] = kind == 4 ? marker : 0x10000005cull;
    c.x[19] = component; c.x[20] = marker;
    if (kind == 1) { c.x[19] = marker; c.x[20] = component; }
    c.x[8] = 0x100000000ull; c.x[24] = 0x200000001ull;
    return c;
}
static void check(unsigned kind, Cpu c) {
    Cpu expected = c;
    originals[kind](&expected); patches[kind](&c);
    assert(!memcmp(&c, &expected, sizeof c));
}
static pthread_barrier_t barrier;
static void *worker(void *unused) {
    (void)unused;
    Cpu c = cpu_for(3);
    int result = pthread_barrier_wait(&barrier);
    assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
    for (unsigned i = 0; i < 700; i++) check(3, c);
    return NULL;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "off")) {
        assert(mmap(area, AREA_SIZE, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == area);
        /* Neither the guest object nor the unmapped clock GOT may be read. */
        for (unsigned i = 0; i < 6; i++) check(i, cpu_for(i));
        xcde_foley_trace((Cpu *)(uintptr_t)1, 0);
        puts("PASS disabled: six checked patches, inaccessible guest objects and Cpu");
        return 0;
    }
    setup();
    if (!strcmp(argv[1], "fields")) {
        freeze();
        for (unsigned i = 0; i < 6; i++) check(i, cpu_for(i));
        thaw();
        for (unsigned layer = 1; layer <= 2; layer++) {
            p32(component + 0x1c4, layer); freeze(); check(0, cpu_for(0)); thaw();
        }
        p32(component + 0x1c4, UINT32_MAX); freeze(); check(1, cpu_for(1)); thaw();
        p32(component + 0x1c4, 0);
        const unsigned subtypes[] = {19, 20};
        for (unsigned i = 0; i < 2; i++) {
            *(unsigned char *)(uintptr_t)(marker + 0xa) = subtypes[i];
            freeze(); check(0, cpu_for(0)); check(1, cpu_for(1)); thaw();
        }
        for (unsigned subtype = 0; subtype < 4; subtype++) {
            *(unsigned char *)(uintptr_t)(marker + 0xa) = subtype == 3 ? 255 : subtype;
            freeze(); check(4, cpu_for(4)); thaw();
        }
        p64(component + 0x38, 0); *got = 0; freeze(); check(3, cpu_for(3)); thaw();
        p64(component + 0x38, actor); *got = slot; p64(slot, 0);
        freeze(); check(5, cpu_for(5)); thaw();
        p64(slot, clock_object);
        Cpu c = cpu_for(2); c.x[8] = UINT64_MAX; c.x[24] = 0;
        freeze(); check(2, c); thaw();
        const unsigned ids[] = {0, 0xe7, UINT32_MAX};
        for (unsigned i = 0; i < 3; i++) {
            c = cpu_for(3); c.x[1] = ids[i]; freeze(); check(3, c); thaw();
        }
    } else if (!strcmp(argv[1], "controls")) {
        /* Each absent/out-of-range link must stop before an inaccessible pointer. */
        p64(component + 0xd0, 0); freeze(); check(0, cpu_for(0)); thaw();
        p64(component + 0xd0, 1); p64(component + 0xd8, 1);
        freeze(); check(0, cpu_for(0)); thaw();
        p64(component + 0xd0, (uint64_t)area + 0x5000); p64(component + 0xd8, 2);
        p64((uint64_t)area + 0x5048, 0); freeze(); check(0, cpu_for(0)); thaw();
        p32(component + 0x2d0, UINT32_MAX); freeze(); check(0, cpu_for(0)); thaw();
        p32(component + 0x2d0, 0); p64(component + 0x78, 0);
        freeze(); check(0, cpu_for(0)); thaw();
        p64(component + 0x78, 1); p64(component + 0x80, 2);
        freeze(); check(0, cpu_for(0)); thaw();
        p64(component + 0x78, (uint64_t)area + 0x9000);
        p64(component + 0x80, 4); p64((uint64_t)area + 0x9048, 0);
        freeze(); check(0, cpu_for(0)); thaw();
        p32(component + 0x200, UINT32_MAX); p64(component + 0x80, UINT64_MAX);
        p64(component + 0x78, 1); freeze(); check(0, cpu_for(0)); thaw();
    } else if (!strcmp(argv[1], "filters")) {
        *(unsigned char *)(uintptr_t)(marker + 0xa) = 18;
        freeze();
        for (unsigned i = 0; i < 5000; i++) {
            check(i % 2, cpu_for(i % 2));
            Cpu c = cpu_for(2); c.x[8] = 1; c.x[24] = 0x100000001ull;
            check(2, c);
        }
        for (unsigned i = 0; i < 6; i++) {
            Cpu c = cpu_for(i);
            if (i == 1) c.x[20] = 0;
            else if (i < 3) c.x[19] = 0;
            else c.x[0] = 0;
            check(i, c);
        }
        for (unsigned i = 0; i < 6; i++) if (i == 0 || i == 1 || i == 4) {
            Cpu c = cpu_for(i);
            c.x[i == 0 ? 20 : i == 1 ? 19 : 1] = 0;
            check(i, c);
        }
        xcde_foley_trace(NULL, 0); xcde_foley_trace((Cpu *)(uintptr_t)1, 99);
        check(3, cpu_for(3));
        thaw();
    } else if (!strcmp(argv[1], "budget")) {
        freeze();
        for (unsigned i = 0; i < 5000; i++) check(3, cpu_for(3));
        thaw();
        assert(mprotect(area, AREA_SIZE, PROT_NONE) == 0);
        assert(mprotect((void *)0x7000b93000ull, 4096, PROT_NONE) == 0);
        xcde_foley_trace((Cpu *)(uintptr_t)1, 0);
        for (unsigned i = 0; i < 6; i++) check(i, cpu_for(i));
    } else if (!strcmp(argv[1], "concurrent")) {
        freeze();
        pthread_t threads[8];
        assert(pthread_barrier_init(&barrier, NULL, 8) == 0);
        for (unsigned i = 0; i < 8; i++) assert(pthread_create(&threads[i], NULL, worker, NULL) == 0);
        for (unsigned i = 0; i < 8; i++) assert(pthread_join(threads[i], NULL) == 0);
        assert(pthread_barrier_destroy(&barrier) == 0);
        thaw();
    } else {
        assert(!"unknown scenario");
    }
    puts("PASS original Cpu operations and read-only borrowed memory");
    return 0;
}
'''


def verify_scenarios(run, binaries):
    results = []
    for enabled, scenario in ((0, 'off'), (0, 'fields'), (1, 'fields'), (1, 'controls'), (1, 'filters'),
                              (1, 'budget'), (1, 'concurrent')):
        stdout, stderr = run(f'run-{enabled}-{scenario}', [binaries[enabled], scenario])
        if not stdout.startswith('PASS ') or not stdout.endswith('\n'):
            raise AssertionError('native fixture did not report successful preservation checks')
        records = []
        for line in stderr.splitlines():
            if not line.startswith('[foley] '):
                raise AssertionError(f'unexpected native diagnostic: {line}')
            fields = line.removeprefix('[foley] ').split()
            record = dict(field.split('=', 1) for field in fields)
            if len(record) != len(fields):
                raise AssertionError('duplicate trace field')
            records.append(record)
        if enabled == 0:
            if records:
                raise AssertionError('disabled tracing emitted records')
        else:
            numbers = [int(r['n']) for r in records]
            if sorted(numbers) != list(range(1, len(records) + 1)):
                raise AssertionError('trace sequence IDs are missing or duplicated')
            if any(r['t_ns'] != '123456789' for r in records):
                raise AssertionError('trace lost host timestamp')
            for index, r in enumerate(records):
                absent_actor = scenario == 'fields' and index == 17
                absent_clock = scenario == 'fields' and index in (17, 18)
                expected_actor = '0' if absent_actor else '7004002000'
                if r['comp'] != '7004000000' or r['actor'] != expected_actor:
                    raise AssertionError('wrong component or actor pointer')
                if r['actor_id'] != ('0' if absent_actor else '123'):
                    raise AssertionError('wrong actor ID')
                if r['actor_type'] != ('0' if absent_actor else '2'):
                    raise AssertionError('wrong actor type')
                expected_dt = '0' if absent_clock else '0.00625000009'
                if r['dt'] != expected_dt:
                    raise AssertionError('wrong engine elapsed value for fixture clock')
                if r['kind'] in ('0', '1', '4') and r['marker'] != '7004001000':
                    raise AssertionError('wrong marker pointer')
            if scenario in ('budget', 'concurrent'):
                if len(records) != 4096 or {r['kind'] for r in records} != {'3'}:
                    raise AssertionError('trace budget or concurrent records incorrect')
            elif scenario == 'filters':
                if len(records) != 1 or records[0]['kind'] != '3' or records[0]['sound_id'] != '92':
                    raise AssertionError('filtered calls consumed budget or emitted records')
            elif scenario == 'fields':
                if len(records) != 23 or [int(r['kind']) for r in records[:6]] != list(range(6)):
                    raise AssertionError('field-case record count/order incorrect')
                for r in records[:2]:
                    if {k: r[k] for k in ('frame', 'subtype', 'layer', 'layer_valid', 'generation',
                                          'stored_cursor', 'seen_generation')} != dict(
                            frame='12', subtype='0', layer='0', layer_valid='1', generation='7',
                            stored_cursor='0.25', seen_generation='6'):
                        raise AssertionError('marker fields incorrect')
                    expected = dict(action='2', transition='5', parameter='0', motion='2', selected='1', controls='2',
                                    control='7004006000', cursor='0.375', rate='1.25', weight='0.75',
                                    duration='0.800000012', flags='2', gate_control='7004008000',
                                    gate_cursor='0.625', gate_rate='0.5')
                    if any(r.get(k) != v for k, v in expected.items()):
                        raise AssertionError('selected/gate control fields incorrect')
                if records[2]['prior'] != '0' or records[2]['current'] != '1':
                    raise AssertionError('contact fields were not read before CMP')
                if records[3]['sound_id'] != '92' or records[4]['sound_id'] != '201' or records[5]['mode'] != '92':
                    raise AssertionError('surface/authored/reset fields incorrect')
                if records[6]['generation'] != '17' or records[6]['stored_cursor'] != '0.75' or records[6]['seen_generation'] != '16':
                    raise AssertionError('layer 1 offsets incorrect')
                expected = dict(action='3', transition='6', parameter='1', motion='3', selected='0', controls='1',
                                control='7004007000', cursor='0.875', gate_control='7004007000',
                                gate_cursor='0.875', gate_rate='1.25')
                if any(records[6].get(k) != v for k, v in expected.items()):
                    raise AssertionError('layer 1 control offsets incorrect')
                for r in records[7:9]:
                    if any(r[k] != '0' for k in ('layer_valid', 'generation', 'stored_cursor', 'seen_generation',
                                                'parameter', 'motion', 'control', 'gate_control', 'duration')):
                        raise AssertionError('invalid layer read timing fields')
                if [r['subtype'] for r in records[9:13]] != ['19', '19', '20', '20']:
                    raise AssertionError('terrain subtypes not retained')
                if [r['subtype'] for r in records[13:17]] != ['0', '1', '2', '255']:
                    raise AssertionError('authored sound subtypes wrongly filtered')
                if any(records[i]['dt'] != '0' for i in (17, 18)) or records[17]['actor'] != '0':
                    raise AssertionError('null actor/clock handling incorrect')
                if records[19]['prior'] != str(2**32 - 1) or records[19]['current'] != '0':
                    raise AssertionError('contact register widths incorrect')
                if [r['sound_id'] for r in records[20:]] != ['0', '231', str(2**32 - 1)]:
                    raise AssertionError('sound IDs were suppressed or truncated')
            elif scenario == 'controls':
                if len(records) != 8 or {r['kind'] for r in records} != {'0'}:
                    raise AssertionError('control boundary record count/kind incorrect')
                for r in records:
                    if any(r.get(k) != '0' for k in ('control', 'cursor', 'rate', 'weight', 'duration', 'flags')):
                        raise AssertionError('absent/out-of-bounds selected control was read')
                expected = dict(gate_control='7004008000', gate_cursor='0.625', gate_rate='0.5')
                for r in records[:3]:
                    if any(r.get(k) != v for k, v in expected.items()):
                        raise AssertionError('absent selected control lost independent gate lookup')
                for r in records[3:]:
                    if any(r.get(k) != '0' for k in ('gate_control', 'gate_cursor', 'gate_rate')):
                        raise AssertionError('absent/out-of-bounds gate control was read')
        results.append(dict(enabled=enabled, scenario=scenario, records=len(records),
                            cpu_and_borrowed_memory_preserved=True,
                            consecutive_unique_sequence_ids=True if enabled else None))
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--main', required=True, type=Path, help='selected extracted DE 2.0.0 main (read only)')
    parser.add_argument('--output', type=Path, help='fresh directory beneath Monado_Linux/runs')
    args = parser.parse_args()
    if sys.platform != 'linux' or platform.machine() != 'x86_64':
        parser.error('native Linux x86-64 required')
    selected = args.main.resolve(strict=True)
    if digest(selected) != MAIN_SHA256:
        parser.error('selected main differs from verified DE 2.0.0')
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    os.umask(0o077)
    runs = ROOT / 'runs'
    if runs.is_symlink():
        parser.error('runs must not be a symlink')
    runs.mkdir(exist_ok=True)
    if args.output:
        output = args.output.absolute()
        if output != output.resolve() or not output.is_relative_to(runs) or output.exists():
            parser.error('--output must be fresh and non-symlink beneath Monado_Linux/runs')
        output.mkdir(parents=True)
    else:
        output = Path(tempfile.mkdtemp(prefix='xcde-foley-trace.', dir=runs))
    sys.path.insert(0, str(ROOT / 'tools/lifter'))
    from gen import Module
    from patchfmt import PatchMismatch, load_patches
    receipt = dict(status='running', scope=__doc__, cwd=str(ROOT), argv=sys.argv,
                   input_path=str(selected), input_sha256=digest(selected), host=list(platform.uname()),
                   python=sys.version, sources_before=source_snapshot(), commands=[],
                   limitations=['Synthetic guest objects and clock, no game/UI/replay, saves or real audio.',
                                'Tests passive instrumentation only; not a correction for accelerated audio.',
                                'Native original-instruction oracle uses the same selected portable lifter.',
                                'Concurrent tests validate bounded record allocation, not game-thread scheduling.'])

    def save():
        (output / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(name, argv):
        command = dict(name=name, argv=list(map(str, argv)), cwd=str(ROOT), timeout_seconds=60,
                       env={'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'TMPDIR': str(output)})
        receipt['commands'].append(command)
        save()
        process = subprocess.Popen(command['argv'], cwd=ROOT, env=command['env'], stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        try:
            stdout, stderr = process.communicate(timeout=60)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            os.killpg(process.pid, signal.SIGKILL)
            stdout, stderr = process.communicate(timeout=5)
            command['timed_out_or_interrupted'] = True
        (output / (name + '.stdout')).write_bytes(stdout)
        (output / (name + '.stderr')).write_bytes(stderr)
        command['exit'] = process.returncode
        if process.returncode or command.get('timed_out_or_interrupted'):
            raise RuntimeError(f'{name} failed; see {output}')
        return stdout.decode(), stderr.decode()

    save()
    try:
        module = Module('main', str(selected), BASE, simd='c')
        patches = load_patches(str(ROOT / 'games/xcde/patches.py'))
        sites = [BASE + offset for offset in SITES]
        # LINUX-013 also owns the independent phase-seek correction; this
        # preservation fixture covers only the six passive trace callsites.
        import runpy
        trace_sites = runpy.run_path(str(ROOT / 'games/xcde/patches.py'))['FOLEY_TRACE_SITES']
        if set(sites) != set(trace_sites):
            raise AssertionError('foley patch set differs from six audited sites')
        functions = []
        for index, address in enumerate(sites):
            emitted = patches[address].apply(module, address)
            original = module.lift(address)
            if original.kind != 'op':
                raise AssertionError('original instruction is not a plain operation')
            functions += [f'static void patch_{index}(Cpu *c) {{ {emitted} }}',
                          f'static void original_{index}(Cpu *c) {{ {original.code} }}']
            saved_word = module.word
            module.word = lambda addr, saved_word=saved_word: saved_word(addr) ^ 1
            try:
                patches[address].apply(module, address)
            except PatchMismatch:
                pass
            else:
                raise AssertionError(f'{address:#x}: changed input passed mismatch guard')
            finally:
                module.word = saved_word
        receipt.update(build_id=module.nso.build_id, sites=list(map(hex, sites)), all_mismatch_guards_passed=True)
        fixture = output / 'selected_foley_hooks.c'
        fixture.write_text(NATIVE.replace('/*PATCH_FUNCTIONS*/', '\n'.join(functions))
                          .replace('/*PATCH_TABLE*/', ', '.join(f'patch_{i}' for i in range(6)))
                          .replace('/*ORIGINAL_TABLE*/', ', '.join(f'original_{i}' for i in range(6))))
        compiler = ROOT / '.deps/bin/clang-cc'
        receipt['compiler_sha256'] = digest(compiler)
        run('compiler-version', [compiler, '--version'])
        binaries = {}
        for enabled in (0, 1):
            binary = output / f'foley-trace-{enabled}'
            run(f'compile-{enabled}', [compiler, '-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off',
                                      '-fno-strict-aliasing', '-ffunction-sections', '-fdata-sections',
                                      f'-DXCDE_FOLEY_TRACE={enabled}', '-Wl,--gc-sections', '-I', ROOT,
                                      fixture, '-pthread', '-lm', '-o', binary])
            binaries[enabled] = binary
        receipt['results'] = verify_scenarios(run, binaries)
        receipt['sources_after'] = source_snapshot()
        if receipt['sources_after'] != receipt['sources_before'] or digest(selected) != receipt['input_sha256']:
            raise RuntimeError('source or selected input changed during verification')
        receipt['status'] = 'passed'
    except BaseException as error:
        receipt.update(status='failed', error=f'{type(error).__name__}: {error}')
        raise
    finally:
        receipt['artifacts'] = {p.name: digest(p) for p in sorted(output.iterdir())
                                if p.is_file() and p.name != 'receipt.json'}
        save()
    print(f"PASS {len(receipt['results'])} passive trace scenarios; {output / 'receipt.json'}")


if __name__ == '__main__':
    main()
