// [XCDE] LINUX-033: authored synthetic objects/calls; no game files or graphics.
#define _GNU_SOURCE
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../../runtime/hle/title_exit.c"

static unsigned calls, closes, cancel_callbacks, constructors, destructors, labels;
static uint64_t caption_context;
static size_t allocated;
static bool fatal_expected;
static jmp_buf fatal_jump;
static const uint64_t sequence = HEAP_BASE + 0x1000, stack_top = HEAP_BASE + 0x10000;
static const uint64_t node = HEAP_BASE + 0x3000, choice = HEAP_BASE + 0x4000;

void rt_fatal_trace(void) {
    assert(fatal_expected);
    longjmp(fatal_jump, 1);
}

void *hle_alloc(uint64_t size) {
    assert(size && allocated + size < 0x10000);
    void *result = (void *)(uintptr_t)(HLE_HEAP + allocated);
    allocated += (size + 15) & ~(size_t)15;
    memset(result, 0, size);
    return result;
}

uint64_t plat_now_ns(void) { return 123456789; }
void am_request_host_exit(uint64_t now) { assert(now == plat_now_ns()); ++closes; }
void xcde_exit_caption_begin(uint64_t context) {
    assert(context == sequence + 0x210); caption_context = context;
}
void xcde_exit_caption_end(uint64_t context) {
    assert(context == sequence + 0x210); if (caption_context == context) caption_context = 0;
}

// Only contracts relevant to the authored bridge are modeled. The actual AM
// wake/lock/deadline implementation is tested separately with real kernel/IPC.
void guest_call(Cpu *c, uint64_t function) {
    uint64_t x0 = c->x[0], x1 = c->x[1], sp = c->sp;
    assert(sp == stack_top - 0x400 && !(sp & 15));
    ++calls;
    if (function == 0x700057c210ull) {
        assert(x0 == node && x1 >= HLE_HEAP && x1 < HLE_HEAP + allocated);
        assert(!strcmp((const char *)(uintptr_t)x1, "Exit to desktop"));
        ++labels;
    } else if (function == 0x7000591d90ull) {
        assert(x0 == stack_top + 0x28 || x0 == stack_top + 0x48);
        memset((void *)(uintptr_t)x0, 0, 32);
        ++destructors;
    } else if (function == 0x7000591c10ull) {
        assert(x1 >= HLE_HEAP && x1 < HLE_HEAP + allocated);
        const char *text = (const char *)(uintptr_t)x1;
        if (x0 == stack_top + 0x28) assert(!strcmp(text, "Exit game?"));
        else {
            assert(x0 == stack_top + 0x48);
            assert(!strcmp(text, "Exit to desktop? Unsaved progress will be lost."));
        }
        memset((void *)(uintptr_t)x0, 0x5a, 32);
        ++constructors;
    } else if (function == 0x70003c5ef0ull) {
        assert(LD64(x0) == 0x7000b56608ull && LD32(x1) == 1);
        assert(LD64(x0 + 8) == sequence + 0x200 && LD64(x0 + 0x10) == sequence);
        assert(!xcde_title_exit_pending(sequence));
        assert(!caption_context);
        assert(!xcde_title_exit_reply(c)); // Recursive original callback is unhooked.
        ST32(sequence + 0x270, 1); // Synthetic native Cancel bookkeeping.
        ST32(sequence + 0x1ac, UINT32_MAX);
        ++cancel_callbacks;
    } else assert(!"unexpected guest contract");
    // Deliberately clobber CPU, FP/vector/TLS/stack state to test restoration.
    memset(c, 0xa5, sizeof *c);
}

static Cpu cpu(void) {
    Cpu c;
    memset(&c, 0x37, sizeof c);
    c.sp = stack_top;
    return c;
}

static uint64_t ui(void) { return sequence + 0x90; }
static uint64_t row(unsigned index) { assert(index < 7); return ui() + 0x44 + 8 * index; }

static void configure(void) {
    title_exit_reset();
    memset((void *)(uintptr_t)sequence, 0, 0x1000);
    ST64(sequence + 0x110, ui());
    const int32_t actions[7] = {-1, 0, 2, 3, 4, 5, 6};
    for (unsigned i = 0; i < 7; ++i) {
        ST32(row(i), actions[i]);
        ST8(row(i) + 4, 1);
    }
    ST32(ui() + 0x7c, 4); // Options must stay selected after compaction.
    assert(xcde_title_exit_configure(ui()));
    assert(LD32(ui() + 0x7c) == 3 && LD32(row(3)) == 4);
    assert(LD32(row(4)) == 7 && LD32(row(5)) == 5 && LD32(row(6)) == 6);
}

static void arm(void) {
    ST32(ui() + 0x7c, 4);
    assert(xcde_title_exit_action(sequence, 7) == 6);
    assert(xcde_title_exit_pending(sequence));
    ST64(sequence + 0x248, 0x7000b56608ull);
    ST64(sequence + 0x250, sequence + 0x200);
    ST64(sequence + 0x258, sequence);
}

static void fatal_dispatch(void) {
    fatal_expected = true;
    if (!setjmp(fatal_jump)) {
        xcde_title_exit_check_dispatch(sequence, 7);
        assert(!"escaped Exit must fail closed");
    }
    fatal_expected = false;
}

int main(void) {
    assert(mmap((void *)(uintptr_t)HEAP_BASE, 0x20000, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0)
           == (void *)(uintptr_t)HEAP_BASE);
    assert(mmap((void *)(uintptr_t)HLE_HEAP, 0x10000, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0)
           == (void *)(uintptr_t)HLE_HEAP);
    configure();
    assert(xcde_title_exit_row_is_exit(ui(), row(4)));
    assert(!xcde_title_exit_row_is_exit(ui(), row(3)));
    assert(!xcde_title_exit_row_is_exit(ui(), row(4) + 1));
    assert(!xcde_title_exit_row_is_exit(0, row(4)));
    for (uint32_t action = 0; action <= 6; ++action)
        assert(xcde_title_exit_action(sequence, action) == action);
    assert(!closes && !calls);

    Cpu c = cpu();
    c.x[19] = ui(); c.x[25] = row(4); c.x[28] = c.x[0] = node;
    Cpu before = c;
    assert(xcde_title_exit_label(&c) && !memcmp(&c, &before, sizeof c));
    assert(labels == 1);
    c.x[25] = row(3); before = c;
    assert(!xcde_title_exit_label(&c) && !memcmp(&c, &before, sizeof c));
    assert(labels == 1);

    c = cpu(); c.x[19] = sequence; c.x[0] = sequence + 0x200; c.x[1] = stack_top + 0x20;
    memset((void *)(uintptr_t)c.x[1], 0x91, 0x80);
    ST64(c.x[1] + 0x68, 2); ST32(c.x[1] + 0x74, 1);
    unsigned char descriptor[0x80];
    memcpy(descriptor, (const void *)(uintptr_t)c.x[1], sizeof descriptor);
    before = c;
    xcde_title_exit_dialog(&c); // Ordinary native confirmation must be untouched.
    assert(!memcmp(&c, &before, sizeof c));
    assert(!memcmp(descriptor, (const void *)(uintptr_t)c.x[1], sizeof descriptor));
    arm();
    xcde_title_exit_dialog(&c);
    assert(!memcmp(&c, &before, sizeof c) && constructors == 2 && destructors == 2);
    assert(caption_context == sequence + 0x210);
    for (unsigned i = 0; i < sizeof descriptor; ++i)
        if (i < 8 || i >= 0x48) assert(descriptor[i] == LD8(c.x[1] + i));
    assert(!closes); // Opening/held initial action cannot itself close the game.

    c = cpu(); c.x[0] = sequence + 0x248; c.x[1] = choice; ST32(choice, 1);
    before = c;
    assert(xcde_title_exit_reply(&c) && !memcmp(&c, &before, sizeof c));
    assert(!closes && cancel_callbacks == 1 && LD32(choice) == 1);
    assert(LD32(sequence + 0x1ac) == UINT32_MAX);
    assert(!xcde_title_exit_reply(&c));

    // The native dialog can clone its callback delegate. Its logical owner and
    // vtable identify it; it need not retain its original physical address.
    const uint64_t clone = HEAP_BASE + 0x5000;
    memcpy((void *)(uintptr_t)clone, (const void *)(uintptr_t)(sequence + 0x248), 24);
    arm(); c.x[0] = clone; before = c;
    assert(xcde_title_exit_reply(&c) && !memcmp(&c, &before, sizeof c));
    assert(!closes && cancel_callbacks == 2);

    arm(); ST32(choice, 0); before = c;
    assert(xcde_title_exit_reply(&c) && !memcmp(&c, &before, sizeof c));
    assert(closes == 1 && cancel_callbacks == 3 && LD32(choice) == 0);
    assert(!xcde_title_exit_reply(&c) && closes == 1);
    assert(LD32(sequence + 0x1ac) == UINT32_MAX); // Never request a story switch.
    arm(); c.x[0] = sequence + 0x248; before = c;
    assert(xcde_title_exit_reply(&c) && !memcmp(&c, &before, sizeof c));
    assert(closes == 2 && cancel_callbacks == 4 && LD32(choice) == 0);
    fatal_dispatch();

    configure();
    // Fully occupied / unexpected menus must be preserved instead of losing an
    // existing hidden entry, including the latent Continue action1.
    for (unsigned i = 0; i < 7; ++i) ST32(row(i), i);
    unsigned char original[7 * 8];
    memcpy(original, (const void *)(uintptr_t)row(0), sizeof original);
    assert(!xcde_title_exit_configure(ui()));
    assert(!memcmp(original, (const void *)(uintptr_t)row(0), sizeof original));
    assert(!xcde_title_exit_pending(sequence));
    assert(!xcde_title_exit_configure(0));
    title_exit_reset();
    assert(!xcde_title_exit_pending(sequence) && closes == 2);
    assert(munmap((void *)(uintptr_t)HEAP_BASE, 0x20000) == 0);
    assert(munmap((void *)(uintptr_t)HLE_HEAP, 0x10000) == 0);
    puts("title Exit bridge: native-call/register preservation, dialog ownership, Cancel/confirm and existing rows passed");
    return 0;
}
