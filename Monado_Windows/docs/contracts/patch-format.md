# Contract: patch format

`games/<id>/patches.py` defines `PATCHES = { address: Patch }`, each `Patch` with:
- `expected`: the expected original bytes (u32). If they don't match, the generator aborts with the address and the
  bytes found.
- `emit(gen, addr) -> str`: the replacement C.
- `req`: the requirement or task that justifies it (mandatory).
- `note`: why.
