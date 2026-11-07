# ADR 0001: Base: LASRecomp; ssbu-recomp only as a reference

- **Status:** Accepted · **Date:** 2026-10-17
- **Requirements:** FR-010, Art. III, Art. X

## Decision
Start from LASRecomp's generator and runtime (commit `136f679`, MIT). ssbu-recomp and B2recomp (GPL-3.0) are used only as design references.

## Reason
LAS lifts the SDK and simulates SVC + IPC (~21 services), which absorbs SDK version differences between games. It already implements through HLE 26 of the 28 `movie::` functions XCDE uses, `nvnBootstrapLoader`, `nn::vi`, `nv::`, `nn::pl`, `nn::mem` and `nn::hid`. Its generator decodes 100 % of XCDE's instructions (research.md). MIT licence and three platforms.

## Alternatives considered
ssbu-recomp / B2recomp: HLE of ~1,100 `nn::` functions per game, Windows only, GPL-3.0. SkyRecomp: a LAS fork without attribution and with binaries. Starting from zero: months of already solved work.
