# ADR 0006: Common runtime + `games/xcde/` + game profile

- **Status:** Accepted · **Date:** 2026-10-17
- **Requirements:** Art. IX

## Decision
Nothing XCDE-specific in `runtime/`. Addresses, build-ids, patches, hooks and options go in `games/xcde/`, described by a game profile.

## Reason
The other three Xenoblade games should reuse the runtime. LAS has game constants and flags (`g_is_las`) spread around that need isolating. (In practice several XCDE hooks ended up in `runtime/hle/`: moving them out is open work.)

## Alternatives considered
A fork per game: divergence and duplicated work.
