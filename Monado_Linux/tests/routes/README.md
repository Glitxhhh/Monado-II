# Test routes (FR-063)

A route is a recording of controller input: one line per change, `<frame> <buttons hex> <lx> <ly> <rx> <ry>`.
Buttons: A=1 B=2 X=4 Y=8 L=0x40 R=0x80 ZL=0x100 ZR=0x200 Plus=0x400 Minus=0x800, d-pad left / up / right / down =
0x1000 / 0x2000 / 0x4000 / 0x8000. Lines starting with `#` are comments.

- Record: `XCDE_INPUT_RECORD=tests/routes/my_route.txt build/xcde.exe targets` (play; every state change is written).
- Replay: `XCDE_INPUT_REPLAY=tests/routes/my_route.txt build/xcde.exe targets` (overrides the host input).
- With `XCDE_DETERMINISTIC=1 XCDE_FRAME_HASH=build/h.txt` you also get each frame's hash; compare two runs with
  `python tools/hash_diff.py a.txt b.txt`. `scripts/drive_env.sh` wraps all of this (hidden window, captures, a save
  to start from).

`input_sample.txt` is a minimal example route (A, Plus and stick up on the title); it holds no game data. Routes that
load a save (`load_*`, `save_a`, `menu_*`, `battle_*`) expect `DRIVE_SAVE=<folder with save_0/>`.
