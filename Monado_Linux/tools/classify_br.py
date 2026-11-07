"""Research tool (T020): classify indirect `br` sites of a module by what the jump-table resolver does with them.

Usage: classify_br.py <module> [--show N]     (needs generated/<module>/<module>_table.c and targets/exefs)
Writes counts and the most common instruction-shape signatures of unresolved sites to stdout.
"""
import argparse
import bisect
import collections
import json
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'lifter'))
import capstone  # noqa: E402
import gen  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('module')
    ap.add_argument('--show', type=int, default=6)
    ap.add_argument('--back', type=int, default=12)
    a = ap.parse_args()
    prof = json.load(open(os.path.join(ROOT, 'games', 'xcde', 'profile.json')))
    mod = next(m for m in prof['modules'] if m['name'] == a.module)
    base = int(mod['base'], 16)
    m = gen.Module(a.module, os.path.join(ROOT, 'targets', 'exefs', a.module), base, 'c')
    txt = open(os.path.join(ROOT, 'generated', a.module, a.module + '_table.c')).read()
    body = re.search(r'_fn_addrs\[\] = \{(.*?)\};', txt, re.S).group(1)
    addrs = sorted(int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', body))
    cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    res = collections.Counter()
    unresolved = []
    for i, w in enumerate(m.words):
        if (w & 0xFFFFFC1F) != 0xD61F0000:
            continue
        pc = m.lo + i * 4
        k = bisect.bisect_right(addrs, pc) - 1
        entry = addrs[k]
        end = addrs[k + 1] if k + 1 < len(addrs) else m.hi
        jt = m.jump_table(pc, entry, end)
        if jt:
            res['resolved'] += 1
        elif m.computed_target(pc, entry):
            res['computed_unresolved'] += 1
            unresolved.append((pc, entry, end))
        else:
            res['non_table (load/tail call)'] += 1
    print(dict(res))

    def shape(pc, entry):
        ops = []
        for q in range(pc - 4 * a.back, pc + 4, 4):
            if q < entry:
                continue
            ins = next(cs.disasm(bytes(m.words[(q - m.lo) // 4].to_bytes(4, 'little')), q), None)
            if ins:
                ops.append(ins.mnemonic)
        return ' '.join(ops[-8:])
    sig = collections.Counter(shape(pc, e) for pc, e, _ in unresolved)
    print('top shapes of unresolved computed jumps (last 8 mnemonics before br):')
    for s, n in sig.most_common(12):
        print(f'{n:5}  {s}')
    print()
    for pc, entry, end in unresolved[:a.show]:
        print(f'--- br at {pc:#x} (function {entry:#x}..{end:#x}, {(end - entry) // 4} insns)')
        for q in range(max(entry, pc - 4 * a.back), pc + 4, 4):
            ins = next(cs.disasm(m.words[(q - m.lo) // 4].to_bytes(4, 'little'), q), None)
            print(f'  {q:#x}: {ins.mnemonic} {ins.op_str}' if ins else f'  {q:#x}: ??')


if __name__ == '__main__':
    main()
