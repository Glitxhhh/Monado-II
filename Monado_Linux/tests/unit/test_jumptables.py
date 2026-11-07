"""T021 (tests first): jump-table resolution on synthetic code (no game data).

The pattern is the byte-offset table clang emits for dense switches:
    cmp w8,#N ; b.hi default ; adrp/add x9 -> table ; adr x10 -> cases ; ldrb w11,[x9,x8] ; add x10,x10,x11,lsl #2 ; br x10
The `adr` that holds the cases' base must NOT start a new function (research R4): doing so split the function
right before its cases and left the resolver with targets outside the function.
"""
import os
import sys
import unittest

LIFTER = os.path.join(os.path.dirname(__file__), '..', '..', 'tools', 'lifter')
sys.path.insert(0, LIFTER)
try:
    import gen
    from lifter import Lifter
    HAVE = True
except Exception:  # unicorn / lz4 / capstone not installed
    HAVE = False

BASE = 0x7000000000
NOP, RET = 0xD503201F, 0xD65F03C0


def adr(rd, pc, target):
    imm = target - pc
    return 0x10000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7FFFF) << 5) | rd


def adrp(rd, pc, target):
    imm = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12
    return 0x90000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7FFFF) << 5) | rd


def add_imm(rd, rn, imm12):
    return 0x91000000 | (imm12 << 10) | (rn << 5) | rd


def ldrb_reg(rt, rn, rm):
    return 0x38606800 | (rm << 16) | (rn << 5) | rt


def ldrh_reg(rt, rn, rm):  # ldrh w, [xn, xm, lsl #1]
    return 0x78607800 | (rm << 16) | (rn << 5) | rt


def add_lsl(rd, rn, rm, sh):
    return 0x8B000000 | (rm << 16) | (sh << 10) | (rn << 5) | rd


def br(rn):
    return 0xD61F0000 | (rn << 5)


def cmp_w(rn, n):
    return 0x7100001F | (n << 10) | (rn << 5)


def b_hi(from_i, to_i):
    return 0x54000008 | (((to_i - from_i) & 0x7FFFF) << 5)


def ldr_sp(rt, off):  # ldr xt, [sp, #off]
    return 0xF9400000 | ((off // 8) << 10) | (31 << 5) | rt


def and_ff(rd, rn):  # and xd, xn, #0xff
    return 0x92401C00 | (rn << 5) | rd


class FakeNso:
    def __init__(self, image):
        self.image = image


def make_module(words, table_off, table_bytes, size=0x3000):
    image = bytearray(size)
    for i, w in enumerate(words):
        image[i * 4:i * 4 + 4] = w.to_bytes(4, 'little')
    image[table_off:table_off + len(table_bytes)] = table_bytes
    m = object.__new__(gen.Module)
    m.name, m.base = 't', BASE
    m.nso = FakeNso(image)
    m.lo, m.hi = BASE, BASE + len(words) * 4
    m.words = tuple(words)
    m.lifter = Lifter(BASE, 'c')
    m.cache, m.mu = {}, None
    from collections import Counter
    m.stats = Counter()
    return m


def byte_table_function(wide=False):
    """Function with 3 cases reached through a byte (or halfword) offset table; returns (words, table_off, table)."""
    table_off = 0x2000
    tbl_addr = BASE + table_off
    W = []
    W.append(cmp_w(8, 2))                                   # 0  cmp w8,#2
    W.append(b_hi(1, 14))                                   # 1  b.hi default (idx 14)
    W.append(adrp(9, BASE + 2 * 4, tbl_addr))               # 2
    W.append(add_imm(9, 9, tbl_addr & 0xFFF))               # 3
    W.append(adr(10, BASE + 4 * 4, BASE + 8 * 4))           # 4  adr x10, cases (idx 8)
    W.append((ldrh_reg if wide else ldrb_reg)(11, 9, 8))    # 5
    W.append(add_lsl(10, 10, 11, 2))                        # 6
    W.append(br(10))                                        # 7
    for c in range(3):                                      # 8..13 : cases
        W += [NOP, RET]
    W.append(RET)                                           # 14 default
    # cases at words 8,10,12 -> offsets 0,2,4 (in words) from idx 8
    table = bytes([0, 2, 4]) if not wide else (0).to_bytes(2, 'little') + (2).to_bytes(2, 'little') + (4).to_bytes(2, 'little')
    return W, table_off, table


def reloaded_index_function():
    """T141: the bounds check tests one copy of the index and the lookup reloads another from a stack slot
    (XCDE main 0x70005264b4: cmp w8,#3 ; b.hi ; ldr x8,[sp,#8] ; and x8,x8,#0xff ; ...). Emulating with only the
    compared register set gave one garbage target and the table was taken as resolved (crash in the first battle)."""
    table_off = 0x2000
    tbl_addr = BASE + table_off
    W = []
    W.append(cmp_w(8, 2))                                   # 0  cmp w8,#2
    W.append(b_hi(1, 16))                                   # 1  b.hi default (idx 16)
    W.append(ldr_sp(8, 8))                                  # 2  ldr x8,[sp,#8]   (the index, reloaded)
    W.append(and_ff(8, 8))                                  # 3  and x8,x8,#0xff
    W.append(adrp(9, BASE + 4 * 4, tbl_addr))               # 4
    W.append(add_imm(9, 9, tbl_addr & 0xFFF))               # 5
    W.append(adr(10, BASE + 6 * 4, BASE + 10 * 4))          # 6  adr x10, cases (idx 10)
    W.append(ldrb_reg(11, 9, 8))                            # 7
    W.append(add_lsl(10, 10, 11, 2))                        # 8
    W.append(br(10))                                        # 9
    for c in range(3):                                      # 10..15 : cases
        W += [NOP, RET]
    W.append(RET)                                           # 16 default
    return W, table_off, bytes([0, 2, 4])


@unittest.skipUnless(HAVE, 'generator dependencies (unicorn, lz4, capstone) not installed')
class TestJumpTables(unittest.TestCase):
    def test_index_reloaded_after_the_bounds_check(self):
        W, off, tbl = reloaded_index_function()
        m = make_module(W, off, tbl)
        got = m.jump_table(BASE + 9 * 4, BASE, BASE + 17 * 4)
        self.assertEqual(got, [BASE + 10 * 4, BASE + 12 * 4, BASE + 14 * 4])

    def test_single_target_for_a_multi_entry_table_is_not_trusted(self):
        # an index the emulation cannot follow (from a register no one sets) must not "resolve" to one case
        W, off, tbl = reloaded_index_function()
        W[2] = NOP                       # x8 is never reloaded...
        W[3] = and_ff(8, 7)              # ...the lookup index comes from x7 instead
        m = make_module(W, off, tbl)
        got = m.jump_table(BASE + 9 * 4, BASE, BASE + 17 * 4)
        self.assertIsNone(got)

    def test_resolves_byte_table(self):
        W, off, tbl = byte_table_function()
        m = make_module(W, off, tbl)
        got = m.jump_table(BASE + 7 * 4, BASE, BASE + 15 * 4)
        self.assertEqual(got, [BASE + 8 * 4, BASE + 10 * 4, BASE + 12 * 4])

    def test_resolves_halfword_table(self):
        W, off, tbl = byte_table_function(wide=True)
        m = make_module(W, off, tbl)
        got = m.jump_table(BASE + 7 * 4, BASE, BASE + 15 * 4)
        self.assertEqual(got, [BASE + 8 * 4, BASE + 10 * 4, BASE + 12 * 4])

    def test_table_base_adr_is_not_a_function_start(self):
        W, off, tbl = byte_table_function()
        starts = gen.adr_targets(W, BASE, lambda a: BASE <= a < BASE + len(W) * 4 and a % 4 == 0)
        self.assertEqual(starts, set())

    def test_plain_adr_of_code_still_a_function_start(self):
        # adr x0, <function>  not followed by a br: a callback address, must stay a function start
        W = [adr(0, BASE, BASE + 6 * 4), NOP, NOP, NOP, NOP, RET, NOP, RET]
        starts = gen.adr_targets(W, BASE, lambda a: BASE <= a < BASE + len(W) * 4 and a % 4 == 0)
        self.assertEqual(starts, {BASE + 6 * 4})

    def test_scan_keeps_cases_inside_the_function(self):
        W, off, tbl = byte_table_function()
        m = make_module(W, off, tbl)
        labels, ext, tables = m.scan(BASE, BASE + 15 * 4)
        self.assertIn(BASE + 7 * 4, tables)
        self.assertEqual(tables[BASE + 7 * 4], [BASE + 8 * 4, BASE + 10 * 4, BASE + 12 * 4])

    def _emit(self, words, off, tbl):
        m = make_module(words, off, tbl)
        end = BASE + len(words) * 4
        labels, ext, tables = m.scan(BASE, end)
        return chr(10).join(m.emit_function(BASE, end, labels, tables, {}))

    def test_resolved_table_has_no_all_instruction_fallback(self):
        W, off, tbl = byte_table_function()
        code = self._emit(W, off, tbl)
        self.assertIn('switch (', code)
        self.assertNotIn('L_dispatch', code)
        # only the table's targets (+ branch targets) get labels, not every instruction
        self.assertNotIn('L_7000000014:', code)

    def test_unresolved_table_keeps_the_safe_fallback(self):
        W, off, tbl = byte_table_function()
        W[0] = NOP   # no cmp/b.hi guard: the resolver cannot bound the table
        W[1] = NOP
        code = self._emit(W, off, tbl)
        self.assertIn('L_dispatch', code)
        self.assertIn('L_7000000014:', code)

    def test_resolved_switch_still_reaches_every_case(self):
        W, off, tbl = byte_table_function()
        code = self._emit(W, off, tbl)
        for i in (8, 10, 12):
            self.assertIn(f'case {BASE + i * 4:#x}ull: goto L_{BASE + i * 4:x};'.replace('ull', ''), code.replace('ull', ''))


if __name__ == '__main__':
    unittest.main()
