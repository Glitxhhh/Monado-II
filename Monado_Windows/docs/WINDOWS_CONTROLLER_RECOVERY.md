# Native SDL controller recovery

The Windows ImGui SDL3 backend adapts accepted LINUX-089 event-thread recovery.
It polls for missed add/remove events at most once per second, permits one
immediate focus-return retry, retains a healthy AutoFirst selection across
enumeration changes and retains healthy AutoAll handles while replacing losses.
Manual mode preserves the caller's complete handle ownership. Last-device loss
emits one neutral navigation snapshot. Open-error text is bounded/sanitized and
at most sixteen failures are logged. These are portable SDL policies; there is
no Linux hidraw or Steam process handling in the native backend.

`tests/run_windows_imgui_controller.py` compiles the actual Windows backend with
the actual ImGui core and pinned native SDL3 library. Ten authored virtual-device
scenarios run with SDL dummy video, including bounded failure/backoff, disconnect
release, focus retry, replacement, AutoFirst/AutoAll and Manual ownership. It
creates no game, settings or save profile and injects no input into other apps.
Private receipts bind the fixture/backend/header/core sources, native dependency
prefix, compiler arguments, PE hash and per-scenario logs under `runs/`.

These native component passes do not qualify physical controllers, rumble,
third-party overlays, Bluetooth reconnect or a human gameplay session. The same
backend translation unit remains in the production settings launcher compile and
link lists; no fixture-only recovery implementation is used.
