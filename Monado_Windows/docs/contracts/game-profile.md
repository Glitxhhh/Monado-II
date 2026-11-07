# Contract: game profile

A *game profile* (`games/<id>/profile.json`) is the single source of truth for what sets a game apart from the common
runtime.

| Field | Type | XCDE example | Use |
|---|---|---|---|
| `id` | string | `xcde` | the `games/xcde/` folder |
| `title_id` | string | `0100ff500e34a000` | messages and save paths |
| `version` | string | `2.0.0` | version check |
| `modules[]` | `{name, base, lift, build_id}` | `main`/`0x7000000000`/lift, `sdk`/`0x7004000000`/lift, `subsdk0`/`0x7002000000`/no lift, `rtld` | loader and generator |
| `romfs_manifest` | `{dirs:{name:[n_files,bytes]}, sha256}` | | FR-004 |
| `heap_max` | bytes | | memory |
| `patches` | path | `games/xcde/patches.py` | FR-013 |
| `hooks` | path | `games/xcde/hooks.c` | FR-012 |

Rules: the runtime must not refer to a game by name; flags like `g_is_las` are replaced by profile queries.
