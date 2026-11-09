// [XCDE] LINUX-033: authored native ABI fixtures; no game files or graphics.
#define _GNU_SOURCE
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../../runtime/hle/system_exit.c"

enum { MOCK_ROWS_BYTES = 6 * 0x90, MAX_UI = 8 };
typedef struct {
    uint64_t ui, root, node, vtable, row_root, label, help, sequence;
    unsigned char rows[MOCK_ROWS_BYTES];
    unsigned constructors, widget_constructors, widget_destructors, updates, labels, helps;
    unsigned badges, visible, enabled;
} MockUi;

static MockUi mock_ui[MAX_UI];
static unsigned mock_count, calls, closes, cancel_callbacks, variants, variant_destructors;
static unsigned widget_creations, label_calls;
static uint64_t bound_caption, nested_ui;
static size_t allocated;
static bool fatal_expected, nested_constructor;
static unsigned missing_kind, required_cancel;
static jmp_buf fatal_jump;
static const uint64_t sequence = HEAP_BASE + 0x1000;
static const uint64_t child = HEAP_BASE + 0x30000, dialog = HEAP_BASE + 0x31000;
static const uint64_t callback = HEAP_BASE + 0x32000, callback_clone = HEAP_BASE + 0x32100;
static const uint64_t stack_top = HEAP_BASE + 0x70000;
static const uint64_t visible_function = 0x7000100000ull;

void rt_fatal_trace(void) { assert(fatal_expected); longjmp(fatal_jump, 1); }

#define EXPECT_FATAL(expression) do { \
    fatal_expected = true; \
    if (!setjmp(fatal_jump)) { expression; assert(!"expected closed ABI guard"); } \
    fatal_expected = false; \
} while (0)

void *hle_alloc(uint64_t size) {
    assert(size && allocated + size < 0x40000);
    void *p = (void *)(uintptr_t)(HLE_HEAP + allocated);
    allocated += (size + 15) & ~(size_t)15;
    memset(p, 0, size);
    return p;
}

uint64_t plat_now_ns(void) { return 123456789; }
void am_request_host_exit(uint64_t now) {
    assert(now == plat_now_ns() && !bound_caption);
    assert(cancel_callbacks == required_cancel);
    assert(LD32(child + 0x30) == 1); // Authored native Cancel bookkeeping marker.
    ++closes;
}
void xcde_exit_caption_begin(uint64_t context) {
    assert(context == dialog + 0x100); bound_caption = context;
}
void xcde_exit_caption_end(uint64_t context) {
    assert(context == dialog + 0x100);
    if (bound_caption == context) bound_caption = 0;
}

static Cpu cpu(void) {
    Cpu c; memset(&c, 0x37, sizeof c); c.sp = stack_top; return c;
}

static MockUi *by_ui(uint64_t ui) {
    for (unsigned i = 0; i < mock_count; ++i) if (mock_ui[i].ui == ui) return &mock_ui[i];
    assert(!"unknown external UI"); return NULL;
}

static MockUi *by_field(uint64_t value) {
    for (unsigned i = 0; i < mock_count; ++i) {
        MockUi *u = &mock_ui[i];
        if (value == u->root || value == u->node || value == u->row_root
                || value == u->label || value == u->help || value == u->ui + ROW) return u;
    }
    assert(!"unknown native object"); return NULL;
}

static Cpu constructor_cpu(uint64_t seq, bool enabled) {
    memset((void *)(uintptr_t)seq, 0x59, 0x500);
    ST64(seq + UI_OWNER_POINTER, 0);
    ST32(seq + 0x50, enabled ? 1u << 5 : 0);
    Cpu c = cpu(); c.x[19] = seq; c.x[0] = c.x[21] = seq + 0x70;
    c.x[1] = HEAP_BASE + 0x40000; c.x[2] = HEAP_BASE + 0x41000;
    return c;
}

// Each constructor below creates authored field patterns, not native widget
// implementations. Every nested call clobbers the full CPU, including FP/TLS.
void guest_call(Cpu *c, uint64_t function) {
    uint64_t x0 = c->x[0], x1 = c->x[1], x2 = c->x[2];
    uint64_t x3 = c->x[3], x4 = c->x[4], result = 0;
    assert(c->sp == stack_top - 0x400 && !(c->sp & 15));
    ++calls;
    if (function == 0x7000293c00ull) {
        assert(x0 >= HLE_HEAP && x0 + UI_BYTES <= HLE_HEAP + allocated);
        assert(x1 == HEAP_BASE + 0x40000 && x2 == HEAP_BASE + 0x41000 && !x3 && !x4);
        MockUi *u = NULL;
        for (unsigned i = 0; i < mock_count; ++i) if (mock_ui[i].ui == x0) u = &mock_ui[i];
        if (!u) {
            assert(mock_count < MAX_UI);
            u = &mock_ui[mock_count]; u->ui = x0;
            u->root = HEAP_BASE + 0x10000 + mock_count * 0x1000;
            u->node = u->root + 0x100; u->vtable = u->root + 0x200;
            u->row_root = u->root + 0x300; u->label = u->root + 0x400; u->help = u->root + 0x500;
            ++mock_count;
        }
        ++u->constructors;
        u->enabled = u->badges = u->visible = 99;
        memset((void *)(uintptr_t)x0, 0x21, ROW);
        ST64(x0 + 8, u->root); ST32(x0 + 0x14, 9); ST32(x0 + 0x18, 12);
        ST32(x0 + 0x1c, 6); ST32(x0 + 0x20, 6); ST32(x0 + 0x28, 5);
        for (unsigned i = 0; i < 6; ++i) {
            uint64_t row = x0 + 0x30 + i * 0x90;
            memset((void *)(uintptr_t)row, (int)(0x41 + i + u->constructors), 0x90);
            ST64(row + 0x80, row); ST32(row + 0x88, 100 + i);
        }
        memcpy(u->rows, (const void *)(uintptr_t)(x0 + 0x30), sizeof u->rows);
        ST64(u->node, missing_kind == 4 ? 0 : u->vtable);
        ST64(u->vtable + 0x20, visible_function);
        if (nested_constructor) {
            nested_constructor = false;
            Cpu inner = constructor_cpu(sequence + 0x4000, true), before = inner;
            xcde_system_exit_construct(&inner);
            nested_ui = inner.x[21]; before.x[21] = nested_ui;
            assert(!memcmp(&inner, &before, sizeof inner) && nested_ui != x0);
            ST64(sequence + 0x4000 + UI_OWNER_POINTER, nested_ui);
            by_ui(nested_ui)->sequence = sequence + 0x4000;
        }
    } else if (function == 0x700057a8a0ull) {
        MockUi *u = by_field(x0);
        assert(x0 == u->root && x1 == stack_top - 0x80 && x2 == 9);
        assert(!strcmp((const char *)(uintptr_t)x3, "rect_bar_07_mc"));
        ST32(x1, 7); ST32(x1 + 4, missing_kind == 3 ? UINT32_MAX : 707);
        result = missing_kind == 1 ? 0 : 1;
    } else if (function == 0x700057a800ull) {
        MockUi *u = by_field(x0); assert(x1 == stack_top - 0x80);
        result = missing_kind == 2 ? 0 : u->node;
    } else if (function == 0x7000579860ull) {
        MockUi *u = by_field(x0); assert(x0 == u->node);
        result = missing_kind == 5 ? 0 : u->row_root;
    } else if (function == 0x700057a770ull) {
        MockUi *u = by_field(x0); assert(x0 == u->row_root);
        assert(!strcmp((const char *)(uintptr_t)x1, "common_window_menu_mc/rect_text_mc"));
        result = missing_kind == 6 ? 0 : u->label;
    } else if (function == 0x7000261640ull) {
        MockUi *u = by_field(x0);
        assert(x0 == u->ui + ROW && x1 == stack_top - 0x70 && LD64(x1) == u->node);
        assert(x2 == HEAP_BASE + 0x40000 && x3 == 153 && !x4);
        memset((void *)(uintptr_t)x0, 0x76, 0x80); ++u->widget_constructors; ++widget_creations;
    } else if (function == visible_function) {
        MockUi *u = by_field(x0); assert(x0 == u->node && x1 == 1); u->visible = 1;
    } else if (function == 0x7000261c10ull) {
        MockUi *u = by_field(x0); assert(!x1); u->badges = 0;
    } else if (function == 0x7000261c70ull || function == 0x70002619b0ull) {
        (void)by_field(x0); assert(!x1);
    } else if (function == 0x7000261ac0ull) {
        MockUi *u = by_field(x0); assert(x1 <= 1); u->enabled = (unsigned)x1;
    } else if (function == 0x70002619a0ull) {
        MockUi *u = by_field(x0); assert(x1 == 0x1234); ++u->updates;
    } else if (function == 0x700057a830ull) {
        MockUi *u = by_field(x0); assert(x0 == u->root && x1 == 12);
        assert(!strcmp((const char *)(uintptr_t)x2, "rect_tect_Main00_mc")); result = u->help;
    } else if (function == 0x700057c210ull) {
        MockUi *u = by_field(x0);
        if (x0 == u->label) { assert(!strcmp((const char *)(uintptr_t)x1, "Exit to desktop")); ++u->labels; ++label_calls; }
        else { assert(x0 == u->help); assert(!strcmp((const char *)(uintptr_t)x1, "Exit to desktop. Unsaved progress will be lost.")); ++u->helps; }
    } else if (function == 0x7000261830ull) {
        MockUi *u = by_field(x0); assert(x0 == u->ui + ROW); ++u->widget_destructors;
    } else if (function == 0x7000591d90ull || function == 0x7000591c10ull) {
        assert(x0 == stack_top + 0x18 || x0 == stack_top + 0x38);
        if (function == 0x7000591d90ull) { memset((void *)(uintptr_t)x0, 0, 32); ++variant_destructors; }
        else {
            assert(!strcmp((const char *)(uintptr_t)x1, x0 == stack_top + 0x18
                ? "Exit game?" : "Exit to desktop? Unsaved progress will be lost."));
            memset((void *)(uintptr_t)x0, 0x5a, 32); ++variants;
        }
    } else if (function == 0x70003b6350ull) {
        assert(LD64(x0) == 0x7000b55c08ull && LD64(x0 + 8) == child && x1 == 1);
        assert(!bound_caption && !xcde_system_exit_reply(c));
        ST32(child + 0x30, 1); ++cancel_callbacks;
    } else assert(!"unexpected native System contract");
    memset(c, 0xa5, sizeof *c); c->x[0] = result;
}

static void original_rows_preserved(MockUi *u) {
    assert(!memcmp(u->rows, (const void *)(uintptr_t)(u->ui + 0x30), sizeof u->rows));
}

static MockUi *construct(uint64_t seq, bool enabled) {
    Cpu c = constructor_cpu(seq, enabled), before = c;
    unsigned char original[0x500]; memcpy(original, (const void *)(uintptr_t)seq, sizeof original);
    xcde_system_exit_construct(&c);
    uint64_t ui = c.x[21]; before.x[21] = ui;
    assert(!memcmp(&c, &before, sizeof c));
    assert(ui >= HLE_HEAP && ui + UI_BYTES <= HLE_HEAP + allocated && ui != seq + 0x70);
    assert(!memcmp(original, (const void *)(uintptr_t)seq, sizeof original));
    ST64(seq + UI_OWNER_POINTER, ui); // The following unchanged native parent instruction.
    MockUi *u = by_ui(ui); u->sequence = seq; original_rows_preserved(u);
    return u;
}

static void destroy(MockUi *u) {
    Cpu c = cpu(); c.x[0] = u->ui; Cpu before = c;
    unsigned count = u->widget_destructors;
    xcde_system_exit_destroy(&c);
    assert(!memcmp(&c, &before, sizeof c) && !LD64(u->ui + ROW_POINTER));
    original_rows_preserved(u);
    unsigned after = u->widget_destructors;
    assert(after == count || after == count + 1);
    xcde_system_exit_destroy(&c); assert(u->widget_destructors == after);
    unsigned prior_calls = calls;
    xcde_system_exit_update(&c); assert(calls == prior_calls);
}

static Cpu arm_dialog(MockUi *u) {
    ST32(u->ui + 0x28, 6);
    assert(xcde_system_exit_action(u->sequence, 6) == 5);
    ST64(child + 8, u->sequence);
    Cpu c = cpu(); c.x[19] = u->sequence; c.x[22] = child; Cpu before = c;
    xcde_system_exit_child(&c); assert(!memcmp(&c, &before, sizeof c));
    c = cpu(); c.x[19] = child; c.x[1] = child; c.x[0] = c.x[22] = dialog;
    c.x[2] = stack_top + 0x18;
    memset((void *)(uintptr_t)c.x[2], 0x91, 0xc8);
    ST64(c.x[2] + 0x60, 2); ST32(c.x[2] + 0x6c, 1);
    unsigned char original[0xc8]; memcpy(original, (const void *)(uintptr_t)c.x[2], sizeof original);
    before = c; unsigned v = variants, d = variant_destructors;
    xcde_system_exit_dialog(&c);
    assert(!memcmp(&c, &before, sizeof c) && variants == v + 2 && variant_destructors == d + 2);
    assert(!memcmp(original + 0x40, (const void *)(uintptr_t)(c.x[2] + 0x40), sizeof original - 0x40));
    assert(bound_caption == dialog + 0x100);
    ST64(callback, 0x7000b55c08ull); ST64(callback + 8, child);
    memcpy((void *)(uintptr_t)callback_clone, (const void *)(uintptr_t)callback, 16);
    return c;
}

int main(void) {
    assert(UI_BYTES == 0x420); // Checked native storage boundary, independent of allocation accounting.
    assert(mmap((void *)(uintptr_t)HEAP_BASE, 0x80000, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)(uintptr_t)HEAP_BASE);
    assert(mmap((void *)(uintptr_t)HLE_HEAP, 0x40000, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)(uintptr_t)HLE_HEAP);
    MockUi *u = construct(sequence, false);
    assert(LD32(u->ui + 0x1c) == 7 && LD32(u->ui + 0x20) == 7);
    assert(LD64(u->ui + ROW_POINTER) == u->ui + ROW && LD32(u->ui + ROW_ID) == 707);
    assert(u->widget_constructors == 1 && u->labels == 1 && u->badges == 0 && u->visible == 1 && !u->enabled);
    Cpu ordinary = cpu(); ordinary.x[19] = child; ordinary.x[0] = dialog;
    ordinary.x[1] = child; ordinary.x[2] = stack_top + 0x18; ordinary.x[22] = dialog;
    Cpu ordinary_before = ordinary; unsigned ordinary_calls = calls;
    xcde_system_exit_dialog(&ordinary);
    assert(!memcmp(&ordinary, &ordinary_before, sizeof ordinary) && calls == ordinary_calls);
    ordinary.x[0] = callback;
    assert(!xcde_system_exit_reply(&ordinary) && !closes);
    for (uint32_t action = 0; action < 6; ++action) {
        ST32(u->ui + 0x28, action); Cpu c = cpu(); c.x[0] = u->ui; Cpu before = c;
        assert(xcde_system_exit_action(sequence, action) == action);
        assert(!xcde_system_exit_selected(&c) && !memcmp(&c, &before, sizeof c));
        assert(!xcde_system_exit_help(&c) && !memcmp(&c, &before, sizeof c));
    }
    Cpu c = cpu(); c.x[0] = u->ui; c.x[1] = 0x1234; Cpu before = c;
    xcde_system_exit_update(&c); assert(!memcmp(&c, &before, sizeof c) && !u->enabled && u->updates == 1);
    ST32(sequence + 0x50, 1u << 5);
    xcde_system_exit_update(&c); assert(!memcmp(&c, &before, sizeof c) && u->enabled && u->updates == 2);
    original_rows_preserved(u);
    ST32(u->ui + 0x28, 6); c = cpu(); c.x[0] = u->ui; before = c;
    assert(xcde_system_exit_help(&c) && !memcmp(&c, &before, sizeof c) && u->helps == 1);
    assert(xcde_system_exit_selected(&c)); before.x[0] = 6; assert(!memcmp(&c, &before, sizeof c));
    for (unsigned variant = 0; variant < 4; ++variant) {
        unsigned prior_closes = closes, prior_cancel = cancel_callbacks;
        c = arm_dialog(u);
        assert(closes == prior_closes); // Merely opening/holding the dialog never exits.
        c = cpu(); c.x[0] = variant & 1 ? callback_clone : callback; c.x[1] = variant < 2 ? 1 : 0;
        before = c; required_cancel = prior_cancel + 1;
        assert(xcde_system_exit_reply(&c) && !memcmp(&c, &before, sizeof c));
        assert(cancel_callbacks == prior_cancel + 1 && closes == prior_closes + (variant >= 2));
        assert(!bound_caption && !xcde_system_exit_reply(&c));
        assert(closes == prior_closes + (variant >= 2));
    }
    assert(closes == 2 && cancel_callbacks == 4);
    c = arm_dialog(u); ST32(c.x[2] + 0x6c, 0);
    EXPECT_FATAL(xcde_system_exit_dialog(&c)); // Default-Yes is never silently accepted.
    ST32(c.x[2] + 0x6c, 1);
    c = cpu(); c.x[0] = callback_clone; c.x[1] = 1; ST64(callback_clone, 0);
    EXPECT_FATAL(xcde_system_exit_reply(&c)); // Matching child with wrong delegate type.
    ST64(callback_clone, 0x7000b55c08ull);
    ST64(child + 8, sequence + 0x2000);
    EXPECT_FATAL(xcde_system_exit_reply(&c)); // Child cannot move to another owner.
    ST64(child + 8, sequence);
    unsigned destroyed = u->widget_destructors; destroy(u);
    assert(u->widget_destructors == destroyed + 1 && !bound_caption);

    // The binding remains reserved during a nested original constructor.
    nested_constructor = true;
    MockUi *outer = construct(sequence + 0x2000, true), *inner = by_ui(nested_ui);
    assert(outer->ui == u->ui && outer != inner && mock_count == 2);
    assert(LD32(inner->ui + 0x1c) == 7 && LD32(inner->ui + 0x20) == 7);
    original_rows_preserved(inner); destroy(inner); destroy(outer);
    for (missing_kind = 1; missing_kind <= 6; ++missing_kind) {
        unsigned before_widgets = widget_creations, before_labels = label_calls;
        MockUi *fallback = construct(sequence + 0x6000, true);
        assert(LD32(fallback->ui + 0x1c) == 6 && LD32(fallback->ui + 0x20) == 6);
        assert(!LD64(fallback->ui + ROW_POINTER)); original_rows_preserved(fallback);
        assert(widget_creations == before_widgets && label_calls == before_labels);
        assert(fallback->badges == 99 && fallback->visible == 99 && fallback->enabled == 99);
        unsigned prior_calls = calls, d = fallback->widget_destructors;
        ST32(fallback->ui + 0x28, 6); c = cpu(); c.x[0] = fallback->ui; before = c;
        xcde_system_exit_update(&c);
        assert(!xcde_system_exit_help(&c) && !xcde_system_exit_selected(&c));
        assert(calls == prior_calls && !memcmp(&c, &before, sizeof c));
        EXPECT_FATAL((void)xcde_system_exit_action(fallback->sequence, 6));
        destroy(fallback); assert(fallback->widget_destructors == d);
    }
    system_exit_reset();
    assert(!bindings && !text && closes == 2);
    assert(munmap((void *)(uintptr_t)HEAP_BASE, 0x80000) == 0);
    assert(munmap((void *)(uintptr_t)HLE_HEAP, 0x40000) == 0);
    puts("System Exit bridge: external storage/native-six preservation, restriction/state, confirmation/clone/Cancel, reservation/reuse and missing-row fallback passed");
    return 0;
}
