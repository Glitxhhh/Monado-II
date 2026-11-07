# ADR 0009: Video: HLE of `movie::` with FFmpeg

- **Status:** Accepted · **Date:** 2026-10-17
- **Requirements:** FR-042

## Decision
The 14 WebM cutscenes play through FFmpeg behind the game's `movie::` API; NV12→RGBA conversion on the GPU.

## Reason
LAS solved the same API (`hle/movie.c`). The cutscenes are standard WebM (EBML).

## Alternatives considered
Conversion on the CPU: 30–45 fps on mid-range machines (LAS #82).
