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


# T222 (party mod session): save / load list thumbnails. f_700057da00 copies a slot's loaded .tmb picture (320x180 RGB)
# into the buffer it maps (bl memcpy at 0x700057dab4: x0 destination, x1 source, x2 size) - only one of the slot's two
# alternating picture buffers. The hook records which buffer holds the game's own picture (runtime/nvn/vulkan/exec.c
# slot_thumb_fix copies it into the twin): the twin is not always empty - in-game its memory held leftovers, shown
# every other frame as pink / green garbage.
def _thumb_loaded(gen, addr):
    return ('c->x[30] = 0x700057dab8ull; { extern void xcde_thumb_loaded(uint64_t d, uint64_t n); '
            'uint64_t d_ = c->x[0], n_ = c->x[2]; f_70008b6b48(c); xcde_thumb_loaded(d_, n_); }')


PATCHES[0x700057dab4] = Patch(0x940ce425, _thumb_loaded, 'T222', 'bl memcpy - slot thumbnail picture loaded (f_700057da00)')
