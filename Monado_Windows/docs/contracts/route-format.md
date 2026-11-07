# Contract: test routes

A text file, one line per input change: `<frame> <buttons hex> <lx> <ly> <rx> <ry>` (see `tests/routes/README.md`).
Deterministic replay: fixed clock and seed. Checks are made by per-frame hashes (`XCDE_FRAME_HASH`) and captures
(`XCDE_SHOTS`). Capture references are local and never published.
