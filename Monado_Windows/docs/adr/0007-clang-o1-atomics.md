# ADR 0007: clang, `-O1` by default, atomics with `__atomic`

- **Status:** Accepted · **Date:** 2026-10-17
- **Requirements:** FR-011, NFR-005

## Decision
Compile the generated C with clang (Zig's `zig cc`), `-O1` by default and `-O0` for huge units. LDAR / STLR / LDXR / STXR as `__atomic` with the guest's ordering, DMB / DSB as fences.

## Reason
At `-O2`, plain memory accesses break the guest's ordering (ssbu lesson #20). Build time is tied to the C size (≈ 236 MB).

## Alternatives considered
GCC / MSVC: worse support for `musttail` and the extensions the generator uses.
