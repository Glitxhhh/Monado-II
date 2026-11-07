# ADR 0004: LAS's memory layout

- **Status:** Accepted · **Date:** 2026-10-17
- **Requirements:** FR-010, NFR-004

## Decision
`main` at `0x7000000000`, `sdk` at `0x7004000000`, guest window `0x7000000000–0x7320000000`, heap up to 3.25 GB, guest addresses = host addresses.

## Reason
Both images (`0x12f9000` and `0x1296000`) fit without overlapping and LAS's constants are reused. No address translation.

## Alternatives considered
A translation table: slower. `HEAP_MAX` reviewed after measuring (Q5: 3.15 GB requested).
