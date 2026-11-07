# ADR 0003: Lift main and sdk; HLE of `movie::`; no subsdk0 or rtld

- **Status:** Accepted · **Date:** 2026-10-17
- **Requirements:** FR-010, FR-042

## Decision
`main` and `sdk` are recompiled. `subsdk0` (the multimedia library, Stagefright + NvMM) is replaced by host implementations of the 28 `movie::` functions `main` imports. The loader does `rtld`'s job.

## Reason
`subsdk0` depends on nvdec / NvMM (hardware). `main` only uses a small API. Saves ~59 MB of C and 18 K functions.

## Alternatives considered
Lifting subsdk0 and simulating the video decoding services: much more work, and fragile.
