// [XCDE] LINUX-035: authored adapters around native asynchronous save tasks.
// Existing filenames, validation, journaled filesystem, SaveData copy operations
// and Continue's native full-payload variant remain owned by the guest. Only
// compact scan progress is retained here; no game implementation is embedded.
#include "../hle.h"
#include "expanded_save_scan.h"
#include "expanded_saves.h"
#include <string.h>

enum {
    MANUAL_LAST = 98, EXTRA_FIRST = 3, SCAN_OWNERS = 8,
    SAVE_BYTES = 0x153f30,
    CONTINUE_TASK = 0xa0, CONTINUE_DATA = 0x154140,
    CONTINUE_VALID = 0x6a3e00, CONTINUE_SIZE = 0x6a3e04,
    CHECK_TASK = 0x10, CHECK_DECODED = 0x1540a0,
    CHECK_PENDING = 0x304030, CHECK_FLAGS = 0x304038,
    CHECK_SIZE = 0x304050,
};

typedef enum { SCAN_CONTINUE = 1, SCAN_PRESENCE = 2 } ScanKind;
typedef struct {
    uint64_t owner, generation, best_time;
    uint16_t next_file, winner_file;
    uint8_t kind, story, best_physical;
    bool waiting, complete, found, winner_auto, restart;
} Scan;

static pthread_mutex_t scan_lock = PTHREAD_MUTEX_INITIALIZER;
static Scan scans[SCAN_OWNERS];
static uint64_t scan_generation;

static bool guest_range(uint64_t address, uint64_t size) {
    return address >= GUEST_LO && address < GUEST_HI && size <= GUEST_HI - address;
}

static void owner_check(uint64_t owner, uint64_t size) {
    if ((owner & 7) || !guest_range(owner, size))
        FATAL("[expanded-saves] invalid native title scan owner\n");
}

static bool snapshot(uint64_t owner, ScanKind kind, Scan *result) {
    bool found = false;
    pthread_mutex_lock(&scan_lock);
    for (unsigned i = 0; i < SCAN_OWNERS; ++i)
        if (scans[i].owner == owner && scans[i].kind == kind) {
            *result = scans[i];
            found = true;
            break;
        }
    pthread_mutex_unlock(&scan_lock);
    return found;
}

static Scan create(uint64_t owner, ScanKind kind) {
    Scan result = { .owner = owner, .kind = kind, .next_file = EXTRA_FIRST };
    bool inserted = false;
    pthread_mutex_lock(&scan_lock);
    for (unsigned i = 0; i < SCAN_OWNERS; ++i)
        if (!scans[i].owner) {
            result.generation = ++scan_generation;
            scans[i] = result;
            inserted = true;
            break;
        }
    pthread_mutex_unlock(&scan_lock);
    if (!inserted) FATAL("[expanded-saves] native title scan owner limit\n");
    return result;
}

static bool publish(const Scan *state) {
    bool current = false;
    pthread_mutex_lock(&scan_lock);
    for (unsigned i = 0; i < SCAN_OWNERS; ++i)
        if (scans[i].owner == state->owner && scans[i].kind == state->kind
                && scans[i].generation == state->generation) {
            // A reset can arrive while a guest helper yields. Never lose it.
            bool restart = scans[i].restart;
            scans[i] = *state;
            scans[i].restart |= restart;
            current = true;
            break;
        }
    pthread_mutex_unlock(&scan_lock);
    return current;
}

static bool generation_current(const Scan *state) {
    Scan latest;
    return snapshot(state->owner, state->kind, &latest)
        && latest.generation == state->generation;
}

static bool current(const Scan *state) {
    Scan latest;
    return snapshot(state->owner, state->kind, &latest)
        && latest.generation == state->generation && !latest.restart;
}

static void forget(uint64_t owner, ScanKind kind) {
    pthread_mutex_lock(&scan_lock);
    for (unsigned i = 0; i < SCAN_OWNERS; ++i)
        if (scans[i].owner == owner && scans[i].kind == kind)
            memset(&scans[i], 0, sizeof scans[i]);
    pthread_mutex_unlock(&scan_lock);
}

// Keep the complete CPU/FP/TLS state and the caller's stack intact. No registry
// lock is held across a guest call, which may yield or unwind during shutdown.
static uint64_t call(Cpu *c, uint64_t fn, uint64_t x0, uint64_t x1,
                     uint64_t x2, uint64_t x3) {
    Cpu saved = *c;
    uint64_t fpcr = rt_get_fpcr(), fpsr = rt_get_fpsr();
    if (!guest_range(c->sp, 1) || c->sp < GUEST_LO + 0x400)
        FATAL("[expanded-saves] invalid native scan helper stack\n");
    rt_unwind_point();
    c->x[0] = x0; c->x[1] = x1; c->x[2] = x2; c->x[3] = x3;
    c->sp = (c->sp - 0x400) & ~15ull;
    guest_call(c, fn);
    uint64_t result = c->x[0];
    *c = saved;
    rt_set_fpcr(fpcr);
    rt_set_fpsr(fpsr);
    return result;
}

static uint64_t story_state(Cpu *c, uint64_t task) {
    // The native filename builder passes the task's context-reference field.
    uint64_t state = call(c, 0x7000592a30ull, task + 8, 0, 0, 0);
    if (!guest_range(state, 0x158))
        FATAL("[expanded-saves] invalid native story state\n");
    return state;
}

static void queue(Cpu *c, uint64_t task, uint16_t file, uint8_t story) {
    uint64_t state = story_state(c, task), old = LD64(state + 0x150);
    ST64(state + 0x150, (old & ~8ull) | (story ? 8ull : 0ull));
    // The normal native call formats/captures its filename before queuing IO,
    // exactly as the existing Future Connected availability scan does.
    call(c, 0x7000087040ull, task, file, 0, 0);
    uint64_t now = LD64(state + 0x150);
    ST64(state + 0x150, (now & ~8ull) | (old & 8ull));
}

void xcde_expanded_scan_continue_begin(uint64_t sequence) {
    owner_check(sequence, CONTINUE_SIZE);
    forget(sequence, SCAN_CONTINUE);
}

void xcde_expanded_scan_continue_drop(uint64_t sequence) {
    if (sequence) forget(sequence, SCAN_CONTINUE);
}

bool xcde_expanded_scan_continue_wait(Cpu *c) {
    uint64_t owner = c->x[19];
    owner_check(owner, CONTINUE_SIZE);
    if (LD8(owner + 0x98)) return false; // Native child/confirmation still busy.
    Scan state;
    uint64_t task = owner + CONTINUE_TASK;
    if (!snapshot(owner, SCAN_CONTINUE, &state)) {
        state = create(owner, SCAN_CONTINUE);
        state.story = (LD64(story_state(c, task) + 0x150) >> 3) & 1;
        if (!current(&state)) return true;
        // Match the native chooser's strict unsigned timestamp comparison,
        // including its treatment of zero timestamps and stable ties.
        for (unsigned i = 0; i < 4; ++i) {
            uint64_t stamp = LD64(owner + CONTINUE_DATA + i * SAVE_BYTES + 8);
            if (LD8(owner + CONTINUE_VALID + i) && stamp > state.best_time) {
                state.best_time = stamp; state.best_physical = i;
                state.winner_auto = i == 0;
                state.winner_file = i ? i - 1 : 0;
                state.found = true;
            }
        }
    }
    if (state.complete) return false;
    if (state.waiting) {
        if (call(c, 0x70000863f0ull, task, 0, 0, 0) & 1) return true;
        if (!current(&state)) return true;
        if (call(c, 0x7000086400ull, task, 0, 0, 0) & 1) {
            uint64_t source = call(c, 0x7000086e70ull, task, 0, 0, 0);
            if (!current(&state)) return true;
            if (!guest_range(source, SAVE_BYTES))
                FATAL("[expanded-saves] invalid native Continue payload\n");
            uint64_t stamp = LD64(source + 8);
            if (stamp > state.best_time) {
                uint64_t dest = owner + CONTINUE_DATA + state.best_physical * SAVE_BYTES;
                // Native field-aware copy into an already constructed object.
                call(c, 0x700039a100ull, dest, source, 0, 0);
                if (!current(&state)) return true;
                ST8(owner + CONTINUE_VALID + state.best_physical, 1);
                state.best_time = stamp; state.winner_file = state.next_file;
                state.winner_auto = false; state.found = true;
            }
        }
        if (!current(&state)) return true;
        state.waiting = false;
        ++state.next_file;
    }
    if (state.next_file > MANUAL_LAST) {
        state.complete = true;
        return !publish(&state);
    }
    // Publish ownership before the native queue helper can yield to a reset.
    state.waiting = true;
    if (!publish(&state)) return true;
    queue(c, task, state.next_file, state.story);
    return true;
}

void xcde_expanded_scan_continue_selected(Cpu *c) {
    uint64_t owner = c->x[19];
    Scan state;
    if (!snapshot(owner, SCAN_CONTINUE, &state) || !state.complete || !state.found)
        FATAL("[expanded-saves] Continue selected before its scan completed\n");
    uint64_t holder = LD64(owner + 0x10);
    if (!guest_range(holder, 0x30))
        FATAL("[expanded-saves] invalid native Continue context holder\n");
    uint64_t context = LD64(holder + 0x28);
    if (context != c->x[9] || !guest_range(context, 0x18de4)
            || (uint32_t)c->x[8] != state.best_physical)
        FATAL("[expanded-saves] native Continue winner changed during scan\n");
    ST32(context + 0x18de0, state.winner_auto ? 0 : state.winner_file + 1);
    xcde_expanded_saves_remember(state.story, state.winner_file, state.winner_auto);
}

void xcde_expanded_scan_presence_drop(uint64_t checker) {
    if (checker) forget(checker, SCAN_PRESENCE);
}

void xcde_expanded_scan_presence_reset(uint64_t checker) {
    if (!checker) return;
    pthread_mutex_lock(&scan_lock);
    for (unsigned i = 0; i < SCAN_OWNERS; ++i)
        if (scans[i].owner == checker && scans[i].kind == SCAN_PRESENCE) {
            // A reused checker must first drain the task it already owns.
            if (scans[i].waiting) scans[i].restart = true;
            else memset(&scans[i], 0, sizeof scans[i]);
            break;
        }
    pthread_mutex_unlock(&scan_lock);
}

void xcde_expanded_scan_presence(Cpu *c) {
    uint64_t owner = c->x[0];
    owner_check(owner, CHECK_SIZE);
    uint64_t task = owner + CHECK_TASK;
    Scan state;
    bool known = snapshot(owner, SCAN_PRESENCE, &state);
    if (known && state.restart) {
        if (state.waiting && (call(c, 0x70000863f0ull, task, 0, 0, 0) & 1)) return;
        if (!generation_current(&state)) return;
        forget(owner, SCAN_PRESENCE);
        known = false;
    }
    if (!known || state.complete) {
        // Preserve all native base-slot validation and its existing startup UI.
        call(c, 0x70003c2880ull, owner, 0, 0, 0);
        uint32_t flags = LD32(owner + CHECK_FLAGS);
        if (known || (flags & 3) != 3) return;
        if ((int32_t)LD32(owner + CHECK_PENDING) != -1)
            FATAL("[expanded-saves] native title scan completed with pending IO\n");
        state = create(owner, SCAN_PRESENCE);
        ST32(owner + CHECK_FLAGS, flags & ~2u);
    }
    if (state.waiting) {
        if (call(c, 0x70000863f0ull, task, 0, 0, 0) & 1) return;
        if (!current(&state)) return;
        if (call(c, 0x7000086400ull, task, 0, 0, 0) & 1) {
            uint32_t flags = LD32(owner + CHECK_FLAGS) | (state.story ? 0x10u : 8u);
            bool decode_main = false;
            if (!state.story) {
                uint64_t native_story = story_state(c, task);
                if (!current(&state)) return;
                // The native FC title path records main-file availability but
                // skips main-story reconstruction and its clear check.
                decode_main = !(LD64(native_story + 0x150) & 8ull);
            }
            if (decode_main) {
                uint64_t source = call(c, 0x7000086e70ull, task, 0, 0, 0);
                if (!current(&state)) return;
                if (!guest_range(source, SAVE_BYTES))
                    FATAL("[expanded-saves] invalid native title availability payload\n");
                call(c, 0x7000073f00ull, owner + CHECK_DECODED, LD64(owner), source, 0);
                if (!current(&state)) return;
                if (call(c, 0x70000ccd80ull, owner + CHECK_DECODED, 0, 0, 0) & 1)
                    flags |= 0x14u;
            }
            if (!current(&state)) return;
            ST32(owner + CHECK_FLAGS, flags & ~2u);
        }
        if (!current(&state)) return;
        state.waiting = false;
        ++state.next_file;
    }
    if (state.next_file > MANUAL_LAST && !state.story) {
        state.story = 1;
        state.next_file = EXTRA_FIRST;
    }
    if (state.next_file > MANUAL_LAST) {
        state.complete = true;
        if (publish(&state)) ST32(owner + CHECK_FLAGS, LD32(owner + CHECK_FLAGS) | 2u);
        return;
    }
    // A reset during the native queue call must drain, rather than replace, IO.
    state.waiting = true;
    if (!publish(&state)) return;
    queue(c, task, state.next_file, state.story);
}

RT_ON_RESET(expanded_save_scan_reset) {
    pthread_mutex_lock(&scan_lock);
    memset(scans, 0, sizeof scans);
    ++scan_generation;
    pthread_mutex_unlock(&scan_lock);
}
