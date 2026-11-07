# ADR 0005: Vulkan 1.3 first, Metal later

- **Status:** Accepted · **Date:** 2026-10-17
- **Requirements:** NFR-002, NFR-004

## Decision
The first graphics backend is Vulkan 1.3 (Windows and Linux). Metal is left for macOS if there is interest.

## Reason
One backend covers Windows and Linux; LAS already has both.

## Alternatives considered
An OpenGL fallback (like ssbu): more code, less benefit.
