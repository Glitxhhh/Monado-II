// [XCDE] LINUX-039: authored ownership adapter for a real, native Quick Step gem.
// The native item pool, equipment, effect decoder, stacking and caps stay native.
// [XCDE] LINUX-044: publish validated bonus ownership to native row E markers.
#include "../hle.h"
#include "../settings.h"
#include "quickstep_slot.h"
#include "quickstep_save.h"

enum { INVENTORY = 0x41c0, GEM_POOL = 0x28870, GEM_STRIDE = 0x2c,
       GEAR_STRIDE = 0x30, GEAR_CAPACITY = 500, SPECIAL_GEAR_CAPACITY = 32,
       GEAR_END = 0x42870,
       EFFECT_STRIDE = 0x2c, NATIVE_EFFECTS = 8, EXTRA_EFFECTS = 9,
       LEASE_CAPACITY = 32, LEASE_BYTES = 0x240,
       LEASE_ARRAY = 0x10, LEASE_INPUT = 0x1a0, LEASE_COUNT = 0x1d0,
       LEASE_TAIL = 0x230 };
static const uint64_t canary = 0xd94ea3b67210c85full;

typedef struct {
    Cpu *cpu;
    uint64_t stack, memory, generation, original_array, original_x22;
    uint32_t original_count;
    bool busy, committed;
} EffectLease;
typedef struct {
    Cpu *cpu;
    uint64_t stack, context, generation;
    uint32_t character;
    bool busy;
} DeferredRefresh;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t playing, generation, last_context, last_view;
static QuickstepSnapshot current;
static uint32_t dirty;
static bool active, last_enabled, refreshing;
static EffectLease leases[LEASE_CAPACITY];
static DeferredRefresh deferred[LEASE_CAPACITY];

static bool range(uint64_t p, size_t n) {
    return p >= GUEST_LO && p < GUEST_HI && n <= GUEST_HI - p;
}

typedef struct { uint32_t type, normal, special; } GearPool;
// Checked against the selected native 9A040 resolver, including the last normal
// and special instances. Both stories resolve one adopted playing inventory.
static const GearPool gear_pools[] = {
    {2, 0, 0x40470}, {4, 0x5dc0, 0x40a70}, {5, 0xbb80, 0x41070},
    {6, 0x11940, 0x41670}, {7, 0x17700, 0x41c70}, {8, 0x1d4c0, 0x42270},
};

static bool ordinary_attached(uint64_t data, uint32_t uid) {
    if (!range(data, INVENTORY + GEAR_END)) return true;
    for (size_t p = 0; p < sizeof gear_pools / sizeof gear_pools[0]; ++p)
        for (unsigned special = 0; special < 2; ++special) {
            uint64_t start = data + INVENTORY + (special ? gear_pools[p].special : gear_pools[p].normal);
            uint32_t count = special ? SPECIAL_GEAR_CAPACITY : GEAR_CAPACITY;
            for (uint32_t i = 0; i < count; ++i) {
                uint64_t item = start + i * GEAR_STRIDE;
                if (!LD8(item + 0x10) || LD16(item + 6) != gear_pools[p].type) continue;
                uint32_t sockets = LD8(item + 0x15);
                if (sockets > 3) return true; // Corrupt native layout cannot establish free ownership.
                for (uint32_t s = 0; s < sockets; ++s) {
                    uint64_t reference = item + 0x18 + s * 8;
                    // Native 1733B0 packs the intrinsic flag at bit48. Such
                    // static definitions never own a pooled gem instance.
                    if (!LD8(reference + 6) && LD32(reference) == uid) return true;
                }
            }
        }
    return false;
}

static bool playable(uint32_t story, uint32_t character) {
    return story == 0 ? character >= 1 && character <= 8
                      : story == 1 && (character == 1 || character == 7
                                      || character == 14 || character == 15);
}

// Preserve the entire guest CPU, TLS/exclusive state and host ARM FP environment.
// Native helpers can yield; no ownership mutex is held across their execution.
static uint64_t call(Cpu *c, uint64_t fn, uint64_t a, uint64_t b,
                     uint64_t d, uint64_t e, uint64_t f) {
    Cpu saved = *c;
    uint64_t fpcr = rt_get_fpcr(), fpsr = rt_get_fpsr();
    if ((c->sp & 15) || c->sp < GUEST_LO + 0x1000 || !range(c->sp - 0x1000, 0x1000))
        FATAL("[quickstep] invalid native helper stack\n");
    c->sp -= 0x1000;
    c->x[0] = a; c->x[1] = b; c->x[2] = d; c->x[3] = e; c->x[4] = f;
    guest_call(c, fn);
    uint64_t result = c->x[0];
    *c = saved;
    rt_set_fpcr(fpcr); rt_set_fpsr(fpsr);
    return result;
}

static uint64_t resolve(Cpu *c, uint64_t context, uint32_t *story) {
    pthread_mutex_lock(&lock);
    bool live = active;
    pthread_mutex_unlock(&lock);
    if (!live) return 0;
    if (!range(context, 8) || (c->sp & 15) || !range(c->sp - 0x80, 8)) return 0;
    uint64_t reference = c->sp - 0x80;
    ST64(reference, context);
    uint64_t data = call(c, 0x7000592b80ull, reference, 0, 0, 0, 0);
    uint64_t view = call(c, 0x7000592a30ull, reference, 0, 0, 0, 0);
    if (!range(view, 0x151) || !range(data, INVENTORY + GEAR_END)) return 0;
    *story = (LD8(view + 0x150) >> 3) & 1u;
    pthread_mutex_lock(&lock);
    bool matches = active && data == playing && *story == current.story;
    if (matches) { last_context = context; last_view = view; }
    pthread_mutex_unlock(&lock);
    return matches ? data : 0;
}

static bool gem(uint64_t data, uint32_t uid, QuickstepGem *out) {
    if ((uid & 0xffff0000u) != 0x30000u || (uid & 0xffffu) >= QUICKSTEP_GEM_CAPACITY) return false;
    uint64_t item = data + INVENTORY + GEM_POOL + (uid & 0xffffu) * GEM_STRIDE;
    // No cylinders, intrinsic gems, free pool entries, zero serial or mixed-effect
    // records. Crafted pooled gems legitimately have static definition zero;
    // their serial and native payload still identify one actual owned instance.
    if (!range(item, GEM_STRIDE) || LD32(item) != uid || LD16(item + 6) != 3
            || !LD32(item + 0xc) || !LD8(item + 0x10)
            || LD8(item + 0x1a) != 1 || LD16(item + 0x1c) != 77
            || !LD8(item + 0x1e) || LD8(item + 0x1e) > 25
            || LD8(item + 0x16) < 1 || LD8(item + 0x16) > 6) return false;
    memset(out, 0, sizeof *out);
    out->uid = uid;
    out->rank = LD8(item + 0x16);
    out->value = LD8(item + 0x1e);
    // +11 is the native favourite/lock flag, not equipment ownership. An actual
    // socket reference reserves the instance even when its parent is unworn.
    out->availability = ordinary_attached(data, uid) ? QUICKSTEP_ORDINARY_EQUIPPED : QUICKSTEP_FREE;
    // Canonical opaque identity: serial, static definition/type and native gem
    // payload. Favourite flags and quantity are deliberately excluded.
    memcpy(out->fingerprint, (void *)(uintptr_t)(item + 0xc), 4);
    memcpy(out->fingerprint + 4, (void *)(uintptr_t)(item + 4), 4);
    memcpy(out->fingerprint + 8, (void *)(uintptr_t)(item + 0x14), 24);
    return true;
}

static int entry_for(uint32_t character) {
    for (uint32_t i = 0; i < current.count; ++i)
        if (current.entries[i].character == character) return (int)i;
    return -1;
}

static void remove_entry(uint32_t i) {
    dirty |= 1u << current.entries[i].character;
    memmove(&current.entries[i], &current.entries[i + 1],
            (current.count - i - 1) * sizeof current.entries[0]);
    memset(&current.entries[--current.count], 0, sizeof current.entries[0]);
}

static void required(void) {
    pthread_mutex_lock(&lock);
    bool needed = g_settings.quickstep_slot || current.count;
    pthread_mutex_unlock(&lock);
    quickstep_save_set_required(needed);
}

void quickstep_slot_adopt(uint64_t data, uint32_t story, const QuickstepSnapshot *snapshot) {
    pthread_mutex_lock(&lock);
    ++generation;
    playing = data; last_context = last_view = 0;
    active = range(data, INVENTORY + GEAR_END) && story < 2;
    memset(&current, 0, sizeof current); current.story = story;
    dirty = 0; refreshing = false; last_enabled = g_settings.quickstep_slot;
    if (active && snapshot && snapshot->story == story && snapshot->count <= QUICKSTEP_CHARACTER_CAPACITY) {
        for (uint32_t i = 0; i < snapshot->count; ++i) {
            const QuickstepEntry *entry = &snapshot->entries[i];
            if (!playable(story, entry->character) || entry_for(entry->character) >= 0) continue;
            bool duplicate = false;
            for (uint32_t j = 0; j < current.count; ++j) if (current.entries[j].uid == entry->uid) duplicate = true;
            if (!duplicate) current.entries[current.count++] = *entry;
        }
    }
    // Native saves include derived stats. Rebuild every playable character so
    // launcher-OFF and absent/missing metadata cannot retain a previous bonus.
    if (active) for (uint32_t ch = 1; ch < QUICKSTEP_CHARACTER_CAPACITY; ++ch)
        if (playable(story, ch)) dirty |= 1u << ch;
    pthread_mutex_unlock(&lock);
    required();
}

void quickstep_slot_suspend(void) {
    pthread_mutex_lock(&lock);
    active = false; playing = 0; last_context = last_view = 0; ++generation; dirty = 0;
    pthread_mutex_unlock(&lock);
}

void quickstep_slot_snapshot(QuickstepSnapshot *out) {
    if (!out) return;
    pthread_mutex_lock(&lock); *out = current; pthread_mutex_unlock(&lock);
}

bool quickstep_slot_snapshot_for_save(uint64_t raw, QuickstepSnapshot *out) {
    if (!out || raw < GUEST_LO + 0x6b0 || !range(raw, QUICKSTEP_SAVE_BYTES)) return false;
    pthread_mutex_lock(&lock);
    memset(out, 0, sizeof *out); out->story = current.story;
    bool valid = active && ((LD32(raw + 0x5ec) >> 22) & 1u) == current.story;
    if (valid) for (uint32_t i = 0; i < current.count; ++i) {
        QuickstepGem item;
        QuickstepEntry *entry = &current.entries[i];
        if (gem(raw - 0x6b0, entry->uid, &item) && item.availability == QUICKSTEP_FREE
                && !memcmp(item.fingerprint, entry->fingerprint, sizeof item.fingerprint))
            out->entries[out->count++] = *entry;
    }
    pthread_mutex_unlock(&lock);
    return valid;
}

void quickstep_slot_tick(Cpu *c, uint64_t context) {
    uint32_t story;
    uint64_t data = resolve(c, context, &story);
    if (!data) return;
    pthread_mutex_lock(&lock);
    if (!active || data != playing || refreshing) { pthread_mutex_unlock(&lock); return; }
    for (uint32_t i = 0; i < current.count;) {
        QuickstepGem item;
        QuickstepEntry *entry = &current.entries[i];
        if (!gem(data, entry->uid, &item) || item.availability != QUICKSTEP_FREE
                || memcmp(item.fingerprint, entry->fingerprint, sizeof entry->fingerprint)) remove_entry(i);
        else ++i;
    }
    if (last_enabled != g_settings.quickstep_slot) {
        for (uint32_t i = 0; i < current.count; ++i) dirty |= 1u << current.entries[i].character;
        last_enabled = g_settings.quickstep_slot;
    }
    uint32_t pending = dirty; dirty = 0;
    uint64_t epoch = generation;
    refreshing = true;
    pthread_mutex_unlock(&lock);
    required();
    for (uint32_t ch = 1; ch < QUICKSTEP_CHARACTER_CAPACITY; ++ch) {
        if (!(pending & (1u << ch))) continue;
        pthread_mutex_lock(&lock);
        bool same = active && playing == data && generation == epoch;
        pthread_mutex_unlock(&lock);
        if (!same) break;
        uint64_t character = call(c, 0x70000afab0ull, context, ch, 0, 0, 0);
        if (!range(character, 0x3480)) FATAL("[quickstep] invalid native character record\n");
        call(c, 0x700008b860ull, character + 4, context, ch, 0, 0);
    }
    pthread_mutex_lock(&lock);
    if (generation == epoch) refreshing = false;
    pthread_mutex_unlock(&lock);
}

size_t quickstep_slot_list(Cpu *c, uint64_t context, uint32_t character, QuickstepGem *out, size_t capacity) {
    quickstep_slot_tick(c, context);
    uint32_t story;
    uint64_t data = resolve(c, context, &story);
    if (!data || !playable(story, character)) return 0;
    size_t total = 0;
    pthread_mutex_lock(&lock);
    if (active && data == playing) for (uint32_t i = 0; i < QUICKSTEP_GEM_CAPACITY; ++i) {
        QuickstepGem item;
        if (!gem(data, 0x30000u | i, &item)) continue;
        item.character = character;
        for (uint32_t j = 0; j < current.count; ++j) if (current.entries[j].uid == item.uid) {
            item.assigned = current.entries[j].character == character;
            item.character = current.entries[j].character;
            if (!item.assigned) item.availability = QUICKSTEP_ASSIGNED_ELSEWHERE;
        }
        if (out && total < capacity) out[total] = item;
        ++total;
    }
    pthread_mutex_unlock(&lock);
    return total;
}

bool quickstep_slot_get(Cpu *c, uint64_t context, uint32_t character, QuickstepGem *out) {
    quickstep_slot_tick(c, context);
    uint32_t story;
    uint64_t data = resolve(c, context, &story);
    if (!out || !data || !playable(story, character)) return false;
    pthread_mutex_lock(&lock);
    int i = entry_for(character);
    bool found = active && data == playing && i >= 0 && gem(data, current.entries[i].uid, out);
    if (found) { out->character = character; out->assigned = true; }
    pthread_mutex_unlock(&lock);
    return found;
}

bool quickstep_slot_reference_matches(uint64_t context, uint32_t character,
                                     const QuickstepGem *expected, uint64_t *epoch) {
    if (!expected || !epoch || !range(context, 8)) return false;
    pthread_mutex_lock(&lock);
    int index = entry_for(character);
    QuickstepGem actual;
    bool valid = g_settings.quickstep_slot && active && playing && last_context == context
        && (!*epoch || *epoch == generation) && playable(current.story, character)
        && index >= 0 && current.entries[index].uid == expected->uid
        && !memcmp(current.entries[index].fingerprint, expected->fingerprint, 32)
        && gem(playing, expected->uid, &actual) && actual.availability == QUICKSTEP_FREE
        && !memcmp(actual.fingerprint, expected->fingerprint, 32);
    if (valid) *epoch = generation;
    pthread_mutex_unlock(&lock);
    return valid;
}

bool quickstep_slot_owner(uint64_t context, uint64_t instance,
                          QuickstepGem *out, uint64_t *epoch) {
    if (!out || !epoch || !range(context, 8) || !range(instance, GEM_STRIDE)) return false;
    pthread_mutex_lock(&lock);
    bool valid = g_settings.quickstep_slot && active && playing && last_context == context
        && range(last_view, 0x151) && ((LD8(last_view + 0x150) >> 3) & 1u) == current.story
        && (!*epoch || *epoch == generation);
    uint32_t uid = valid ? LD32(instance) : 0;
    int index = -1;
    if (valid && (uid & 0xffff0000u) == 0x30000u && (uid & 0xffffu) < QUICKSTEP_GEM_CAPACITY
            && instance == playing + INVENTORY + GEM_POOL + (uid & 0xffffu) * GEM_STRIDE) {
        for (uint32_t i = 0; i < current.count; ++i)
            if (current.entries[i].uid == uid) { index = (int)i; break; }
    }
    QuickstepGem item;
    valid = valid && index >= 0 && playable(current.story, current.entries[index].character)
        && gem(playing, uid, &item) && item.availability == QUICKSTEP_FREE
        && !memcmp(item.fingerprint, current.entries[index].fingerprint, sizeof item.fingerprint);
    if (valid) {
        item.character = current.entries[index].character;
        item.assigned = true;
        *out = item; *epoch = generation;
    }
    pthread_mutex_unlock(&lock);
    return valid;
}

void quickstep_slot_native_marker(Cpu *c) {
    // 232960 binds every real row through 174E80 -> 2327B0. The latter renders
    // the native E marker (none=-1, other character=0, current character=1,
    // unworn ordinary parent=3). Adapt this presentation result only; claiming
    // a native parent in 174F00 would break ordinary detach/transfer semantics.
    uint64_t view = c->x[20];
    if ((uint32_t)c->x[0] != UINT32_MAX || !range(view, 0x140) || LD32(view + 0x28) != 3) return;
    uint64_t context = LD64(view + 8), instance = LD64(view + 0x138), epoch = 0;
    QuickstepGem owner, checked;
    if (!quickstep_slot_owner(context, instance, &owner, &epoch)) return;
    // 174ED0 resolves the same native ownership cache used by 174E80. Its +8
    // character is the exact comparison native 174F00 uses for E-frame 0/1.
    uint64_t cache = call(c, 0x7000174ed0ull, context, 0, 0, 0, 0);
    if (!range(cache, 12) || !range(view, 0x140) || LD32(view + 0x28) != 3
            || LD64(view + 8) != context || LD64(view + 0x138) != instance
            || !quickstep_slot_owner(context, instance, &checked, &epoch)
            || checked.character != owner.character
            || memcmp(checked.fingerprint, owner.fingerprint, sizeof owner.fingerprint)) return;
    c->x[0] = LD32(cache + 8) == owner.character ? 1 : 0;
}

void quickstep_slot_native_portrait_owner(Cpu *c) {
    // Item Details2377E0 uses a different presentation-only native helper,
    //174AE0, returning a character ID rather than the row's E-frame status.
    // Leave the native portrait comparison, visibility, style and frame calls
    // intact; supplying an owner in the global inventory query would invent a
    // gear parent for ordinary transfer/removal code.
    uint64_t view = c->x[20], epoch = 0;
    if ((uint32_t)c->x[0] || !range(view, 0x140) || LD32(view + 0x28) != 3) return;
    QuickstepGem owner;
    if (quickstep_slot_owner(LD64(view + 8), LD64(view + 0x138), &owner, &epoch))
        c->x[0] = owner.character;
}

bool quickstep_slot_set_checked(Cpu *c, uint64_t context, uint32_t character, uint32_t uid, const uint8_t fingerprint[32]) {
    pthread_mutex_lock(&lock);
    uint64_t epoch = generation;
    pthread_mutex_unlock(&lock);
    quickstep_slot_tick(c, context);
    uint32_t story;
    uint64_t data = resolve(c, context, &story);
    if (!data || !g_settings.quickstep_slot || !playable(story, character)) return false;
    pthread_mutex_lock(&lock);
    QuickstepGem item;
    bool valid = active && data == playing && generation == epoch;
    if (uid) valid = valid && fingerprint && gem(data, uid, &item) && item.availability == QUICKSTEP_FREE
                        && !memcmp(item.fingerprint, fingerprint, sizeof item.fingerprint);
    for (uint32_t j = 0; valid && uid && j < current.count; ++j)
        if (current.entries[j].uid == uid && current.entries[j].character != character) valid = false;
    if (valid) {
        int i = entry_for(character);
        if (i >= 0) remove_entry((uint32_t)i);
        if (uid) {
            if (current.count >= QUICKSTEP_CHARACTER_CAPACITY) valid = false;
            else {
                QuickstepEntry *entry = &current.entries[current.count++];
                entry->character = character; entry->uid = uid;
                memcpy(entry->fingerprint, item.fingerprint, sizeof entry->fingerprint);
                dirty |= 1u << character;
            }
        }
    }
    pthread_mutex_unlock(&lock);
    if (valid) quickstep_slot_tick(c, context);
    return valid;
}

bool quickstep_slot_clear_checked(Cpu *c, uint64_t context, uint32_t character,
                                 uint32_t expected_uid, const uint8_t expected_fingerprint[32]) {
    if (!expected_uid || !expected_fingerprint) return false;
    pthread_mutex_lock(&lock);
    uint64_t epoch = generation;
    pthread_mutex_unlock(&lock);
    quickstep_slot_tick(c, context);
    uint32_t story;
    uint64_t data = resolve(c, context, &story);
    if (!data || !g_settings.quickstep_slot || !playable(story, character)) return false;
    pthread_mutex_lock(&lock);
    int index = entry_for(character);
    bool valid = active && data == playing && generation == epoch && index >= 0
                 && current.entries[index].uid == expected_uid
                 && !memcmp(current.entries[index].fingerprint, expected_fingerprint, 32);
    if (valid) remove_entry((uint32_t)index);
    pthread_mutex_unlock(&lock);
    if (valid) quickstep_slot_tick(c, context);
    return valid;
}

static EffectLease *lease(Cpu *c) {
    pthread_mutex_lock(&lock);
    EffectLease *result = NULL;
    for (uint32_t i = 0; i < LEASE_CAPACITY; ++i) {
        if (leases[i].busy && leases[i].cpu == c && leases[i].stack == c->sp)
            FATAL("[quickstep] duplicate native effect lease\n");
        if (!result && !leases[i].busy) result = &leases[i];
    }
    if (!result) FATAL("[quickstep] native effect lease capacity exhausted\n");
    if (!result->memory) result->memory = (uint64_t)(uintptr_t)hle_alloc(LEASE_BYTES);
    if (!range(result->memory, LEASE_BYTES) || (result->memory & 15))
        FATAL("[quickstep] invalid native effect scratch\n");
    result->cpu = c; result->stack = c->sp; result->busy = true; result->committed = false;
    result->generation = generation;
    result->original_array = c->x[19]; result->original_count = (uint32_t)c->x[21];
    result->original_x22 = c->x[22];
    uint64_t memory = result->memory;
    memset((void *)(uintptr_t)memory, 0, LEASE_BYTES);
    ST64(memory, canary); ST64(memory + LEASE_TAIL, canary);
    pthread_mutex_unlock(&lock);
    return result;
}

void quickstep_slot_effects_end(Cpu *c) {
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < LEASE_CAPACITY; ++i) {
        EffectLease *entry = &leases[i];
        if (!entry->busy || entry->cpu != c || entry->stack != c->sp) continue;
        if (LD64(entry->memory) != canary || LD64(entry->memory + LEASE_TAIL) != canary)
            FATAL("[quickstep] native effect scratch overrun\n");
        if (entry->committed) {
            // Restore original post-loop callee-saved values before releasing
            // scratch, including the original zero-count skip behavior.
            c->x[19] = entry->original_array + (entry->original_count ? 4 + entry->original_count * EFFECT_STRIDE : 0);
            c->x[21] = entry->original_count;
            c->x[22] = entry->original_count ? entry->original_count : entry->original_x22;
        }
        entry->busy = false; entry->cpu = NULL; entry->stack = 0;
    }
    pthread_mutex_unlock(&lock);
}

void quickstep_slot_effects(Cpu *c) {
    if (!g_settings.quickstep_slot) return;
    if (!range(c->sp, 0x240) || c->x[19] != c->sp + 0x98 || c->x[21] > NATIVE_EFFECTS
            || c->x[29] != c->sp + 0x2c0)
        FATAL("[quickstep] native effect-loop ABI changed\n");
    uint32_t story, character = LD32(c->sp + 0x58);
    uint64_t context = c->x[24], data = resolve(c, context, &story);
    if (!data || !playable(story, character)) return;
    // Temporary save/Continue metadata reconstruction can use the same root
    // context as gameplay. Only the confirmed playing character's own stats
    // object may receive the extra effect, never a preview or scan allocation.
    uint64_t stats = data + 0x154634 + (uint64_t)(character - 1) * 0x3480;
    QuickstepGem item;
    pthread_mutex_lock(&lock);
    int index = entry_for(character);
    bool valid = active && playing == data && index >= 0 && gem(data, current.entries[index].uid, &item)
                 && item.availability == QUICKSTEP_FREE
                 && !memcmp(item.fingerprint, current.entries[index].fingerprint, sizeof item.fingerprint);
    if (!valid && index >= 0) remove_entry((uint32_t)index);
    uint64_t epoch = generation;
    if (valid && c->x[20] != stats) {
        DeferredRefresh *slot = NULL;
        for (uint32_t i = 0; i < LEASE_CAPACITY; ++i) {
            if (deferred[i].busy && deferred[i].cpu == c && deferred[i].stack == c->sp)
                FATAL("[quickstep] duplicate temporary stats frame\n");
            if (!slot && !deferred[i].busy) slot = &deferred[i];
        }
        if (!slot) FATAL("[quickstep] temporary stats capacity exhausted\n");
        *slot = (DeferredRefresh){.cpu = c, .stack = c->sp, .context = context,
            .generation = epoch, .character = character, .busy = true};
        pthread_mutex_unlock(&lock);
        return;
    }
    pthread_mutex_unlock(&lock);
    if (!valid) return;
    EffectLease *entry = lease(c);
    uint64_t memory = entry->memory, array = memory + LEASE_ARRAY, input = memory + LEASE_INPUT;
    memcpy((void *)(uintptr_t)array, (void *)(uintptr_t)c->x[19], NATIVE_EFFECTS * EFFECT_STRIDE);
    ST32(memory + LEASE_COUNT, c->x[21]);
    if (!(call(c, 0x70000a8290ull, context, item.uid, 0, input + 4, input) & 1)
            || LD16(input) != 77) {
        quickstep_slot_effects_end(c); return;
    }
    // Decode/merge can yield. A load, native consumer or a newly reused pool
    // entry must invalidate the lease before it can reach the ordinary loop.
    pthread_mutex_lock(&lock);
    QuickstepGem check;
    valid = active && g_settings.quickstep_slot && playing == data && generation == epoch && entry->busy
            && entry->cpu == c && entry->stack == c->sp && entry->generation == epoch
            && gem(data, item.uid, &check) && check.availability == QUICKSTEP_FREE
            && !memcmp(item.fingerprint, check.fingerprint, sizeof item.fingerprint);
    index = entry_for(character);
    valid = valid && index >= 0 && current.entries[index].uid == item.uid
            && !memcmp(item.fingerprint, current.entries[index].fingerprint, sizeof item.fingerprint);
    pthread_mutex_unlock(&lock);
    if (!valid) { quickstep_slot_effects_end(c); return; }
    call(c, 0x700008a800ull, LD64(c->sp + 0x28), array, memory + LEASE_COUNT, input, 0);
    uint32_t count = LD32(memory + LEASE_COUNT);
    pthread_mutex_lock(&lock);
    valid = active && g_settings.quickstep_slot && playing == data && generation == epoch && entry->busy
            && entry->cpu == c && entry->stack == c->sp && entry->generation == epoch
            && gem(data, item.uid, &check) && check.availability == QUICKSTEP_FREE
            && !memcmp(item.fingerprint, check.fingerprint, sizeof item.fingerprint);
    index = entry_for(character);
    valid = valid && index >= 0 && current.entries[index].uid == item.uid
            && !memcmp(item.fingerprint, current.entries[index].fingerprint, sizeof item.fingerprint);
    if (valid) {
        if (count < 1 || count > EXTRA_EFFECTS || LD64(memory) != canary || LD64(memory + LEASE_TAIL) != canary)
            FATAL("[quickstep] invalid native effect merge\n");
        entry->committed = true;
        c->x[19] = array; c->x[21] = count;
    }
    pthread_mutex_unlock(&lock);
    if (!valid) { quickstep_slot_effects_end(c); return; }
}

void quickstep_slot_effects_finish(Cpu *c) {
    Cpu saved = *c;
    uint64_t fpcr = rt_get_fpcr(), fpsr = rt_get_fpsr(), context = 0;
    pthread_mutex_lock(&lock);
    for (uint32_t i = 0; i < LEASE_CAPACITY; ++i) {
        DeferredRefresh *entry = &deferred[i];
        if (!entry->busy || entry->cpu != c || entry->stack != c->sp) continue;
        // Consume before calling native code: the real-character refresh can
        // recursively reach this common epilogue, but has no deferred frame.
        entry->busy = false;
        if (active && generation == entry->generation) {
            dirty |= 1u << entry->character;
            context = entry->context;
        }
    }
    pthread_mutex_unlock(&lock);
    if (context) quickstep_slot_tick(c, context);
    *c = saved; rt_set_fpcr(fpcr); rt_set_fpsr(fpsr);
}

static bool invalidate_data(uint64_t data, uint64_t epoch, uint32_t uid) {
    bool changed = false;
    pthread_mutex_lock(&lock);
    if (active && playing == data && generation == epoch) {
        for (uint32_t i = 0; i < current.count;)
            if (current.entries[i].uid == uid) { remove_entry(i); changed = true; }
            else ++i;
    }
    pthread_mutex_unlock(&lock);
    return changed;
}

static void invalidate(Cpu *c, uint64_t context, uint32_t uid) {
    if ((uid & 0xffff0000u) != 0x30000u) return;
    pthread_mutex_lock(&lock);
    bool needed = active && current.count;
    uint64_t epoch = generation;
    pthread_mutex_unlock(&lock);
    if (!needed) return;
    uint32_t story;
    uint64_t data = resolve(c, context, &story);
    if (!data) return;
    if (invalidate_data(data, epoch, uid)) quickstep_slot_tick(c, context);
}

void quickstep_slot_native_equip(Cpu *c) {
    // 17B81C, immediately before the checked native socket write. Native
    // 17B7A0 already rejected out-of-range and intrinsic slots. In particular,
    // BDEE0/175310 favourite changes must never be routed here.
    uint32_t uid = (uint32_t)c->x[19];
    if ((uid & 0xffff0000u) != 0x30000u || c->x[9] > 16 || (c->x[9] & 7)
            || c->x[8] < GUEST_LO + 0x18) return;
    uint64_t item = c->x[8] - 0x18;
    pthread_mutex_lock(&lock);
    uint64_t data = playing, context = last_context, epoch = generation;
    bool candidate = false;
    if (active && current.count) for (size_t p = 0; p < sizeof gear_pools / sizeof gear_pools[0]; ++p)
        for (unsigned special = 0; special < 2; ++special) {
            uint64_t start = data + INVENTORY + (special ? gear_pools[p].special : gear_pools[p].normal);
            uint32_t count = special ? SPECIAL_GEAR_CAPACITY : GEAR_CAPACITY;
            if (item >= start && item < start + count * GEAR_STRIDE
                    && (item - start) % GEAR_STRIDE == 0 && range(item, GEAR_STRIDE)
                    && LD8(item + 0x10) && LD16(item + 6) == gear_pools[p].type
                    && LD8(item + 0x15) <= 3 && c->x[9] / 8 < LD8(item + 0x15)
                    && !LD8(item + 0x18 + c->x[9] + 6)) candidate = true;
        }
    pthread_mutex_unlock(&lock);
    if (!candidate || !invalidate_data(data, epoch, uid)) return;
    required();
    if (context) quickstep_slot_tick(c, context);
}

void quickstep_slot_native_consume(Cpu *c) {
    // BDF40 subtracts a 16-bit quantity. A zero low half (including 0x10000)
    // consumes no item; a same-UID preview allocation is not the playing pool.
    if (!(uint16_t)c->x[2] || !range(c->x[1], GEM_STRIDE)) return;
    uint32_t uid = LD32(c->x[1]), story;
    if ((uid & 0xffff0000u) != 0x30000u || (uid & 0xffffu) >= QUICKSTEP_GEM_CAPACITY) return;
    pthread_mutex_lock(&lock);
    uint64_t epoch = generation;
    pthread_mutex_unlock(&lock);
    uint64_t data = resolve(c, c->x[0], &story);
    if (!data || c->x[1] != data + INVENTORY + GEM_POOL + (uid & 0xffffu) * GEM_STRIDE) return;
    if (invalidate_data(data, epoch, uid)) quickstep_slot_tick(c, c->x[0]);
}

void quickstep_slot_native_destroy(Cpu *c) {
    pthread_mutex_lock(&lock);
    uint64_t data = playing, context = last_context, item = c->x[1];
    uint64_t start = data + INVENTORY + GEM_POOL;
    bool candidate = active && current.count && c->x[0] == data + INVENTORY
                     && item >= start && item < start + GEM_STRIDE * QUICKSTEP_GEM_CAPACITY
                     && (item - start) % GEM_STRIDE == 0 && range(item, GEM_STRIDE);
    pthread_mutex_unlock(&lock);
    if (candidate && context) invalidate(c, context, LD32(item));
}

RT_ON_RESET(quickstep_slot_reset) {
    pthread_mutex_lock(&lock);
    active = false; playing = 0; last_context = last_view = 0; dirty = 0; refreshing = false; ++generation;
    memset(&current, 0, sizeof current);
    memset(leases, 0, sizeof leases);
    memset(deferred, 0, sizeof deferred);
    pthread_mutex_unlock(&lock);
}
