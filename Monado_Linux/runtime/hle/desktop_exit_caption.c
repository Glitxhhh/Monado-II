// [XCDE] LINUX-033: change only the bound Exit dialog's positive caption, through
// native string/UI calls. No global message table, localization data or assets.
#include "../hle.h"
#include "desktop_exit_caption.h"

static pthread_mutex_t caption_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t caption_context;

static bool caption_range(uint64_t address, uint64_t bytes) {
    return address >= GUEST_LO && address < GUEST_HI && bytes <= GUEST_HI - address;
}

void xcde_exit_caption_begin(uint64_t context) {
    if (!caption_range(context, 0x10) || (context & 7))
        FATAL("[desktop-exit] invalid confirmation context\n");
    pthread_mutex_lock(&caption_lock);
    caption_context = context;
    pthread_mutex_unlock(&caption_lock);
}

void xcde_exit_caption_end(uint64_t context) {
    pthread_mutex_lock(&caption_lock);
    if (caption_context == context) caption_context = 0;
    pthread_mutex_unlock(&caption_lock);
}

static void caption_call(Cpu *c, uint64_t function, uint64_t x0, uint64_t x1, uint64_t x2) {
    Cpu saved = *c;
    c->x[0] = x0; c->x[1] = x1; c->x[2] = x2;
    c->sp = (c->sp - 0x400) & ~15ull;
    guest_call(c, function);
    *c = saved;
}

void xcde_exit_caption_render(Cpu *c) {
    pthread_mutex_lock(&caption_lock);
    uint64_t context = caption_context;
    pthread_mutex_unlock(&caption_lock);
    if (!context) return;
    uint64_t manager = c->x[19], packet = c->x[20], ui = c->x[21];
    if (!caption_range(packet, 0x90) || LD64(packet + 0x88) != context) return;
    bool manager_valid = caption_range(manager, 0x600);
    uint64_t attached = manager_valid ? LD64(manager + 0x78) : 0;
    uint64_t bound_ui = manager_valid ? LD64(manager + 0x5f8) : 0;
    uint64_t manager_vtable = manager_valid ? LD64(manager + 0x70) : 0;
    uint64_t backlink = LD64(context + 8), choices = LD64(packet + 0x70);
    uint32_t default_choice = LD32(packet + 0x7c);
    // Native attach links the two endpoint OBJECTS. The first word of the
    // manager endpoint is its vtable, not the address stored in the backlink.
    unsigned mismatch = (!manager_valid ? 1u : 0u)
                      | (!caption_range(ui, 0x80) ? 2u : 0u)
                      | (attached != context ? 4u : 0u)
                      | (backlink != manager + 0x70 ? 8u : 0u)
                      | (bound_ui != ui ? 16u : 0u)
                      | (choices != 2 ? 32u : 0u)
                      | (default_choice != 1 ? 64u : 0u);
    if (mismatch)
        FATAL("[desktop-exit] native confirmation ownership/layout mismatch "
              "mask=%#x manager=%#llx context=%#llx attached=%#llx "
              "backlink=%#llx expected=%#llx vtable=%#llx "
              "ui=%#llx bound_ui=%#llx choices=%llu default=%u\n",
              mismatch, (unsigned long long)manager, (unsigned long long)context,
              (unsigned long long)attached, (unsigned long long)backlink,
              (unsigned long long)(manager + 0x70), (unsigned long long)manager_vtable,
              (unsigned long long)ui, (unsigned long long)bound_ui,
              (unsigned long long)choices, default_choice);
    if (!caption_range(c->sp, 1) || c->sp < GUEST_LO + 0x400)
        FATAL("[desktop-exit] invalid confirmation scratch stack\n");
    // These temporaries are below the hooked caller's stack, above the nested
    // calls' stack. Native setter copies the variant before its destructor.
    uint64_t variant = c->sp - 0x80, text = c->sp - 0x90;
    memset((void *)(uintptr_t)variant, 0, 32);
    memcpy((void *)(uintptr_t)text, "Exit", sizeof "Exit");
    caption_call(c, 0x7000591c10ull, variant, text, 0);
    caption_call(c, 0x7000204480ull, ui, 0, variant);
    caption_call(c, 0x7000591d90ull, variant, 0, 0);
}

RT_ON_RESET(desktop_exit_caption_reset) {
    pthread_mutex_lock(&caption_lock);
    caption_context = 0;
    pthread_mutex_unlock(&caption_lock);
}
