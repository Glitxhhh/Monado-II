"""XCDE instruction patches (contract: docs/contracts/patch-format.md)."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "lifter"))
from patchfmt import Patch  # noqa: E402,F401


def _hook_then_original(fn):
    """Call a runtime hook, then the original instruction (which must lift to plain C)."""
    def emit(gen, addr):
        ins = gen.lift(addr)
        if ins.kind != 'op':
            raise ValueError(f'{addr:#x}: hook patch needs a plain instruction, got {ins.kind}')
        return f'{{ extern void {fn}(void); {fn}(); }} {ins.code}'
    return emit


# T157: the game's frame limiter (nnMain+0x12150, f_70005bcfe0) busy-waits for the end of the 33.3 ms frame: a loop
# that reads the system tick through a vtable until enough time passed, no sleep (a dedicated core on the Switch). On
# PC that spin was ~0.3 of a core. Hooks: the loop head (sleep while the frame end is still far, runtime/hle/framepace.c)
# and the function's exit (a frame boundary: learns the frame interval).
PATCHES = {
    0x70005bd068: Patch(0x320003e0, _hook_then_original('xcde_limiter_spin'), 'T157', 'mov w0, #1 - limiter loop head'),
    0x70005bd168: Patch(0x6d4123e9, _hook_then_original('xcde_limiter_done'), 'T157', 'ldp d9, d8, [sp, #0x10] - limiter exit'),
}


# [XCDE] LINUX-011: the GUI update callback precedes animation traversal. Replace
# its fixed constructor timestep with the existing engine frame delta; leave the
# callback's registers/instruction and all GUI/node speed and pause logic intact.
# This is intentionally separate from world, skeletal and cutscene clocks.
def _gui_frame_delta(gen, addr):
    ins = gen.lift(addr)
    if ins.kind != 'op':
        raise ValueError(f'{addr:#x}: GUI timing hook needs a plain instruction')
    return ('{ extern void xcde_gui_frame_delta(uint64_t gui); '
            'xcde_gui_frame_delta(c->x[0]); } ' + ins.code)


PATCHES[0x700074bfc0] = Patch(0xa9412001, _gui_frame_delta, 'LINUX-011',
                            'ldp x1, x8, [x0, #0x10] - GUI update entry')


# [XCDE] LINUX-012: all reference-period reads that advance held durations in
# the shared navigation updater. The repeat query already uses seconds; the
# original UpdateInfo+4 is a fixed 1/30 reference, not elapsed UpdateInfo+8.
# Retain the original load (including scalar upper-bit clearing), then replace
# just its result. No changes to raw input, edges, timer resets or repeat policy.
def _repeat_frame_delta(gen, addr):
    ins = gen.lift(addr)
    if ins.kind != 'op':
        raise ValueError(f'{addr:#x}: repeat timing hook needs a plain instruction')
    return (ins.code + ' { extern uint32_t xcde_repeat_delta_bits(uint32_t reference); '
            'c->v[1] = xcde_repeat_delta_bits((uint32_t)c->v[1]); }')


REPEAT_STEP_SITES = (
    0x7000578ebc, 0x7000578eec, 0x7000578f18, 0x7000578f44, 0x7000578f70,
    0x7000578f9c, 0x7000578fc8, 0x7000578ff4, 0x7000579020, 0x700057904c,
    0x7000579078, 0x70005790a4, 0x70005790d0, 0x70005790fc, 0x7000579128,
    0x7000579154, 0x7000579180, 0x70005791ac, 0x70005791d8, 0x7000579204,
    0x7000579230, 0x700057925c, 0x7000579288, 0x70005792b4, 0x70005792e0,
    0x7000579308, 0x70005793b4,
)
for _site in REPEAT_STEP_SITES:
    PATCHES[_site] = Patch(0xbd400681, _repeat_frame_delta, 'LINUX-012',
                           'ldr s1, [x20, #4] - navigation held-duration step')


# [XCDE] LINUX-014: view-only navigation repeat preference. Keep the actual held
# timers in seconds; only these repeat consumers see an optional doubled view.
# Retain every scalar load and its upper-lane clearing, without changing raw
# input, non-repeat hold queries, or the shared shoulder/button repeat branches.
def _navigation_duration(gen, addr):
    ins = gen.lift(addr)
    if ins.kind != 'op':
        raise ValueError(f'{addr:#x}: navigation repeat view needs a scalar load')
    reg = gen.word(addr) & 31
    return (ins.code + ' { extern uint32_t xcde_navigation_duration_bits(uint32_t duration); '
            f'c->v[{reg}] = xcde_navigation_duration_bits((uint32_t)c->v[{reg}]); }}')


NAVIGATION_DURATION_SITES = {
    0x7000164978: 0xbd4017e0, 0x7000164a18: 0xbd401be3,
    0x70001c22fc: 0xbd40e100, 0x70001c232c: 0xbd40e121,
    0x70001c23b4: 0xbd40d900, 0x70001c23e4: 0xbd40d921,
    0x70001c24bc: 0xbd409540, 0x70001c24d8: 0xbd409503,
    0x70001c2524: 0xbd408940, 0x70001c2540: 0xbd408903,
    0x70001c25b0: 0xbd409140, 0x70001c25cc: 0xbd409103,
    0x70001c2784: 0xbd408d40, 0x70001c27a0: 0xbd408d03,
}
for _site, _word in NAVIGATION_DURATION_SITES.items():
    PATCHES[_site] = Patch(_word, _navigation_duration, 'LINUX-014',
                           'scalar held-duration view - optional faster menu/arts navigation repeat')


# [XCDE] LINUX-013: passive, bounded developer diagnostics. The runtime helper
# is disabled unless built with --foley-trace; every original instruction runs.
def _foley_trace(kind):
    def emit(gen, addr):
        ins = gen.lift(addr)
        if ins.kind != 'op':
            raise ValueError(f'{addr:#x}: foley trace needs a plain instruction')
        return ('{ extern void xcde_foley_trace(Cpu *c, uint32_t kind); '
                f'xcde_foley_trace(c, {kind}); }} ' + ins.code)
    return emit


FOLEY_TRACE_SITES = {
    0x70003f4b90: (0x320007e8, 0, 'accepted effect marker'),
    0x70003f4c24: (0x320007e8, 1, 'accepted alternate effect marker'),
    0x7000466210: (0x6b18011f, 2, 'contact prior/current comparison'),
    0x7000482528: (0x2a0103f4, 3, 'terrain sound request'),
    0x70004822ec: (0xaa0103f3, 4, 'authored sound request'),
    0x70003f3048: (0x2a0103f4, 5, 'animation reset request'),
}
for _site, (_word, _kind, _note) in FOLEY_TRACE_SITES.items():
    PATCHES[_site] = Patch(_word, _foley_trace(_kind), 'LINUX-013', _note)


# [XCDE] LINUX-013: retain the phase-alignment seek and its exact guest-call
# effects, then synchronize the loop detector's previous-time clock origin.
# Ordinary seeks, transitions, sound/effect dispatch and genuine wraps retain
# their existing behavior. Capture arguments before the original call clobbers
# caller-saved registers. This callsite always seeks the base layer (x1 = 0).
def _animation_phase_rebase(gen, addr):
    ins = gen.lift(addr)
    if ins.kind != 'bl' or ins.target != 0x70003f0740:
        raise ValueError(f'{addr:#x}: phase alignment needs the original animation seek call')
    return ('{ uint64_t animation_ = c->x[0]; uint32_t layer_ = (uint32_t)c->x[1]; '
            'extern void xcde_animation_phase_rebase(uint64_t animation, uint32_t layer); '
            'extern void f_70003f0740(Cpu *c); '
            'c->x[30] = 0x70003f446cull; f_70003f0740(c); '
            'xcde_animation_phase_rebase(animation_, layer_); }')


PATCHES[0x70003f4468] = Patch(0x97fff0b6, _animation_phase_rebase, 'LINUX-013',
                            'bl animation seek - rebase previous cursor after phase-preserving clip change')


# T210 (party mod session): frame-count thresholds at the game's own 30 fps above 30 fps (runtime/hle/party.c
# xcde_frames_gt: "more than n frames at 30 fps" -> the same time at the current frame rate). A field follower restarts
# its "follow while running" move once its movement component (+0x200) counted more than 9 frames of a condition; at
# 60 fps that happened every ~11 frames and the party fell behind walking (2 -> 17.5 m in 200 frames; 30 fps: a steady
# 6.5 m). Both checks of that counter (f_7000443760, f_70004439f0) are `cmp w8, #9`.
def _frames_gt(n):
    def emit(gen, addr):
        return ('{ extern uint32_t xcde_frames_gt(uint32_t n); '
                f'(void)(subs32(c, (uint32_t)c->x[8], xcde_frames_gt({n}))); }}')
    return emit


PATCHES[0x700044394c] = Patch(0x7100251f, _frames_gt(9), 'T210', 'cmp w8, #9 - follower move counter (f_7000443760)')
PATCHES[0x7000443b9c] = Patch(0x7100251f, _frames_gt(9), 'T210', 'cmp w8, #9 - follower move counter (f_70004439f0)')


# [XCDE] LINUX-013: the walk/run hysteresis predicate uses the SAME movement
# counter as T210, but its complementary <10 check was still unscaled. At high
# FPS it rejected running before the scaled >9 checks, forcing walk/run changes
# and their outgoing footstep-event flushes. Match the existing dynamic window:
# count < 10*k complements count > 10*k-1, including the exact native30 boundary.
def _locomotion_frames_lt(gen, addr):
    ins = gen.lift(addr)
    if ins.kind != 'op':
        raise ValueError(f'{addr:#x}: locomotion counter needs a plain compare')
    return ('{ extern uint32_t xcde_frames_gt(uint32_t n); '
            '(void)subs32(c, (uint32_t)c->x[8], xcde_frames_gt(9) + 1u); }')


PATCHES[0x700045b370] = Patch(0x7100291f, _locomotion_frames_lt, 'LINUX-013',
                            'cmp w8, #10 - keep walk/run hysteresis in the T210 movement-counter window')


# T222 (party mod session): save / load list thumbnails. f_700057da00 copies a slot's loaded .tmb picture (320x180 RGB)
# into the buffer it maps (bl memcpy at 0x700057dab4: x0 destination, x1 source, x2 size) - only one of the slot's two
# alternating picture buffers. The hook records which buffer holds the game's own picture (runtime/nvn/vulkan/exec.c
# slot_thumb_fix copies it into the twin): the twin is not always empty - in-game its memory held leftovers, shown
# every other frame as pink / green garbage.
def _thumb_loaded(gen, addr):
    return ('c->x[30] = 0x700057dab8ull; { extern void xcde_thumb_loaded(uint64_t d, uint64_t n); '
            'uint64_t d_ = c->x[0], n_ = c->x[2]; f_70008b6b48(c); xcde_thumb_loaded(d_, n_); }')


PATCHES[0x700057dab4] = Patch(0x940ce425, _thumb_loaded, 'T222', 'bl memcpy - slot thumbnail picture loaded (f_700057da00)')
