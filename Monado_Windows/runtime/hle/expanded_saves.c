// [XCDE] LINUX-035: extend the native logical records while retaining its four
// physical cards, sorting, async file jobs and save transactions. This contains
// authored layout adaptation only; no game implementation or assets.
#include "../hle.h"
#include "expanded_saves.h"

enum { UI_STATE = 0x90, UI_VISIBLE = 0x88, UI_MODE = 0x8c,
       UI_TOTAL = 0x9c, UI_SELECTED = 0xa8, SEQUENCE_UI = 0x130 };

// [XCDE] LINUX-036: bounded cold-read acceleration, never a filesystem cache.
typedef struct {
    uint64_t started, extra;
    uint32_t batches, busy, callback, stalled, budget, limit;
} SaveScanStats;

typedef struct SaveBinding {
    struct SaveBinding *next;
    uint64_t state;
    uint32_t story;
    bool live, initializing;
    uint64_t generation, pump_ui;
    bool initial_scan, pumping, summary_logged;
    SaveScanStats scan;
} SaveBinding;

static pthread_mutex_t save_lock = PTHREAD_MUTEX_INITIALIZER;
static SaveBinding *bindings;
static int32_t remembered[2] = {-1, -1};
static uint64_t binding_generation; // LINUX-036: never reset; rejects stale reuse.

_Static_assert(XCDE_SAVE_RECORD_BASE + XCDE_SAVE_RECORD_COUNT * XCDE_SAVE_RECORD_STRIDE
               == XCDE_SAVE_RECORD_END, "save record end");
_Static_assert(XCDE_SAVE_RECORD_END < XCDE_SAVE_COUNT_OFFSET, "save metadata arena");
_Static_assert(XCDE_SAVE_THUMB_STRIDE * XCDE_SAVE_RECORD_COUNT == XCDE_SAVE_THUMB_BYTES,
               "save thumbnail extent");
_Static_assert(XCDE_SAVE_THUMB_BASE + XCDE_SAVE_THUMB_BYTES == XCDE_SAVE_STATE_BYTES,
               "save allocation extent");

static bool range(uint64_t address, uint64_t bytes) {
    return address >= GUEST_LO && address < GUEST_HI && bytes <= GUEST_HI - address;
}

static uint64_t native_call(Cpu *c, uint64_t function, uint64_t x0, uint64_t x1) {
    Cpu saved = *c;
    uint64_t fpcr = rt_get_fpcr(), fpsr = rt_get_fpsr();
    if ((c->sp & 15) || c->sp < GUEST_LO + 0x400 || !range(c->sp - 0x400, 0x400))
        FATAL("[expanded-saves] invalid native helper stack\n");
    c->x[0] = x0;
    c->x[1] = x1;
    c->sp -= 0x400;
    // Never hold save_lock here: guest callbacks can yield or re-enter hooks.
    guest_call(c, function);
    uint64_t result = c->x[0];
    *c = saved;
    rt_set_fpcr(fpcr);
    rt_set_fpsr(fpsr);
    return result;
}

uint32_t xcde_expanded_saves_story(Cpu *c, uint64_t reference) {
    if (!range(reference, 8) || !range(LD64(reference), 8))
        FATAL("[expanded-saves] invalid native story context\n");
    uint64_t view = native_call(c, 0x7000592a30ull, reference, 0);
    if (!range(view, 0x151)) FATAL("[expanded-saves] invalid native story view\n");
    return (LD8(view + 0x150) >> 3) & 1u;
}

static bool find_state(uint64_t state, uint32_t *story) {
    bool found = false;
    pthread_mutex_lock(&save_lock);
    for (SaveBinding *b = bindings; b; b = b->next)
        if (b->live && b->state == state) { *story = b->story; found = true; break; }
    pthread_mutex_unlock(&save_lock);
    return found;
}

static uint64_t checked_ui(uint64_t ui, uint32_t *story) {
    if (!range(ui, 0xb0) || (ui & 7))
        FATAL("[expanded-saves] invalid native save UI\n");
    uint64_t state = LD64(ui + UI_STATE);
    if (!find_state(state, story) || !range(state, XCDE_SAVE_STATE_BYTES)
            || LD64(ui) != LD64(state) || LD32(ui + UI_VISIBLE) != XCDE_SAVE_VISIBLE_COUNT
            || LD32(ui + UI_TOTAL) != XCDE_SAVE_RECORD_COUNT || LD32(ui + UI_MODE) > 1
            || LD32(state + XCDE_SAVE_COUNT_OFFSET) != XCDE_SAVE_RECORD_COUNT)
        FATAL("[expanded-saves] native save UI ownership/layout mismatch\n");
    return state;
}

void xcde_expanded_saves_initialize(Cpu *c) {
    uint64_t state = c->x[20];
    if ((state & 15) || !range(state, XCDE_SAVE_STATE_BYTES)
            || c->x[0] != state + XCDE_SAVE_RECORD_BASE
            || LD32(state + XCDE_SAVE_COUNT_OFFSET) != XCDE_SAVE_RECORD_COUNT)
        FATAL("[expanded-saves] native save allocation/layout mismatch\n");
    // This checked site runs once after a fresh native constructor/reset, before
    // any asynchronous metadata load. No borrowed or loaded string can alias.
    for (uint32_t i = 0; i < 4; ++i) {
        uint64_t record = state + XCDE_SAVE_RECORD_BASE + i * XCDE_SAVE_RECORD_STRIDE;
        if (LD64(record + 0x18) || LD32(record + 0x3c) || LD32(record + 0x40) != 1
                || LD16(record + 0x44) != (i ? i - 1 : 0)
                || LD8(record + 0x46) != (i == 0) || LD8(record + 0x47)
                || LD8(record + 0x48) || LD8(record + 0x49) != 1 || LD8(record + 0x4a))
            FATAL("[expanded-saves] native empty-record template changed\n");
    }
    pthread_mutex_lock(&save_lock);
    SaveBinding *b;
    for (b = bindings; b; b = b->next) {
        if ((b->live || b->initializing) && b->state == state) {
            pthread_mutex_unlock(&save_lock);
            FATAL("[expanded-saves] duplicate live native save state\n");
        }
    }
    for (b = bindings; b && (b->live || b->initializing); b = b->next) {}
    if (!b) {
        b = calloc(1, sizeof *b);
        if (!b) FATAL("[expanded-saves] allocating state ownership failed\n");
        b->next = bindings;
        bindings = b;
    }
    b->state = state;
    b->initializing = true;
    b->generation = ++binding_generation;
    b->pump_ui = 0;
    b->pumping = false;
    b->initial_scan = true;
    b->summary_logged = false;
    memset(&b->scan, 0, sizeof b->scan);
    b->scan.started = plat_now_ns();
    pthread_mutex_unlock(&save_lock);
    uint32_t story = xcde_expanded_saves_story(c, state);
    // Reserve ownership before the native lookup or any extended-record write.
    // A duplicate/reentrant initialization must never overwrite live metadata.
    pthread_mutex_lock(&save_lock);
    if (!b->initializing || b->state != state) {
        pthread_mutex_unlock(&save_lock);
        FATAL("[expanded-saves] native save initialization lost ownership\n");
    }
    const void *empty = (const void *)(uintptr_t)(state + XCDE_SAVE_RECORD_BASE + XCDE_SAVE_RECORD_STRIDE);
    for (uint32_t i = 4; i < XCDE_SAVE_RECORD_COUNT; ++i) {
        uint64_t record = state + XCDE_SAVE_RECORD_BASE + i * XCDE_SAVE_RECORD_STRIDE;
        memcpy((void *)(uintptr_t)record, empty, XCDE_SAVE_RECORD_STRIDE);
        ST16(record + 0x44, i - 1);
    }
    b->story = story;
    b->initializing = false;
    b->live = true;
    pthread_mutex_unlock(&save_lock);
    // Native reset's old thumbnail base also served as sort end. The relocated
    // thumbnail arena is no longer adjacent to records; supply the true end.
    c->x[1] = state + XCDE_SAVE_RECORD_END;
}

void xcde_expanded_saves_forget(uint64_t state) {
    // LINUX-036: collect one bounded summary without retaining a guest pointer.
    SaveScanStats scan = {0};
    uint32_t story = 0;
    bool report = false;
    pthread_mutex_lock(&save_lock);
    for (SaveBinding *b = bindings; b; b = b->next)
        if ((b->live || b->initializing) && b->state == state) {
            if (b->live && !b->summary_logged) {
                scan = b->scan; story = b->story; report = true;
            }
            b->live = b->initializing = false; b->state = 0; break;
        }
    pthread_mutex_unlock(&save_lock);
    if (report) {
        uint64_t now = plat_now_ns();
        LOG("[expanded-saves] initial scan story=%u elapsed_us=%llu extra=%llu batches=%u busy=%u callback=%u stalled=%u budget=%u limit=%u stop=closed\n",
            story, (unsigned long long)(now >= scan.started ? (now - scan.started) / 1000 : 0),
            (unsigned long long)scan.extra, scan.batches, scan.busy, scan.callback,
            scan.stalled, scan.budget, scan.limit);
    }
}

// [XCDE] LINUX-036: exact metadata progress, not a hash or a guessed completed
// counter. Task callback completion is separately guarded before each call.
typedef struct {
    uint64_t context, ui_state, jobs[2];
    uint32_t ui_flags, visible, mode, total, model_visible, count, flags;
    uint32_t pending, phase, direction[2], task_flags[2];
    uint32_t records[XCDE_SAVE_RECORD_COUNT][4];
} SaveScanProgress;

enum { SCAN_UNKNOWN, SCAN_READY, SCAN_COMPLETE, SCAN_UNSAFE, SCAN_PAUSED };
enum { STOP_BUSY, STOP_CALLBACK, STOP_STALLED, STOP_BUDGET, STOP_LIMIT, STOP_EXTRA };

static SaveBinding *scan_binding(uint64_t state, uint64_t generation) {
    for (SaveBinding *b = bindings; b; b = b->next)
        if (b->live && b->state == state && b->generation == generation) return b;
    return NULL; // Caller owns save_lock; no host pointer survives guest_call.
}

static bool scan_current(uint64_t state, uint64_t generation, uint64_t ui) {
    pthread_mutex_lock(&save_lock);
    SaveBinding *b = scan_binding(state, generation);
    bool current = b && b->pumping && b->initial_scan && b->pump_ui == ui;
    pthread_mutex_unlock(&save_lock);
    return current;
}

static int scan_progress(uint64_t ui, uint64_t state, SaveScanProgress *p) {
    memset(p, 0, sizeof *p);
    p->context = LD64(ui);
    p->ui_state = LD64(ui + UI_STATE);
    if (p->ui_state != state || p->context != LD64(state)) return SCAN_UNKNOWN;
    p->ui_flags = LD32(ui + 0x98);
    p->visible = LD32(ui + UI_VISIBLE);
    p->mode = LD32(ui + UI_MODE);
    p->total = LD32(ui + UI_TOTAL);
    p->model_visible = LD32(ui + 0xa0);
    p->count = LD32(state + XCDE_SAVE_COUNT_OFFSET);
    p->flags = LD32(state + XCDE_SAVE_FLAGS_OFFSET);
    // The model is bound only AFTER the cold scan, in native f283730. During
    // initialization these two model counts are zero, not100/four.
    if (p->visible != 4 || p->mode > 1 || p->count != 100 || p->total || p->model_visible
            || !(p->ui_flags & 1)) return SCAN_UNSAFE;
    p->pending = LD32(state + 0x1541e0);
    p->phase = LD32(state + 0x1541e4);
    unsigned remaining = 0;
    for (unsigned i = 0; i < XCDE_SAVE_RECORD_COUNT; ++i) {
        uint64_t record = state + XCDE_SAVE_RECORD_BASE + i * XCDE_SAVE_RECORD_STRIDE;
        for (unsigned f = 0; f < 4; ++f) p->records[i][f] = LD32(record + 0x3c + f * 4);
        uint32_t request = p->records[i][1], identity = p->records[i][2];
        if ((request != 1 && request != 4)
                || (identity & 0xffffffu) != (i ? i - 1 : 0x10000u)) return SCAN_UNSAFE;
        remaining += request == 1;
    }
    if (!remaining) return SCAN_COMPLETE;
    // Initial constructors also permit bit3; the NG+ path sets bit6 before
    // reading. Capture/completion/cancel bits and unknown future flags fall back.
    if ((p->flags & 7) != (p->mode ? 3u : 5u) || (p->flags & ~0x4fu)) return SCAN_UNSAFE;
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t task = state + (i ? 0x1540a0 : 0x10);
        p->jobs[i] = LD64(task + 0x118);
        p->direction[i] = LD32(task + 0x10);
        p->task_flags[i] = LD32(task + 0x13c);
        // Native constructor uses0; metadata/thumbnail reads use2. WRITE is1,
        // and an undocumented direction must never gain extra native cadence.
        if (p->direction[i] != 0 && p->direction[i] != 2) return SCAN_UNSAFE;
    }
    // Known native thumbnail reads keep their original one-update cadence.
    // They can pause cold metadata work without permanently disabling it.
    if (p->phase == 3 || p->phase == 4) return SCAN_PAUSED;
    uint16_t file = p->pending & 0xffffu;
    uint8_t alternate = (p->pending >> 16) & 0xffu;
    if (!p->phase) {
        if (file != 0x7fff || alternate) return SCAN_UNSAFE;
    } else {
        if (p->phase > 2 || alternate > 1 || file >= XCDE_SAVE_MANUAL_COUNT || (alternate && file))
            return SCAN_UNSAFE;
        unsigned index = alternate ? 0 : file + 1;
        if (p->records[index][1] != 1) return SCAN_UNSAFE;
    }
    return SCAN_READY;
}

static void scan_count(uint64_t state, uint64_t generation, unsigned event) {
    pthread_mutex_lock(&save_lock);
    SaveBinding *b = scan_binding(state, generation);
    if (b) switch (event) {
        case STOP_BUSY: ++b->scan.busy; break;
        case STOP_CALLBACK: ++b->scan.callback; break;
        case STOP_STALLED: ++b->scan.stalled; break;
        case STOP_BUDGET: ++b->scan.budget; break;
        case STOP_LIMIT: ++b->scan.limit; break;
        case STOP_EXTRA: ++b->scan.extra; break;
    }
    pthread_mutex_unlock(&save_lock);
}

static void scan_finish(uint64_t state, uint64_t generation, int status) {
    SaveScanStats scan = {0};
    uint32_t story = 0;
    bool report = false;
    pthread_mutex_lock(&save_lock);
    SaveBinding *b = scan_binding(state, generation);
    if (b) {
        b->pumping = false;
        if (status == SCAN_UNKNOWN) b->pump_ui = 0;
        if (status == SCAN_COMPLETE || status == SCAN_UNSAFE) {
            b->initial_scan = false; // Never re-enable after Save/write/thumbnail phases.
            if (!b->summary_logged) {
                b->summary_logged = true; scan = b->scan; story = b->story; report = true;
            }
        }
    }
    pthread_mutex_unlock(&save_lock);
    if (report) {
        uint64_t now = plat_now_ns();
        LOG("[expanded-saves] initial scan story=%u elapsed_us=%llu extra=%llu batches=%u busy=%u callback=%u stalled=%u budget=%u limit=%u stop=%s\n",
            story, (unsigned long long)(now >= scan.started ? (now - scan.started) / 1000 : 0),
            (unsigned long long)scan.extra, scan.batches, scan.busy, scan.callback,
            scan.stalled, scan.budget, scan.limit, status == SCAN_COMPLETE ? "complete" : "ineligible");
    }
}

static void scan_pump(Cpu *c, uint64_t ui, uint64_t state) {
    if ((ui & 7) || !range(ui, 0xb0) || (state & 15) || !range(state, XCDE_SAVE_STATE_BYTES)) return;
    uint64_t generation = 0;
    pthread_mutex_lock(&save_lock);
    for (SaveBinding *b = bindings; b; b = b->next) {
        if (!b->live || b->state != state || !b->initial_scan || b->pumping
                || (b->pump_ui && b->pump_ui != ui)) continue;
        b->pump_ui = ui; b->pumping = true; generation = b->generation; ++b->scan.batches;
        break;
    }
    pthread_mutex_unlock(&save_lock);
    if (!generation) return;
    uint64_t start = plat_now_ns();
    int status = SCAN_UNKNOWN;
    for (unsigned step = 0; step < XCDE_SAVE_SCAN_MAX_EXTRA; ++step) {
        if (!scan_current(state, generation, ui)) break;
        SaveScanProgress before, after;
        status = scan_progress(ui, state, &before);
        if (status != SCAN_READY) break;
        uint64_t now = plat_now_ns();
        if (now < start || now - start >= XCDE_SAVE_SCAN_BUDGET_NS) {
            scan_count(state, generation, STOP_BUDGET); break;
        }
        bool wait = false;
        for (unsigned i = 0; i < 2; ++i) {
            uint64_t task = state + (i ? 0x1540a0 : 0x10);
            uint32_t busy = native_call(c, 0x70000863f0ull, task, 0);
            if (!scan_current(state, generation, ui)) { wait = true; break; }
            if (busy) { scan_count(state, generation, STOP_BUSY); wait = true; break; }
            // f86990 clears busy BEFORE finishing validation/copy callbacks.
            // Its64-bit job identity is cleared only at callback exit.
            if (LD64(task + 0x118)) {
                scan_count(state, generation, STOP_CALLBACK); wait = true; break;
            }
        }
        if (wait || !scan_current(state, generation, ui)) break;
        status = scan_progress(ui, state, &before); // Poll callbacks may have changed ownership/work.
        if (status != SCAN_READY) break;
        now = plat_now_ns();
        if (now < start || now - start >= XCDE_SAVE_SCAN_BUDGET_NS) {
            scan_count(state, generation, STOP_BUDGET); break;
        }
        (void)native_call(c, 0x70002823a0ull, state, 0);
        if (!scan_current(state, generation, ui)) break;
        scan_count(state, generation, STOP_EXTRA);
        status = scan_progress(ui, state, &after);
        if (status != SCAN_READY) break;
        if (!memcmp(&before, &after, sizeof before)) {
            scan_count(state, generation, STOP_STALLED); break;
        }
        if (step + 1 == XCDE_SAVE_SCAN_MAX_EXTRA) scan_count(state, generation, STOP_LIMIT);
    }
    scan_finish(state, generation, status);
}

void xcde_expanded_saves_pump(Cpu *c, uint64_t ui, uint64_t state) {
    // LINUX-036: preserve even the original call's volatile results and FP state;
    // only native memory effects of approved extra metadata reads are retained.
    Cpu saved = *c;
    uint64_t fpcr = rt_get_fpcr(), fpsr = rt_get_fpsr();
    scan_pump(c, ui, state);
    *c = saved;
    rt_set_fpcr(fpcr); rt_set_fpsr(fpsr);
}

void xcde_expanded_saves_remember(uint32_t story, uint16_t file_id, bool autosave) {
    if (story > 1 || file_id >= XCDE_SAVE_MANUAL_COUNT || (autosave && file_id))
        FATAL("[expanded-saves] invalid remembered save identity\n");
    pthread_mutex_lock(&save_lock);
    remembered[story] = autosave ? 0 : (int32_t)file_id + 1;
    pthread_mutex_unlock(&save_lock);
}

void xcde_expanded_saves_capture(Cpu *c) {
    uint32_t story;
    uint64_t ui = c->x[0], state = checked_ui(ui, &story);
    int32_t selected = (int32_t)LD32(ui + UI_SELECTED);
    if (selected < 0 || selected >= XCDE_SAVE_RECORD_COUNT)
        FATAL("[expanded-saves] selected save index is out of bounds\n");
    uint64_t record = state + XCDE_SAVE_RECORD_BASE + selected * XCDE_SAVE_RECORD_STRIDE;
    uint16_t file_id = LD16(record + 0x44);
    uint8_t alternate = LD8(record + 0x46);
    bool autosave = alternate != 0;
    if (alternate > 1 || (autosave ? selected != 0 : selected != (int32_t)file_id + 1)
            || (autosave && LD32(ui + UI_MODE) == 0))
        FATAL("[expanded-saves] native selected save identity mismatch\n");
    xcde_expanded_saves_remember(story, file_id, autosave);
}

void xcde_expanded_saves_legacy_set(Cpu *c) {
    uint32_t story;
    uint64_t state = c->x[19];
    uint32_t slot = (uint32_t)c->x[1];
    if (!find_state(state, &story) || !range(state, XCDE_SAVE_STATE_BYTES)
            || c->x[0] != LD64(state) || slot < 1 || slot > XCDE_SAVE_MANUAL_COUNT)
        FATAL("[expanded-saves] invalid native remembered-slot write\n");
    // The vanilla field has only four bits. Keep original IDs exactly, and use
    // its documented zero/default for extended IDs rather than touching flags.
    (void)native_call(c, 0x7000081980ull, c->x[0], slot <= 3 ? slot : 0);
}

void xcde_expanded_saves_default(Cpu *c) {
    uint64_t sequence = c->x[19];
    if (!range(sequence, 0x138)) FATAL("[expanded-saves] invalid save sequence\n");
    uint32_t story;
    uint64_t ui = LD64(sequence + SEQUENCE_UI), state = checked_ui(ui, &story);
    if (c->x[0] != LD64(state) || LD32(ui + UI_MODE) != 0)
        FATAL("[expanded-saves] native remembered-slot getter context mismatch\n");
    uint32_t legacy = (uint32_t)native_call(c, 0x70000819d0ull, c->x[0], 0);
    if (legacy > 3) FATAL("[expanded-saves] invalid legacy remembered-slot value\n");
    pthread_mutex_lock(&save_lock);
    int32_t selected = remembered[story];
    pthread_mutex_unlock(&save_lock);
    c->x[0] = selected >= 0 ? (uint32_t)selected : legacy;
}

RT_ON_RESET(expanded_saves_reset) {
    pthread_mutex_lock(&save_lock);
    while (bindings) {
        SaveBinding *next = bindings->next;
        free(bindings);
        bindings = next;
    }
    remembered[0] = remembered[1] = -1;
    pthread_mutex_unlock(&save_lock);
}
