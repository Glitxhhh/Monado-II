// [XCDE] LINUX-035: authored native ABI fixture, no game code/data or saves.
#define _GNU_SOURCE
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "../../runtime/hle/expanded_saves.c"

// Independently checked against the selected-input ABI probes, not inferred
// from a larger buffer or from a mock accepting its own incorrect layout.
_Static_assert(XCDE_SAVE_MANUAL_COUNT == 99 && XCDE_SAVE_RECORD_COUNT == 100, "logical slots");
_Static_assert(XCDE_SAVE_VISIBLE_COUNT == 4, "physical cards");
_Static_assert(XCDE_SAVE_RECORD_BASE == 0x1541e8 && XCDE_SAVE_RECORD_STRIDE == 0x50, "native records");
_Static_assert(XCDE_SAVE_RECORD_END == 0x156128 && XCDE_SAVE_COUNT_OFFSET == 0x1fcf28, "record/count arenas");
_Static_assert(XCDE_SAVE_THUMB_BASE == 0x587150 && XCDE_SAVE_THUMB_STRIDE == 0x2a300, "relocated thumbnails");
_Static_assert(XCDE_SAVE_THUMB_BYTES == 0x107ac00 && XCDE_SAVE_STATE_BYTES == 0x1601d50, "allocation extent");

enum { FATAL_EXIT = 71, MAP_BYTES = 0x3600000 };
static const uint64_t states[2] = {HEAP_BASE + 0x10000, HEAP_BASE + 0x1800000};
static const uint64_t uis[2] = {HEAP_BASE + 0x3200000, HEAP_BASE + 0x3201000};
static const uint64_t contexts[2] = {HEAP_BASE + 0x3300000, HEAP_BASE + 0x3301000};
static const uint64_t views[2] = {HEAP_BASE + 0x3302000, HEAP_BASE + 0x3303000};
static const uint64_t sequences[2] = {HEAP_BASE + 0x3304000, HEAP_BASE + 0x3305000};
static const uint64_t references[2] = {HEAP_BASE + 0x3306000, HEAP_BASE + 0x3307000};
static const uint64_t stack_top = HEAP_BASE + 0x3500000;
static uint32_t native_words[2];
static unsigned lookups, setters, getters, cases, rejected;
static uint32_t setter_arg, default_story;
static bool fatal_expected, invalid_view;
static Cpu negative_cpu;
static uint32_t negative_story;
static uint16_t negative_id;
static bool negative_auto;
enum { LOOKUP_NORMAL, LOOKUP_REMEMBER, LOOKUP_FORGET, LOOKUP_REENTER };
static unsigned lookup_action;
static unsigned pump_calls, pump_polls, pump_cases, pump_action;
static uint64_t pump_now, pump_cost, pump_poll_cost, pump_poll_callback;
static bool pumping_fixture;
enum { PUMP_PROGRESS, PUMP_STALL, PUMP_REENTER, PUMP_FORGET, PUMP_REUSE };
static uint64_t pump_native(Cpu *c, uint64_t function);
static void pump_cold(unsigned story, unsigned mode);

uint64_t plat_now_ns(void) { return pump_now; }
void rt_unwind_point(void) {}

void rt_fatal_trace(void) {
    if (fatal_expected) _Exit(FATAL_EXIT);
    abort();
}

static unsigned context_story(uint64_t context) {
    if (context == contexts[0]) return 0;
    assert(context == contexts[1]);
    return 1;
}

// These are the narrow call contracts of the bridge. Native list navigation,
// sorting and actual file jobs are independently exercised by private probes.
void guest_call(Cpu *c, uint64_t function) {
    assert(c->sp == stack_top - 0x400 && !(c->sp & 15));
    uint64_t result = 0;
    if (function == 0x70002823a0ull || function == 0x70000863f0ull) {
        result = pump_native(c, function);
    } else if (function == 0x7000592a30ull) {
        uint64_t reference = c->x[0];
        unsigned story = context_story(LD64(reference));
        ++lookups;
        if (lookup_action == LOOKUP_REMEMBER) {
            // The ownership mutex must not span a guest call.
            xcde_expanded_saves_remember(story, 7, false);
        } else if (lookup_action == LOOKUP_FORGET) {
            xcde_expanded_saves_forget(reference);
        } else if (lookup_action == LOOKUP_REENTER) {
            Cpu nested = *c;
            nested.sp = stack_top;
            nested.x[0] = reference + XCDE_SAVE_RECORD_BASE;
            nested.x[20] = reference;
            xcde_expanded_saves_initialize(&nested);
            assert(!"reentrant initialization must fail closed");
        }
        result = invalid_view ? GUEST_HI - 0x100 : views[story];
    } else if (function == 0x7000081980ull) {
        unsigned story = context_story(c->x[0]);
        setter_arg = (uint32_t)c->x[1];
        assert(setter_arg <= 3); // Actual vanilla setter ORs unmasked input into a nibble.
        uint32_t upper = native_words[story] & ~15u;
        native_words[story] = upper | setter_arg;
        assert((native_words[story] & ~15u) == upper);
        ++setters;
    } else if (function == 0x70000819d0ull) {
        unsigned story = context_story(c->x[0]);
        default_story = story;
        result = native_words[story] & 15u;
        ++getters;
    } else assert(!"unexpected native bridge call");
    // Includes FPCR/FPSR, vectors, TLS, exclusives, flags, padding and stack.
    memset(c, 0xc7, sizeof *c);
    c->x[0] = result;
}

static Cpu cpu(void) {
    Cpu c;
    memset(&c, 0x39, sizeof c);
    c.sp = stack_top;
    return c;
}

static uint64_t record(unsigned story, unsigned index) {
    assert(story < 2 && index < 100);
    return states[story] + 0x1541e8 + index * 0x50;
}

static void pristine(unsigned story) {
    uint64_t state = states[story];
    memset((void *)(uintptr_t)(state - 64), 0xe3, 64);
    memset((void *)(uintptr_t)state, 0xa6, XCDE_SAVE_STATE_BYTES);
    memset((void *)(uintptr_t)(state + XCDE_SAVE_STATE_BYTES), 0x4d, 64);
    memset((void *)(uintptr_t)(state + 0x587150), 0x80, 100u * 0x2a300u);
    ST64(state, contexts[story]);
    ST32(state + 0x1fcf28, 100);
    ST32(state + 0x1fcf2c, 0x7a5c39d1);
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t p = record(story, i);
        memset((void *)(uintptr_t)p, 0x60 + i, 0x50);
        ST16(p, 0); // Empty inline UTF16 payload; reserved bytes remain distinct.
        ST64(p + 0x18, 0);
        ST32(p + 0x3c, 0);
        ST32(p + 0x40, 1);
        ST16(p + 0x44, i ? i - 1 : 0);
        ST8(p + 0x46, i == 0);
        ST8(p + 0x47, 0); ST8(p + 0x48, 0); ST8(p + 0x49, 1); ST8(p + 0x4a, 0);
    }
    ST64(references[story], contexts[story]);
    ST8(views[story] + 0x150, story ? 0xa8 : 0xf7); // Only bit3 identifies story.
    memset((void *)(uintptr_t)uis[story], 0x92, 0xb0);
    ST64(uis[story], contexts[story]);
    ST64(uis[story] + 0x90, state);
    ST32(uis[story] + 0x88, 4); ST32(uis[story] + 0x8c, 1);
    ST32(uis[story] + 0x9c, 100); ST32(uis[story] + 0xa8, 0);
    ST64(sequences[story] + 0x130, uis[story]);
}

static void initialize(unsigned story) {
    uint64_t state = states[story];
    unsigned char *before = malloc(XCDE_SAVE_STATE_BYTES);
    assert(before);
    memcpy(before, (const void *)(uintptr_t)state, XCDE_SAVE_STATE_BYTES);
    Cpu c = cpu(); c.x[0] = state + 0x1541e8; c.x[20] = state;
    Cpu expected = c;
    expected.x[1] = state + 0x156128;
    xcde_expanded_saves_initialize(&c);
    assert(!memcmp(&c, &expected, sizeof c));
    assert(!memcmp(before, (const void *)(uintptr_t)state, 0x154328));
    assert(!memcmp(before + 0x156128, (const void *)(uintptr_t)(state + 0x156128),
                   XCDE_SAVE_STATE_BYTES - 0x156128));
    for (unsigned i = 4; i < 100; ++i) {
        unsigned char template[0x50];
        memcpy(template, before + 0x1541e8 + 0x50, sizeof template);
        template[0x44] = (unsigned char)(i - 1); template[0x45] = 0;
        assert(!memcmp(template, (const void *)(uintptr_t)record(story, i), sizeof template));
    }
    for (unsigned i = 0; i < 64; ++i) {
        assert(LD8(state - 64 + i) == 0xe3);
        assert(LD8(state + XCDE_SAVE_STATE_BYTES + i) == 0x4d);
    }
    // All manual thumbnails precede the independent autosave buffer99, and no
    // visible-card or count/flags storage is borrowed for the larger arena.
    for (unsigned i = 0; i < 100; ++i) {
        uint64_t p = state + 0x587150 + i * 0x2a300;
        assert(LD8(p) == 0x80 && LD8(p + 0x2a300 - 1) == 0x80);
    }
    assert(state + 0x587150 + 100u * 0x2a300u == state + XCDE_SAVE_STATE_BYTES);
    free(before);
    ++cases;
}

static uint32_t default_index(unsigned story) {
    // The native nibble getter is called only while constructing a Save menu.
    // A Load/Continue choice can supply provenance for the next Save opening.
    uint32_t prior_mode = LD32(uis[story] + 0x8c);
    ST32(uis[story] + 0x8c, 0);
    Cpu c = cpu(); c.x[0] = contexts[story]; c.x[19] = sequences[story];
    Cpu expected = c;
    xcde_expanded_saves_default(&c);
    uint32_t result = (uint32_t)c.x[0]; expected.x[0] = result;
    assert(!memcmp(&c, &expected, sizeof c) && default_story == story);
    ST32(uis[story] + 0x8c, prior_mode);
    return result;
}

static void select_index(unsigned story, unsigned mode, unsigned index) {
    ST32(uis[story] + 0x8c, mode); ST32(uis[story] + 0xa8, index);
    Cpu c = cpu(); c.x[0] = uis[story]; Cpu before = c;
    unsigned char ui_before[0xb0];
    memcpy(ui_before, (const void *)(uintptr_t)uis[story], sizeof ui_before);
    xcde_expanded_saves_capture(&c);
    assert(!memcmp(&c, &before, sizeof c));
    assert(!memcmp(ui_before, (const void *)(uintptr_t)uis[story], sizeof ui_before));
    assert(default_index(story) == index);
    ++cases;
}

static void initialize_negative(void) { xcde_expanded_saves_initialize(&negative_cpu); }
static void capture_negative(void) { xcde_expanded_saves_capture(&negative_cpu); }
static void default_negative(void) { xcde_expanded_saves_default(&negative_cpu); }
static void setter_negative(void) { xcde_expanded_saves_legacy_set(&negative_cpu); }
static void remember_negative(void) { xcde_expanded_saves_remember(negative_story, negative_id, negative_auto); }
static void story_negative(void) { (void)xcde_expanded_saves_story(&negative_cpu, references[0]); }

static void expect_fatal(void (*operation)(void)) {
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        fatal_expected = true;
        alarm(3); // Detect a mutex accidentally held during native re-entry.
        operation();
        _Exit(0);
    }
    int status = 0; assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == FATAL_EXIT);
    ++rejected;
}

static void expect_init_atomic(void) {
    unsigned char *before = malloc(XCDE_SAVE_STATE_BYTES); assert(before);
    memcpy(before, (const void *)(uintptr_t)states[0], XCDE_SAVE_STATE_BYTES);
    expect_fatal(initialize_negative);
    assert(!memcmp(before, (const void *)(uintptr_t)states[0], XCDE_SAVE_STATE_BYTES));
    free(before);
}

static void test_bad_initialization(void) {
    expanded_saves_reset(); pristine(0);
    negative_cpu = cpu(); negative_cpu.x[20] = states[0];
    negative_cpu.x[0] = states[0] + 0x1541e8;
    const unsigned offsets[] = {0x18, 0x3c, 0x40, 0x44, 0x46, 0x47, 0x48, 0x49, 0x4a};
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned j = 0; j < sizeof offsets / sizeof offsets[0]; ++j) {
            uint64_t p = record(0, i) + offsets[j]; uint8_t old = LD8(p);
            ST8(p, old ^ 0x40); expect_init_atomic(); ST8(p, old);
        }
    ST32(states[0] + 0x1fcf28, 4); expect_init_atomic(); ST32(states[0] + 0x1fcf28, 100);
    ++negative_cpu.x[0]; expect_init_atomic(); --negative_cpu.x[0];
    ++negative_cpu.x[20]; expect_init_atomic(); --negative_cpu.x[20];
    invalid_view = true; expect_init_atomic(); invalid_view = false;
    lookup_action = LOOKUP_FORGET; expect_init_atomic(); lookup_action = LOOKUP_NORMAL;
    lookup_action = LOOKUP_REENTER; expect_init_atomic(); lookup_action = LOOKUP_NORMAL;
    expanded_saves_reset();
}

static void test_selection_and_nibble(void) {
    expanded_saves_reset(); pristine(0); pristine(1);
    native_words[0] = 0xabcde5a2; native_words[1] = 0x7432b6d3;
    initialize(0); initialize(1);
    assert(default_index(0) == 2 && default_index(1) == 3);
    for (unsigned story = 0; story < 2; ++story) {
        Cpu c = cpu(), before = c;
        assert(xcde_expanded_saves_story(&c, references[story]) == story);
        assert(!memcmp(&c, &before, sizeof c));
        for (unsigned mode = 0; mode < 2; ++mode) {
            for (unsigned index = mode ? 0 : 1; index < 100; ++index) {
                select_index(story, mode, index);
                if (!mode) {
                    c = cpu(); c.x[0] = contexts[story]; c.x[1] = index; c.x[19] = states[story]; before = c;
                    uint32_t upper = native_words[story] & ~15u;
                    xcde_expanded_saves_legacy_set(&c);
                    assert(!memcmp(&c, &before, sizeof c));
                    assert(setter_arg == (index <= 3 ? index : 0));
                    assert((native_words[story] & ~15u) == upper);
                }
            }
        }
        // Traverse every legal four-card window without creating new records
        // or changing total/visible counts. Native scrolling is a separate probe.
        for (unsigned top = 0; top <= 96; ++top)
            for (unsigned row = 0; row < 4; ++row) select_index(story, 1, top + row);
    }
    // Scanner/Continue provenance is explicit per story, independent of whichever
    // menu was last selected or of the vanilla four-bit remembered fields.
    xcde_expanded_saves_remember(0, 98, false); xcde_expanded_saves_remember(1, 37, false);
    assert(default_index(0) == 99 && default_index(1) == 38);
    xcde_expanded_saves_remember(0, 0, true);
    assert(default_index(0) == 0 && default_index(1) == 38);
    // Missing/occupied/clear metadata must not be overwritten by selection.
    for (unsigned i = 0; i < 100; ++i) ST32(record(0, i) + 0x3c, i % 3 ? 1 : 0);
    unsigned char before_records[100 * 0x50];
    memcpy(before_records, (const void *)(uintptr_t)record(0, 0), sizeof before_records);
    for (unsigned i = 0; i < 100; ++i) select_index(0, 1, i);
    assert(!memcmp(before_records, (const void *)(uintptr_t)record(0, 0), sizeof before_records));
}

static void test_ownership_failures(void) {
    // State ownership remains live after the exhaustive valid-selection checks.
    negative_cpu = cpu(); negative_cpu.x[0] = uis[0];
    struct { unsigned offset; uint64_t value; bool wide; } bad_ui[] = {
        {0, contexts[1], true}, {0x90, states[1], true},
        {0x88, 5, false}, {0x8c, 2, false}, {0x9c, 4, false},
        {0xa8, 100, false}, {0xa8, UINT32_MAX, false},
    };
    for (unsigned i = 0; i < sizeof bad_ui / sizeof bad_ui[0]; ++i) {
        uint64_t p = uis[0] + bad_ui[i].offset;
        uint64_t old = bad_ui[i].wide ? LD64(p) : LD32(p);
        if (bad_ui[i].wide) ST64(p, bad_ui[i].value); else ST32(p, bad_ui[i].value);
        expect_fatal(capture_negative);
        if (bad_ui[i].wide) ST64(p, old); else ST32(p, old);
    }
    ST32(uis[0] + 0x8c, 0); ST32(uis[0] + 0xa8, 0); expect_fatal(capture_negative);
    ST32(uis[0] + 0x8c, 1); ST32(uis[0] + 0xa8, 99);
    uint16_t id = LD16(record(0, 99) + 0x44);
    ST16(record(0, 99) + 0x44, 97); expect_fatal(capture_negative); ST16(record(0, 99) + 0x44, id);
    ST8(record(0, 99) + 0x46, 1); expect_fatal(capture_negative); ST8(record(0, 99) + 0x46, 0);
    ST32(uis[0] + 0xa8, 0); ST8(record(0, 0) + 0x46, 2);
    expect_fatal(capture_negative); ST8(record(0, 0) + 0x46, 1); ST32(uis[0] + 0xa8, 99);
    ST32(states[0] + 0x1fcf28, 4); expect_fatal(capture_negative); ST32(states[0] + 0x1fcf28, 100);
    negative_cpu.x[0] = 0; expect_fatal(capture_negative);
    negative_cpu.x[0] = uis[0] + 1; expect_fatal(capture_negative);
    negative_story = 2; negative_id = 0; negative_auto = false; expect_fatal(remember_negative);
    negative_story = 0; negative_id = 99; expect_fatal(remember_negative);
    negative_id = 1; negative_auto = true; expect_fatal(remember_negative);
    negative_cpu = cpu(); negative_cpu.x[0] = contexts[0]; negative_cpu.x[19] = states[0];
    negative_cpu.x[1] = 0; expect_fatal(setter_negative);
    negative_cpu.x[1] = 100; expect_fatal(setter_negative);
    negative_cpu.x[1] = 99; negative_cpu.x[0] = contexts[1]; expect_fatal(setter_negative);
    negative_cpu.x[0] = contexts[0]; negative_cpu.x[19] = states[0] + 16; expect_fatal(setter_negative);
    negative_cpu = cpu(); negative_cpu.x[0] = contexts[0]; negative_cpu.x[19] = sequences[0];
    ST32(uis[0] + 0x8c, 1); expect_fatal(default_negative); ST32(uis[0] + 0x8c, 0);
    negative_cpu.x[0] = contexts[1]; expect_fatal(default_negative); negative_cpu.x[0] = contexts[0];
    uint32_t old_word = native_words[0]; native_words[0] |= 15; expect_fatal(default_negative); native_words[0] = old_word;
    ST64(sequences[0] + 0x130, 0); expect_fatal(default_negative); ST64(sequences[0] + 0x130, uis[0]);
    ST64(references[0], 0); expect_fatal(story_negative); ST64(references[0], contexts[0]);
}

static void test_lifecycle(void) {
    expanded_saves_reset(); pristine(0); initialize(0);
    // A duplicate must reject BEFORE touching hidden loaded records. Shared
    // guest pages make child writes observable even though FATAL exits the child.
    ST32(record(0, 19) + 0x3c, 0xdeadbeef);
    negative_cpu = cpu(); negative_cpu.x[0] = states[0] + 0x1541e8; negative_cpu.x[20] = states[0];
    expect_init_atomic();
    xcde_expanded_saves_remember(0, 98, false);
    xcde_expanded_saves_forget(states[0]);
    negative_cpu = cpu(); negative_cpu.x[0] = uis[0]; expect_fatal(capture_negative);
    pristine(0); initialize(0);
    assert(default_index(0) == 99); // Menu destruction does not discard last choice.
    expanded_saves_reset(); pristine(0); native_words[0] = 0x76543211; initialize(0);
    assert(default_index(0) == 1); // A runtime reset does discard every host shadow.
    xcde_expanded_saves_forget(0); // Unrelated ownership cannot retire the live state.
    select_index(0, 1, 99);
    expanded_saves_reset(); pristine(0); lookup_action = LOOKUP_REMEMBER;
    initialize(0); lookup_action = LOOKUP_NORMAL;
    assert(default_index(0) == 8);
    expanded_saves_reset();
}

// This fixture models the native helper's narrow ABI and asynchronous progress,
// not the game's implementation. The independent selected-input probe covers
// the actual native state machine. All addresses below are authored guest RAM.
static uint64_t pump_native(Cpu *c, uint64_t function) {
    assert(pumping_fixture);
    unsigned story = c->x[0] >= states[1] && c->x[0] < states[1] + XCDE_SAVE_STATE_BYTES;
    uint64_t state = states[story];
    if (function == 0x70000863f0ull) {
        assert(c->x[0] == state + 0x10 || c->x[0] == state + 0x1540a0);
        ++pump_polls;
        pump_now += pump_poll_cost;
        if (c->x[0] == pump_poll_callback) {
            ST64(c->x[0] + 0x118, HEAP_BASE + 0x4321);
            pump_poll_callback = 0;
        }
        return LD32(c->x[0] + 0x13c) & 1u;
    }
    assert(c->x[0] == state);
    // Both direction and the callback handle matter: the native worker clears
    // busy at entry, while retaining its handle until its callback has finished.
    assert(LD32(state + 0x20) != 1 && LD32(state + 0x1540b0) != 1);
    assert(!(LD32(state + 0x14c) & 1) && !LD64(state + 0x128));
    assert(!(LD32(state + 0x1541dc) & 1) && !LD64(state + 0x1541b8));
    ++pump_calls;
    pump_now += pump_cost;
    if (pump_action == PUMP_STALL) return 0;
    if (pump_action == PUMP_FORGET) {
        xcde_expanded_saves_forget(state);
        return 0;
    }
    if (pump_action == PUMP_REUSE) {
        unsigned mode = LD32(uis[story] + 0x8c);
        pump_action = PUMP_PROGRESS;
        xcde_expanded_saves_forget(state);
        pristine(story); initialize(story); pump_cold(story, mode);
        return 0;
    }
    if (pump_action == PUMP_REENTER) {
        pump_action = PUMP_PROGRESS;
        unsigned before_calls = pump_calls, before_polls = pump_polls;
        Cpu before = *c;
        xcde_expanded_saves_pump(c, uis[story], state);
        assert(pump_calls == before_calls && pump_polls == before_polls);
        assert(!memcmp(c, &before, sizeof *c));
    }
    unsigned index = 0;
    for (; index < 100 && LD32(record(story, index) + 0x40) == 4; ++index) {}
    assert(index < 100 && LD32(record(story, index) + 0x40) == 1);
    for (unsigned i = index; i < 100; ++i) assert(LD32(record(story, i) + 0x40) == 1);
    uint64_t current = record(story, index);
    // Missing/error and populated metadata all complete via native request4;
    // the adapter must not reinterpret the result as another file operation.
    ST32(current + 0x3c, index % 3 ? 1 : 0);
    ST32(current + 0x40, 4);
    if (++index == 100) {
        ST16(state + 0x1541e0, 0x7fff);
        ST8(state + 0x1541e2, 0);
        ST32(state + 0x1541e4, 0);
        ST32(state + 0x1fcf2c, LD32(state + 0x1fcf2c) & ~1u);
    } else {
        ST16(state + 0x1541e0, index - 1);
        ST8(state + 0x1541e2, 0);
        ST32(state + 0x1541e4, 1);
    }
    return 0;
}

static void pump_cold(unsigned story, unsigned mode) {
    uint64_t state = states[story], ui = uis[story];
    // Real cold Save/Load initialization has an UNBOUND zero-size list model.
    // It becomes total100/visible4 only after metadata readiness is consumed.
    ST32(ui + 0x98, 1); ST32(ui + 0x9c, 0); ST32(ui + 0xa0, 0);
    ST32(ui + 0x8c, mode);
    ST32(state + 0x1fcf2c, mode ? 3 : 5);
    ST16(state + 0x1541e0, 0); ST8(state + 0x1541e2, 1);
    ST32(state + 0x1541e4, 1);
    const unsigned tasks[] = {0x10, 0x1540a0};
    for (unsigned i = 0; i < 2; ++i) {
        ST32(state + tasks[i] + 0x10, 0);
        ST32(state + tasks[i] + 0x13c, 0);
        ST64(state + tasks[i] + 0x118, 0);
    }
}

static void pump_prepare(unsigned story, unsigned mode) {
    expanded_saves_reset();
    pump_calls = pump_polls = 0; pump_now = pump_cost = pump_poll_cost = pump_poll_callback = 0;
    pump_action = PUMP_PROGRESS; pumping_fixture = true;
    pristine(story); initialize(story); pump_cold(story, mode);
}

static void pump_run(unsigned story) {
    // The checked patch performs the exact original native BL first. These
    // bytes represent its post-call CPU, which the extra pump must preserve.
    Cpu c = cpu(); c.x[30] = 0x7000283204ull; c.x[0] = 0x12345678;
    Cpu before = c;
    unsigned calls_before = pump_calls;
    xcde_expanded_saves_pump(&c, uis[story], states[story]);
    assert(!memcmp(&c, &before, sizeof c));
    assert(pump_calls - calls_before <= 7); // Original plus extras: at most8.
    ++pump_cases;
}

static void test_read_pump(void) {
    for (unsigned story = 0; story < 2; ++story)
        for (unsigned mode = 0; mode < 2; ++mode) {
            pump_prepare(story, mode);
            pump_run(story);
            assert(pump_calls == 7 && pump_polls >= 7);
            for (unsigned i = 0; i < 7; ++i) assert(LD32(record(story, i) + 0x40) == 4);
            for (unsigned i = 7; i < 100; ++i) assert(LD32(record(story, i) + 0x40) == 1);
            unsigned updates = 1;
            while (pump_calls < 100) { pump_run(story); assert(++updates <= 15); }
            assert(pump_calls == 100 && updates == 15);
            for (unsigned i = 0; i < 100; ++i) {
                assert(LD32(record(story, i) + 0x40) == 4);
                assert(LD32(record(story, i) + 0x3c) == (i % 3 ? 1u : 0u));
            }
            unsigned done = pump_calls;
            pump_run(story); assert(pump_calls == done);
            // An already shown menu cannot regain initial-scan eligibility.
            ST32(states[story] + 0x1fcf2c, mode ? 3 : 5);
            ST32(record(story, 99) + 0x40, 1);
            pump_run(story); assert(pump_calls == done);
        }

    pump_prepare(0, 1); pump_action = PUMP_STALL;
    pump_run(0); assert(pump_calls == 1); // A no-progress update must never spin.
    pump_action = PUMP_PROGRESS; pump_run(0); assert(pump_calls == 8);

    pump_prepare(0, 1);
    ST32(states[0] + 0x1541e4, 2); ST32(states[0] + 0x20, 2);
    ST32(states[0] + 0x14c, 0x40000006);
    ST32(states[0] + 0x1541dc, 0x40000004);
    pump_run(0); assert(pump_calls == 7); // Success/constructor flags are not busy.

    pump_prepare(1, 0);
    ST32(states[1] + 0x1fcf2c, 5 | 0x48);
    pump_run(1); assert(pump_calls == 7); // Native permission/NG+ bits are compatible.

    pump_prepare(0, 1);
    ST16(states[0] + 0x1541e0, 0x7fff); ST8(states[0] + 0x1541e2, 0);
    ST32(states[0] + 0x1541e4, 0);
    pump_run(0); assert(pump_calls == 7); // Native idle metadata scheduling is safe.

    pump_prepare(0, 0); pump_cost = 600000;
    pump_run(0); assert(pump_calls == 1 && pump_now == 600000);
    // The soft budget is checked between calls; one slow native call cannot be
    // preempted. A subsequent update receives a fresh bounded budget.
    pump_run(0); assert(pump_calls == 2 && pump_now == 1200000);

    pump_prepare(0, 1); pump_poll_cost = 300000;
    pump_run(0); assert(pump_calls == 0 && pump_now >= 500000);
    pump_poll_cost = 0; pump_run(0); assert(pump_calls == 7);

    for (unsigned which = 0; which < 2; ++which) {
        unsigned task = which ? 0x1540a0 : 0x10;
        pump_prepare(0, 1);
        ST32(states[0] + task + 0x13c, 1);
        pump_run(0); assert(pump_calls == 0);
        ST32(states[0] + task + 0x13c, 0);
        pump_run(0); assert(pump_calls == 7);
        pump_prepare(0, 1);
        ST64(states[0] + task + 0x118, HEAP_BASE + 0x1234);
        pump_run(0); assert(pump_calls == 0); // Busy0, callback handle still live.
        ST64(states[0] + task + 0x118, 0);
        pump_run(0); assert(pump_calls == 7);
        pump_prepare(0, 1);
        pump_poll_callback = states[0] + task;
        pump_run(0); assert(pump_calls == 0); // Callback becomes visible during poll.
        ST64(states[0] + task + 0x118, 0);
        pump_run(0); assert(pump_calls == 7);
    }

    // Unsafe native work retires this owner's initial-only permission. Moving
    // the state back to a read-looking tuple must not accelerate later writes.
    const unsigned unsafe_requests[] = {0, 2, 3, 5, UINT32_MAX};
    for (unsigned i = 0; i < sizeof unsafe_requests / sizeof unsafe_requests[0]; ++i) {
        pump_prepare(0, 1);
        ST32(record(0, 57) + 0x40, unsafe_requests[i]);
        pump_run(0); assert(pump_calls == 0);
        ST32(record(0, 57) + 0x40, 1);
        pump_run(0); assert(pump_calls == 0);
    }
    // A populated autosave may temporarily enter the native thumbnail reader.
    // Pause extra work there, then resume remaining metadata after the normal
    // update completes that read. This is not a new initial-read campaign.
    for (unsigned phase = 3; phase <= 4; ++phase) {
        pump_prepare(0, 1);
        ST32(record(0, 0) + 0x40, 4); ST32(record(0, 0) + 0x3c, 1);
        ST32(states[0] + 0x1541e4, phase);
        ST32(states[0] + 0x1540b0, 2);
        pump_run(0); assert(pump_calls == 0);
        ST16(states[0] + 0x1541e0, 0); ST8(states[0] + 0x1541e2, 0);
        ST32(states[0] + 0x1541e4, 1);
        pump_run(0); assert(pump_calls == 7);
        assert(LD32(record(0, 0) + 0x3c) == 1);
        assert(LD32(record(0, 7) + 0x40) == 4);
    }
    const unsigned unsafe_phases[] = {5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, UINT32_MAX};
    for (unsigned i = 0; i < sizeof unsafe_phases / sizeof unsafe_phases[0]; ++i) {
        pump_prepare(0, 1);
        ST32(states[0] + 0x1541e4, unsafe_phases[i]);
        pump_run(0); assert(pump_calls == 0);
        ST32(states[0] + 0x1541e4, 1);
        pump_run(0); assert(pump_calls == 0);
    }
    const uint32_t bad_pending[] = {0x20000, 99, 0x10001, 0x1007fff};
    for (unsigned i = 0; i < sizeof bad_pending / sizeof bad_pending[0]; ++i) {
        pump_prepare(0, 1); ST32(states[0] + 0x1541e0, bad_pending[i]);
        pump_run(0); assert(pump_calls == 0);
        ST32(states[0] + 0x1541e0, 0x10000);
        pump_run(0); assert(pump_calls == 0);
    }
    const unsigned unsafe_directions[] = {1, 3, UINT32_MAX};
    for (unsigned which = 0; which < 2; ++which)
        for (unsigned i = 0; i < sizeof unsafe_directions / sizeof unsafe_directions[0]; ++i) {
            pump_prepare(0, 1);
            unsigned direction = which ? 0x1540b0 : 0x20;
            ST32(states[0] + direction, unsafe_directions[i]);
            pump_run(0); assert(pump_calls == 0);
            ST32(states[0] + direction, 0);
            pump_run(0); assert(pump_calls == 0);
        }
    const unsigned unsafe_flags[] = {0x10, 0x20, 0x80, 0x100, 0x80000000};
    for (unsigned i = 0; i < sizeof unsafe_flags / sizeof unsafe_flags[0]; ++i) {
        pump_prepare(0, 1);
        ST32(states[0] + 0x1fcf2c, 3 | unsafe_flags[i]);
        pump_run(0); assert(pump_calls == 0);
        ST32(states[0] + 0x1fcf2c, 3);
        pump_run(0); assert(pump_calls == 0);
    }
    pump_prepare(0, 1); ST32(uis[0] + 0x98, 0);
    pump_run(0); assert(pump_calls == 0);
    ST32(uis[0] + 0x98, 1); pump_run(0); assert(pump_calls == 0);
    pump_prepare(0, 1); ST32(uis[0] + 0x9c, 100); ST32(uis[0] + 0xa0, 4);
    pump_run(0); assert(pump_calls == 0);
    ST32(uis[0] + 0x9c, 0); ST32(uis[0] + 0xa0, 0);
    pump_run(0); assert(pump_calls == 0);

    pump_prepare(0, 1);
    Cpu foreign = cpu(), foreign_before = foreign;
    xcde_expanded_saves_pump(&foreign, 0, states[0]);
    xcde_expanded_saves_pump(&foreign, uis[0], states[1]);
    assert(!pump_calls && !memcmp(&foreign, &foreign_before, sizeof foreign));
    pump_run(0); assert(pump_calls == 7); // Unrelated ownership does not retire it.

    pump_prepare(0, 1); pump_action = PUMP_REENTER;
    pump_run(0); assert(pump_calls == 7);
    pump_prepare(0, 1); pump_action = PUMP_FORGET;
    pump_run(0); assert(pump_calls == 1);
    pump_action = PUMP_PROGRESS; pump_run(0); assert(pump_calls == 1);
    pump_prepare(0, 1); pump_action = PUMP_REUSE;
    pump_run(0); assert(pump_calls == 1); // New owner generation, same address.
    pump_run(0); assert(pump_calls == 8);
    // A new constructor at the same guest address obtains a new generation.
    pump_prepare(0, 1); pump_run(0); assert(pump_calls == 7);
    expanded_saves_reset(); pump_run(0); assert(pump_calls == 7);
    pumping_fixture = false;
    printf("expanded saves: bounded initial-read pump passed (%u updates)\n", pump_cases);
}

int main(void) {
    const struct rlimit no_core = {0, 0};
    assert(setrlimit(RLIMIT_CORE, &no_core) == 0);
    assert(mmap((void *)(uintptr_t)HEAP_BASE, MAP_BYTES, PROT_READ | PROT_WRITE,
                MAP_SHARED | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0)
           == (void *)(uintptr_t)HEAP_BASE);
    test_bad_initialization();
    test_selection_and_nibble();
    test_ownership_failures();
    test_lifecycle();
    test_read_pump();
    assert(lookups > 0 && setters == 198 && getters > 1000 && cases > 1000 && rejected > 60);
    assert(munmap((void *)(uintptr_t)HEAP_BASE, MAP_BYTES) == 0);
    printf("expanded saves: authored native bridge invariants passed (%u cases, %u fail-closed controls)\n", cases, rejected);
    return 0;
}
