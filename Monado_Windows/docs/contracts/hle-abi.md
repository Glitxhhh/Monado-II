# Contract: ABI of HLE implementations

- Signature: `void fn(Cpu *c)`; arguments in `c->x[0..7]` and `c->v[0..7]` (AAPCS64), result in `c->x[0]` / `c->v[0]`.
- Registration by **symbol name** (`HLE_FUNC("name", ident)`); the loader binds UNDEF imports against the registry
  before the lifted SDK.
- Override by **address** (`hle_override(addr, fn, name)`) only with a cited task and requirement.
- Guest-visible memory is reserved with `hle_alloc` (the `HLE_HEAP` zone); never host heap pointers.
- Every HLE declares its audit class: `real`, `simplified` or `stub`. A stub returns 0 only when audited; in strict
  mode it stops the game with a report.
- No host blocking: an HLE that waits does so with the simulated kernel's primitives (so the watchdog sees it).
