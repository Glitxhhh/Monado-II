// [XCDE] LINUX-013: bounded, passive diagnostics for the selected DE 2.0.0
// footstep/event paths. Included by framepace.c. Checked patches retain their
// original instruction; this helper only reads Cpu/guest memory and logs.
#if defined(XCDE_FOLEY_TRACE) && XCDE_FOLEY_TRACE
#include <stdatomic.h>

enum { FOLEY_TRACE_LIMIT = 4096 };
static _Atomic uint32_t foley_trace_entries;

static uint16_t foley_u16(uint64_t address) {
    uint16_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof value);
    return value;
}
static uint8_t foley_u8(uint64_t address) {
    uint8_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof value);
    return value;
}
static uint32_t foley_u32(uint64_t address) {
    uint32_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof value);
    return value;
}
static uint64_t foley_u64(uint64_t address) {
    uint64_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof value);
    return value;
}
static float foley_f32(uint64_t address) {
    float value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof value);
    return value;
}
static bool foley_claim(uint32_t *entry) {
    uint32_t used = atomic_load_explicit(&foley_trace_entries, memory_order_relaxed);
    while (used < FOLEY_TRACE_LIMIT) {
        if (atomic_compare_exchange_weak_explicit(&foley_trace_entries, &used, used + 1,
                                                  memory_order_relaxed, memory_order_relaxed)) {
            *entry = used + 1;
            return true;
        }
    }
    return false;
}

void xcde_foley_trace(Cpu *c, uint32_t kind) {
    // Exhausted or unsupported calls must not inspect Cpu or guest pointers.
    if (atomic_load_explicit(&foley_trace_entries, memory_order_relaxed) >= FOLEY_TRACE_LIMIT ||
        kind > 5 || !c) return;
    uint64_t component, marker = 0;
    uint32_t prior = 0, current = 0, id = 0, mode = 0;
    uint8_t subtype = 0;
    switch (kind) {
    case 0: component = c->x[19]; marker = c->x[20]; break;
    case 1: component = c->x[20]; marker = c->x[19]; break;
    case 2:
        component = c->x[19];
        prior = (uint32_t)c->x[8]; current = (uint32_t)c->x[24];
        if (prior == current) return;
        break;
    case 3: component = c->x[0]; id = (uint32_t)c->x[1]; break;
    case 4: component = c->x[0]; marker = c->x[1]; break;
    case 5: component = c->x[0]; mode = (uint32_t)c->x[1]; break;
    default: return;
    }
    if (!component) return;
    if (kind == 0 || kind == 1 || kind == 4) {
        if (!marker) return;
        memcpy(&subtype, (const void *)(uintptr_t)(marker + 0xa), sizeof subtype);
        // Only effect-marker subtypes 0/19/20 reach the terrain-footstep path.
        // Authored sound requests use their own 0/1/2 subtype policy; log them
        // without interpreting every request as a sound being played.
        if (kind != 4 && subtype != 0 && subtype != 19 && subtype != 20) return;
    }
    uint32_t entry;
    if (!foley_claim(&entry)) return;

    uint64_t now = plat_now_ns();
    uint64_t slot = foley_u64(0x7000b93458ull);
    uint64_t clock = slot ? foley_u64(slot) : 0;
    float elapsed = clock ? foley_f32(clock + 0x2034) : 0;
    uint64_t actor = foley_u64(component + 0x38);
    uint32_t actor_id = actor ? foley_u32(actor + 0x34) : 0;
    uint32_t actor_type = actor ? foley_u32(actor + 0x30) : 0;

    // One stdio call per entry keeps concurrent records on distinct lines.
    // No stateful event suppression: only the filters above and process budget.
#define FOLEY_COMMON_ARGS entry, kind, (unsigned long long)now, (double)elapsed, \
    (unsigned long long)component, (unsigned long long)actor, actor_id, actor_type
#define FOLEY_COMMON "[foley] n=%u kind=%u t_ns=%llu dt=%.9g comp=%llx actor=%llx actor_id=%u actor_type=%u"
    if (kind == 0 || kind == 1) {
        uint32_t layer = foley_u32(component + 0x1c4);
        uint32_t generation = 0, seen_generation = 0;
        uint32_t parameter = 0, motion = 0, selected = 0, action = 0, transition = 0;
        uint64_t count = 0, control = 0, gate_control = 0;
        float stored_cursor = 0;
        if (layer < 2) {
            generation = foley_u32(component + 0xc4 + 0x50 * layer);
            stored_cursor = foley_f32(component + 0xc0 + 0x50 * layer);
            seen_generation = foley_u32(component + 0x300 + 0x138 * layer);
            // 3f1510's selected layer control supplies the loop counter's
            // cursor. 3f4690 instead looks up the current motion's control.
            // Record both: a control change can resemble an animation wrap.
            uint64_t state = component + 0xb0 + 0x50 * layer;
            selected = foley_u8(state + 0x18);
            count = foley_u64(state + 0x28);
            uint64_t entries = foley_u64(state + 0x20);
            if (entries && selected < count)
                control = foley_u64(entries + 0x30 * selected + 0x18);
            uint64_t parameters = component + 0x138 * layer;
            // 3f4040 retains the action and the request's transition code;
            // the latter identifies the last accepted graph condition.
            action = foley_u32(parameters + 0x1d0);
            transition = foley_u32(parameters + 0x1d4);
            parameter = foley_u32(parameters + 0x2d0);
            if (parameter < 2) {
                motion = foley_u32(parameters + 0x200 + 0x78 * parameter);
                // Mirror only the existing non-allocating lookup in3f27b0.
                // Do not invoke its lazy creation or any guest function.
                uint64_t motions = foley_u64(component + 0x78);
                uint64_t motion_count = foley_u64(component + 0x80);
                if (motions && motion < 0x80000000u && motion < motion_count)
                    gate_control = foley_u64(motions + 0x20 * (uint64_t)motion + 8);
            }
        }
        LOG(FOLEY_COMMON " marker=%llx frame=%u subtype=%u layer=%u layer_valid=%u generation=%u stored_cursor=%.9g seen_generation=%u"
            " action=%u transition=%u parameter=%u motion=%u selected=%u controls=%llu control=%llx cursor=%.9g rate=%.9g weight=%.9g duration=%.9g flags=%u"
            " gate_control=%llx gate_cursor=%.9g gate_rate=%.9g\n",
            FOLEY_COMMON_ARGS, (unsigned long long)marker, (unsigned)foley_u16(marker + 8),
            (unsigned)subtype, layer, (unsigned)(layer < 2), generation, (double)stored_cursor, seen_generation,
            action, transition, parameter, motion, selected, (unsigned long long)count, (unsigned long long)control,
            control ? (double)foley_f32(control + 0x80) : 0,
            control ? (double)foley_f32(control + 0x88) : 0,
            control ? (double)foley_f32(control + 0x84) : 0,
            control ? (double)foley_f32(control + 0x90) : 0,
            control ? foley_u32(control + 0x94) : 0,
            (unsigned long long)gate_control,
            gate_control ? (double)foley_f32(gate_control + 0x80) : 0,
            gate_control ? (double)foley_f32(gate_control + 0x88) : 0);
    } else if (kind == 2) {
        LOG(FOLEY_COMMON " prior=%u current=%u\n", FOLEY_COMMON_ARGS, prior, current);
    } else if (kind == 3) {
        LOG(FOLEY_COMMON " sound_id=%u\n", FOLEY_COMMON_ARGS, id);
    } else if (kind == 4) {
        LOG(FOLEY_COMMON " marker=%llx subtype=%u sound_id=%u\n", FOLEY_COMMON_ARGS,
            (unsigned long long)marker, (unsigned)subtype, (unsigned)foley_u16(marker + 0xe));
    } else {
        LOG(FOLEY_COMMON " mode=%u\n", FOLEY_COMMON_ARGS, mode);
    }
#undef FOLEY_COMMON
#undef FOLEY_COMMON_ARGS
}
#else
void xcde_foley_trace(Cpu *c, uint32_t kind) {
    (void)c;
    (void)kind;
}
#endif
