// [XCDE] LINUX-018: checked precompile child/supervisor wire protocol. Values are single ASCII tokens.
#pragma once
#include <stddef.h>
#include <string.h>
#define PRECOMPILE_WORK_MAGIC "monado_precompile_work 1"
#define PRECOMPILE_CANCEL_MAGIC "monado_precompile_cancel 1"
#define PRECOMPILE_WORK_PATH "cache/precompile_work.txt"
#define PRECOMPILE_CANCEL_PATH "cache/precompile_cancel.txt"
#define PRECOMPILE_ID_ENV "XCDE_PRECOMPILE_ID"
#define PRECOMPILE_SESSION_ENV "XCDE_PRECOMPILE_SESSION"
// Work fields: session, identity, phase, state; decimal files_total/files_done/shaders/programs/programs_done,
// seed_total/seed_done/variants_total/variants_done/attempted/built/failed/skipped/unsupported/errors,
// gpu_vendor/gpu_device/gpu_driver; gpu_cache_uuid is32hex. Unknown future fields may be ignored.
// phase: scan|build|seed|pipelines|done. state: running|complete|partial|cancelled|failed.
// Cancel file: first PRECOMPILE_CANCEL_MAGIC, then "session <current32hex>". Stale sessions never cancel work.
static inline int precompile_hex(const char *s, size_t n) {
    if (!s || strlen(s) != n) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))) return 0;
    return 1;
}
