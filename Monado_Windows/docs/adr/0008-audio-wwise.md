# ADR 0008: Audio: lifted Wwise; audren / audout and hwopus

- **Status:** Accepted · **Date:** 2026-10-17
- **Requirements:** FR-040, FR-041

## Decision
The Wwise engine (static in `main`, v134 banks) runs recompiled. The host provides `audren:u` / `audout:u` and `hwopus` with libopus.

## Reason
Evidence: `AK::*` strings, `AK::OpusDecodingThread`, the declared `hwopus` service, `BKHD` v134 banks and `AKPK` packages.

## Alternatives considered
HLE of Wwise: not viable (the engine's proprietary format).
