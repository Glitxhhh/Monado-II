// [XCDE] LINUX-033: real authored caption bridge, synthetic native UI contracts.
#define _GNU_SOURCE
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <sys/mman.h>
#include "../../runtime/hle/desktop_exit_caption.c"

static const uint64_t manager = HEAP_BASE + 0x1000, packet = HEAP_BASE + 0x3000;
static const uint64_t ui = HEAP_BASE + 0x4000, context = HEAP_BASE + 0x5000;
static const uint64_t manager_vtable = HEAP_BASE + 0x6000;
static const uint64_t endpoint_vtable = HEAP_BASE + 0x7000;
static const uint64_t stack_top = HEAP_BASE + 0x18000;
static unsigned calls, captions;
static unsigned rejected;
static bool fatal_expected;
static jmp_buf fatal_jump;

void rt_fatal_trace(void) {
    assert(fatal_expected);
    longjmp(fatal_jump, 1);
}

void guest_call(Cpu *c, uint64_t function) {
    assert(c->sp == stack_top - 0x400);
    ++calls;
    if (function == 0x7000591c10ull) {
        assert(c->x[0] == stack_top - 0x80 && c->x[1] == stack_top - 0x90);
        assert(!strcmp((const char *)(uintptr_t)c->x[1], "Exit"));
        ST64(c->x[0], c->x[1]); // Synthetic variant consumed by setter below.
    } else if (function == 0x7000204480ull) {
        assert(c->x[0] == ui && c->x[1] == 0 && c->x[2] == stack_top - 0x80);
        assert(!strcmp((const char *)(uintptr_t)LD64(c->x[2]), "Exit"));
        ++captions; // Choice1 and message references are never touched.
    } else if (function == 0x7000591d90ull) {
        assert(c->x[0] == stack_top - 0x80);
    } else assert(!"unexpected native caption contract");
    memset(c, 0xb7, sizeof *c); // Include FP/vector/TLS/stack/register clobbers.
}

static Cpu setup(void) {
    Cpu c;
    memset(&c, 0x39, sizeof c);
    c.sp = stack_top; c.x[19] = manager; c.x[20] = packet; c.x[21] = ui;
    memset((void *)(uintptr_t)packet, 0x63, 0x90);
    ST64(packet + 0x88, context); ST64(packet + 0x70, 2); ST32(packet + 0x7c, 1);
    // Native attach takes x0=&manager->endpoint and x1=client endpoint. It
    // stores x1 at x0+8, then x0 at x1+8: backlink is an OBJECT ADDRESS,
    // while the first word at that address is its independent vtable.
    ST64(manager + 0x70, manager_vtable); ST64(manager + 0x78, context);
    ST64(context, endpoint_vtable); ST64(context + 8, manager + 0x70);
    ST64(manager + 0x5f8, ui);
    assert(LD64(context + 8) == manager + 0x70);
    assert(LD64(context + 8) != LD64(manager + 0x70));
    return c;
}

static void expect_rejected(Cpu *c) {
    Cpu before = *c;
    unsigned before_calls = calls, before_captions = captions;
    unsigned char original[0x90];
    memcpy(original, (const void *)(uintptr_t)packet, sizeof original);
    fatal_expected = true;
    if (!setjmp(fatal_jump)) {
        xcde_exit_caption_render(c);
        assert(!"changed native ownership/layout must fail closed");
    }
    fatal_expected = false;
    assert(calls == before_calls && captions == before_captions);
    assert(!memcmp(c, &before, sizeof before));
    assert(!memcmp(original, (const void *)(uintptr_t)packet, sizeof original));
    ++rejected;
}

int main(void) {
    assert(mmap((void *)(uintptr_t)HEAP_BASE, 0x20000, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0)
           == (void *)(uintptr_t)HEAP_BASE);
    Cpu c = setup(), before = c;
    xcde_exit_caption_render(&c);
    assert(!calls && !memcmp(&c, &before, sizeof c));
    xcde_exit_caption_begin(context);
    ST64(packet + 0x88, context + 0x20);
    xcde_exit_caption_render(&c); // Other native popups keep their captions.
    assert(!calls && !memcmp(&c, &before, sizeof c));
    ST64(packet + 0x88, context);
    unsigned char original[0x90]; memcpy(original, (const void *)(uintptr_t)packet, sizeof original);
    xcde_exit_caption_render(&c);
    assert(calls == 3 && captions == 1 && !memcmp(&c, &before, sizeof c));
    assert(!memcmp(original, (const void *)(uintptr_t)packet, sizeof original));
    assert(LD64(manager + 0x70) == manager_vtable && LD64(context) == endpoint_vtable);
    assert(LD64(context + 8) == manager + 0x70);
    xcde_exit_caption_end(context + 0x20); // An unrelated client cannot clear it.
    xcde_exit_caption_render(&c); assert(captions == 2);
    xcde_exit_caption_end(context);
    xcde_exit_caption_render(&c); assert(captions == 2);
    xcde_exit_caption_begin(context);
    struct { uint64_t address, value; bool word; } corruptions[] = {
        // This is the old r2 fixture's incorrect vtable-as-backlink. It must
        // now be rejected, even though the manager's vtable matches that value.
        {context + 8, manager_vtable, false},
        {context + 8, manager + 0x80, false},
        {manager + 0x78, context + 0x20, false},
        {manager + 0x5f8, ui + 0x20, false},
        {packet + 0x70, 3, false},
        {packet + 0x7c, 0, true}, // Changed/default-Yes ABI stays forbidden.
    };
    for (unsigned i = 0; i < sizeof corruptions / sizeof corruptions[0]; ++i) {
        c = setup();
        if (corruptions[i].word)
            ST32(corruptions[i].address, (uint32_t)corruptions[i].value);
        else ST64(corruptions[i].address, corruptions[i].value);
        expect_rejected(&c);
    }
    c = setup(); c.x[19] = GUEST_LO - 8; expect_rejected(&c);
    c = setup(); c.x[21] = GUEST_HI; expect_rejected(&c);
    assert(rejected == 8);
    c = setup(); before = c;
    xcde_exit_caption_render(&c); // Rejections did not corrupt the valid binding.
    assert(captions == 3 && !memcmp(&c, &before, sizeof c));
    unsigned before_reset = calls;
    desktop_exit_caption_reset();
    xcde_exit_caption_render(&c);
    assert(calls == before_reset && captions == 3);
    assert(munmap((void *)(uintptr_t)HEAP_BASE, 0x20000) == 0);
    puts("desktop Exit caption: native positive-only update, endpoint address/vtable distinction, "
         "8 ownership/default rejections and Cpu preservation passed");
    return 0;
}
