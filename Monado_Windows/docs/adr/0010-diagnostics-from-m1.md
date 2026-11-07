# ADR 0010: Diagnostics from M1

- **Status:** Accepted · **Date:** 2026-10-17
- **Requirements:** FR-060–065, Art. VII

## Decision
Watchdog, out-of-process minidump, deterministic mode with a per-frame hash, `VK_EXT_device_fault` + checkpoints and the HLE audit exist before gameplay work.

## Reason
The real hangs of ssbu and LAS showed after 30–60 min and were diagnosed thanks to these tools.

## Alternatives considered
Adding them later: reproducing hangs without tools.
