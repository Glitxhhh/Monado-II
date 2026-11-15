// [XCDE] LINUX-040 r4: do not start optional UI construction in a depleted arena.
#include "../hle.h"
#include "../settings.h"
#include "quickstep_ui_memory.h"
#include <stdatomic.h>

// Successful pending-loader polls must not consume the later failure budget.
static _Atomic unsigned reports[2];
static bool range(uint64_t p, size_t n) {
    return p >= GUEST_LO && p < GUEST_HI && n <= GUEST_HI - p;
}
static uint64_t call(Cpu *c, uint64_t fn, uint64_t a, uint64_t b, uint64_t d) {
    Cpu saved = *c;
    uint64_t fpcr = rt_get_fpcr(), fpsr = rt_get_fpsr();
    c->sp -= 0x2000;
    c->x[0] = a; c->x[1] = b; c->x[2] = d;
    guest_call(c, fn);
    uint64_t result = c->x[0];
    *c = saved; rt_set_fpcr(fpcr); rt_set_fpsr(fpsr);
    return result;
}
static bool selected(uint64_t context, uint64_t registry, uint32_t selector,
                     uint64_t record) {
    if (!range(context, 0x2c) || LD32(context + 0x28) != selector
        || !range(registry + 0xab000, 64) || !range(record, 0xc8)) return false;
    uint32_t bit = selector - 1;
    return (LD32(registry + 0xab000 + 4 * (bit >> 5)) & (1u << (bit & 31))) != 0;
}
bool quickstep_ui_memory_allow(Cpu *c, uint64_t headroom, const char *consumer) {
    if (!g_settings.quickstep_slot) return false;
    const char *reason = "invalid-request";
    uint32_t selector = 0;
    uint64_t total = 0, used = 0, free_bytes = 0, largest = 0;
    uint64_t expected_allocator = 0, expected_heap = 0, expected_cookie = 0;
    bool allowed = false;
    if (!headroom || headroom > 64ull * 1024 * 1024 || (c->sp & 15)
        || c->sp < GUEST_LO + 0x2100 || !range(c->sp - 0x2100, 0x2100)) goto done;
    reason = "selector";
    uint64_t owner = LD64(0x7000b8a278ull);
    if (!range(owner, 8)) goto done;
    uint64_t context = LD64(owner);
    if (!range(context, 0x2c)) goto done;
    selector = LD32(context + 0x28);
    if (!selector || selector > 512) goto done;
    uint64_t manager = LD64(0x7000b935a8ull);
    if (!range(manager, 8)) goto done;
    uint64_t registry = LD64(manager);
    if (!range(registry, 0xab040)) goto done;
    uint64_t record = registry + (selector - 1) * 0x558ull;
    if (!selected(context, registry, selector, record)) goto done;
    // Native5F50D0 uses this same recursive record mutex for allocations.
    // Never walk the variable-size heap without its synchronized record.
    reason = "unsynchronized-arena";
    if (!(LD8(record + 0xc0) & 2) || !LD8(record + 0x20)) goto done;
    uint64_t scratch = c->sp - 0x100;
    uint8_t original[0x80];
    memcpy(original, (void *)(uintptr_t)scratch, sizeof original);
    memset((void *)(uintptr_t)scratch, 0, sizeof original);
    call(c, 0x70005dbde0ull, scratch, record, 0);
    reason = "owner-changed";
    bool locked = LD64(scratch) == record;
    if (locked && g_settings.quickstep_slot
        && LD64(0x7000b8a278ull) == owner && LD64(owner) == context
        && LD64(0x7000b935a8ull) == manager && LD64(manager) == registry
        && selected(context, registry, selector, record)
        && (LD8(record + 0xc0) & 2) && LD8(record + 0x20)) {
        uint64_t allocator = LD64(record + 0x28);
        reason = "allocator";
        if (range(allocator, 0x48)) {
            uint64_t cls = LD64(allocator), heap = LD64(allocator + 0x30);
            uint64_t cookie = LD64(0x7000bb51a0ull);
            // Exactly the selected DE2.0.0 native variable-size heap query.
            if (cls == 0x7000b6b530ull && range(cls, 0x58)
                && LD64(cls + 0x50) == 0x70005f3f60ull
                && range(heap, 0x390) && cookie && LD64(heap + 0x40) == cookie) {
                call(c, 0x70005f3f60ull, allocator, scratch + 0x10, 0);
                expected_allocator = allocator; expected_heap = heap; expected_cookie = cookie;
                total = LD64(scratch + 0x10); used = LD64(scratch + 0x18);
                free_bytes = LD64(scratch + 0x20); largest = LD64(scratch + 0x28);
                reason = "heap-metrics";
                if (total && total <= GUEST_HI - GUEST_LO && used <= total
                    && free_bytes <= total && largest <= free_bytes) {
                    uint64_t block = headroom < 0x10000 ? headroom : 0x10000;
                    allowed = free_bytes >= headroom && largest >= block;
                    reason = allowed ? "allowed" : "insufficient-headroom";
                }
            }
        }
    }
    // Release BEFORE any layout/texture construction; holding this heap mutex
    // across those managers would introduce a lock-order inversion. The result
    // is consequently a conservative snapshot, not a reservation or a promise
    // that unrelated concurrent allocation cannot exhaust the native arena.
    if (locked) call(c, 0x70005dbe20ull, scratch, 0, 0);
    if (allowed && (!g_settings.quickstep_slot || !selected(context, registry, selector, record)
        || LD64(0x7000b8a278ull) != owner || LD64(owner) != context
        || LD64(0x7000b935a8ull) != manager || LD64(manager) != registry
        || LD64(record + 0x28) != expected_allocator
        || LD64(expected_allocator) != 0x7000b6b530ull
        || LD64(expected_allocator + 0x30) != expected_heap
        || LD64(expected_heap + 0x40) != expected_cookie
        || LD64(0x7000bb51a0ull) != expected_cookie
        || !(LD8(record + 0xc0) & 2) || !LD8(record + 0x20))) {
        allowed = false; reason = "owner-changed";
    }
    memcpy((void *)(uintptr_t)scratch, original, sizeof original);
done:
    if (atomic_fetch_add_explicit(&reports[allowed ? 0 : 1], 1, memory_order_relaxed) < 32)
        fprintf(stderr, "[quickstep-ui-memory] consumer=%.48s selector=%u requested=%llu total=%llu used=%llu free=%llu largest=%llu result=%s\n",
                consumer ? consumer : "unknown", selector, (unsigned long long)headroom,
                (unsigned long long)total, (unsigned long long)used,
                (unsigned long long)free_bytes, (unsigned long long)largest, reason);
    return allowed;
}
RT_ON_RESET(quickstep_ui_memory_reset) {
    atomic_store_explicit(&reports[0], 0, memory_order_relaxed);
    atomic_store_explicit(&reports[1], 0, memory_order_relaxed);
}
