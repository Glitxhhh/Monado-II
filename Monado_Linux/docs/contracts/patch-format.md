# Contract: patch format

`games/<id>/patches.py` defines `PATCHES = { address: Patch }`, each `Patch` with:
- `expected`: the expected original bytes (u32). If they don't match, the generator aborts with the address and the
  bytes found.
- `emit(gen, addr) -> str`: the replacement C.
- `req`: the requirement or task that justifies it (mandatory).
- `note`: why.

## Checked function entries [XCDE] LINUX-041

The same authored file may define `FUNCTION_ENTRIES`, a tuple of
`patchfmt.FunctionEntries` objects. Each names one module, a checked build-ID
prefix, the native entry addresses and their expected words, dispatch-site word
guards, and a requirement/task. Entries are added before the generator's
partition fixpoint so a legitimate indirect target crossing a partition has an
ordinary native body and dispatch entry. They do not replace instructions or
install a runtime fallback.

The generator validates the entire matching module set before creating output:
build identity, text range, alignment, uint32 words and duplicate entries must
all pass. Other modules receive no entries from that set. Metadata contains
addresses and version/word checks only; game-derived tables, generated C or other
game data remain outside source packages.
