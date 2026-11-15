// [XCDE] LINUX-033: authored System-menu extension using the selected game's
// existing seventh row, font, navigation and confirmation. Native six widgets
// are constructed normally in a separate, extended guest allocation: the
// original inline storage ends immediately before its owner's live fields.
// No game implementation, layout, text table, image or font is included here.
#include "../hle.h"
#include "../services/am.h"
#include "system_exit.h"
#include "desktop_exit_caption.h"

enum { UI_BYTES = 0x420, UI_OWNER_POINTER = 0x400, ROW = 0x390,
       ROW_POINTER = 0x410, ROW_ID = 0x418 };

typedef struct SystemExitBinding {
    struct SystemExitBinding *next;
    uint64_t ui, sequence, child, dialog_context;
    bool live, pending, constructing, extra;
} SystemExitBinding;

typedef struct {
    char row[32], title[32], body[96], help[96];
    char node[32], text_node[64], help_node[32];
} SystemExitStrings;

static pthread_mutex_t system_lock = PTHREAD_MUTEX_INITIALIZER;
static SystemExitBinding *bindings;
static SystemExitStrings *text;

static bool range(uint64_t address, uint64_t bytes) {
    return address >= GUEST_LO && address < GUEST_HI && bytes <= GUEST_HI - address;
}

static SystemExitStrings *strings(void) {
    pthread_mutex_lock(&system_lock);
    if (!text) {
        text = hle_alloc(sizeof *text);
        strcpy(text->row, "Exit to desktop");
        strcpy(text->title, "Exit game?");
        strcpy(text->body, "Exit to desktop? Unsaved progress will be lost.");
        strcpy(text->help, "Exit to desktop. Unsaved progress will be lost.");
        strcpy(text->node, "rect_bar_07_mc");
        strcpy(text->text_node, "common_window_menu_mc/rect_text_mc");
        strcpy(text->help_node, "rect_tect_Main00_mc");
    }
    SystemExitStrings *result = text;
    pthread_mutex_unlock(&system_lock);
    return result;
}

static SystemExitBinding *find_ui(uint64_t ui) {
    SystemExitBinding *found = NULL;
    pthread_mutex_lock(&system_lock);
    for (SystemExitBinding *b = bindings; b; b = b->next)
        if (b->live && b->ui == ui) { found = b; break; }
    pthread_mutex_unlock(&system_lock);
    if (found && (!range(ui, UI_BYTES) || !range(found->sequence, 0x410)
            || LD64(found->sequence + UI_OWNER_POINTER) != ui))
        FATAL("[system-exit] native System UI owner mismatch\n");
    return found;
}

static SystemExitBinding *find_sequence(uint64_t sequence) {
    if (!range(sequence, 0x410)) return NULL;
    SystemExitBinding *b = find_ui(LD64(sequence + UI_OWNER_POINTER));
    return b && b->sequence == sequence ? b : NULL;
}

static SystemExitBinding *find_child(uint64_t child) {
    SystemExitBinding *found = NULL;
    pthread_mutex_lock(&system_lock);
    for (SystemExitBinding *b = bindings; b; b = b->next)
        if (b->live && b->extra && b->pending && b->child == child) { found = b; break; }
    pthread_mutex_unlock(&system_lock);
    if (found && (!range(child, 0xa8) || LD64(child + 8) != found->sequence
            || find_sequence(found->sequence) != found))
        FATAL("[system-exit] native confirmation owner mismatch\n");
    return found;
}

static uint64_t call(Cpu *c, uint64_t function, uint64_t x0, uint64_t x1,
                     uint64_t x2, uint64_t x3, uint64_t x4) {
    Cpu saved = *c;
    if (!range(c->sp, 1) || c->sp < GUEST_LO + 0x400)
        FATAL("[system-exit] invalid guest helper stack\n");
    c->x[0] = x0; c->x[1] = x1; c->x[2] = x2; c->x[3] = x3; c->x[4] = x4;
    c->sp = (c->sp - 0x400) & ~15ull;
    guest_call(c, function);
    uint64_t result = c->x[0];
    *c = saved;
    return result;
}

void xcde_system_exit_construct(Cpu *c) {
    uint64_t sequence = c->x[19], context = c->x[1], resource = c->x[2];
    if (!range(sequence, 0x410) || c->x[0] != sequence + 0x70
            || c->x[21] != c->x[0] || LD64(sequence + UI_OWNER_POINTER))
        FATAL("[system-exit] native System constructor layout mismatch\n");
    SystemExitBinding *b = NULL;
    pthread_mutex_lock(&system_lock);
    for (b = bindings; b && (b->live || b->constructing); b = b->next) {}
    if (!b) {
        b = calloc(1, sizeof *b);
        if (!b) FATAL("[system-exit] allocating owner record failed\n");
        b->ui = (uintptr_t)hle_alloc(UI_BYTES);
        b->next = bindings;
        bindings = b;
    }
    b->sequence = sequence;
    b->constructing = true;
    b->extra = false;
    b->pending = false;
    b->child = b->dialog_context = 0;
    uint64_t ui = b->ui;
    pthread_mutex_unlock(&system_lock);
    memset((void *)(uintptr_t)ui, 0, UI_BYTES);
    // Re-enter the complete original constructor. No original widget bytes,
    // ownership or reference counts are copied to the extended allocation.
    call(c, 0x7000293c00ull, ui, context, resource, 0, 0);
    if (!range(LD64(ui + 8), 0x20) || LD32(ui + 0x1c) != 6 || LD32(ui + 0x20) != 6)
        FATAL("[system-exit] native six-row System layout changed\n");
    for (unsigned i = 0; i < 6; ++i)
        if (LD64(ui + 0xb0 + i * 0x90) != ui + 0x30 + i * 0x90
                || (int32_t)LD32(ui + 0xb8 + i * 0x90) < 0)
            FATAL("[system-exit] original System row is missing\n");
    SystemExitStrings *s = strings();
    uint64_t scratch = c->sp - 0x80, root = LD64(ui + 8);
    ST64(scratch, UINT64_MAX);
    if (!(call(c, 0x700057a8a0ull, root, scratch, LD32(ui + 0x14),
               (uintptr_t)s->node, 0) & 1))
        goto ready;
    uint64_t node = call(c, 0x700057a800ull, root, scratch, 0, 0, 0);
    if (!range(node, 0x20) || (int32_t)LD32(scratch + 4) < 0)
        goto ready;
    // Preflight the unused node before creating an owning widget or making it
    // visible. A resource without the optional row/text keeps the original six.
    uint64_t vtable = LD64(node);
    if (!range(vtable, 0x28) || !range(LD64(vtable + 0x20), 4)) goto ready;
    uint64_t row_root = call(c, 0x7000579860ull, node, 0, 0, 0, 0);
    if (!range(row_root, 0x20)) goto ready;
    uint64_t label = call(c, 0x700057a770ull, row_root, (uintptr_t)s->text_node, 0, 0, 0);
    if (!range(label, 0x60)) goto ready;
    ST64(scratch + 0x10, node);
    call(c, 0x7000261640ull, ui + ROW, scratch + 0x10, context, 153, 0);
    ST64(ui + ROW_POINTER, ui + ROW);
    ST32(ui + ROW_ID, LD32(scratch + 4));
    // Original construction hides unused resource rows. Make this native node
    // visible again, then use the same widget state setters as the first six.
    call(c, LD64(vtable + 0x20), node, 1, 0, 0, 0);
    call(c, 0x7000261c10ull, ui + ROW, 0, 0, 0, 0);
    call(c, 0x7000261c70ull, ui + ROW, 0, 0, 0, 0);
    call(c, 0x70002619b0ull, ui + ROW, 0, 0, 0, 0);
    call(c, 0x7000261ac0ull, ui + ROW, (LD32(sequence + 0x50) >> 5) & 1, 0, 0, 0);
    call(c, 0x700057c210ull, label, (uintptr_t)s->row, 0, 0, 0);
    ST32(ui + 0x1c, 7);
    ST32(ui + 0x20, 7);
    b->extra = true;
ready:
    pthread_mutex_lock(&system_lock);
    b->live = true;
    b->constructing = false;
    pthread_mutex_unlock(&system_lock);
    // The parent stores this register in its existing owning pointer. Its
    // destructor/cleanup/resume paths all use that pointer, not inline storage.
    c->x[21] = ui;
    if (b->extra)
        LOG("[system-exit] seventh native System row configured; original six retained\n");
    else LOG("[system-exit] optional Exit row unavailable; original six System rows retained\n");
}

void xcde_system_exit_destroy(Cpu *c) {
    SystemExitBinding *b = find_ui(c->x[0]);
    if (!b) return;
    uint64_t context = b->dialog_context;
    if (b->extra) call(c, 0x7000261830ull, b->ui + ROW, 0, 0, 0, 0);
    ST64(b->ui + ROW_POINTER, 0);
    pthread_mutex_lock(&system_lock);
    b->live = b->pending = b->extra = false;
    b->child = b->dialog_context = 0;
    pthread_mutex_unlock(&system_lock);
    if (context) xcde_exit_caption_end(context);
}

void xcde_system_exit_update(Cpu *c) {
    SystemExitBinding *b = find_ui(c->x[0]);
    if (b && b->extra) {
        // Exit follows the existing Title Screen permission, including native
        // restrictions while saving or when the current game state disallows it.
        call(c, 0x7000261ac0ull, b->ui + ROW,
             (LD32(b->sequence + 0x50) >> 5) & 1, 0, 0, 0);
        call(c, 0x70002619a0ull, b->ui + ROW, c->x[1], 0, 0, 0);
    }
}

bool xcde_system_exit_help(Cpu *c) {
    SystemExitBinding *b = find_ui(c->x[0]);
    if (!b || !b->extra || LD32(b->ui + 0x28) != 6) return false;
    uint64_t help = call(c, 0x700057a830ull, LD64(b->ui + 8), LD32(b->ui + 0x18),
                         (uintptr_t)strings()->help_node, 0, 0);
    if (!range(help, 0x60)) FATAL("[system-exit] System help node is missing\n");
    call(c, 0x700057c210ull, help, (uintptr_t)strings()->help, 0, 0, 0);
    return true;
}

bool xcde_system_exit_selected(Cpu *c) {
    SystemExitBinding *b = find_ui(c->x[0]);
    if (!b || !b->extra || LD32(b->ui + 0x28) != 6) return false;
    c->x[0] = 6;
    return true;
}

uint32_t xcde_system_exit_action(uint64_t sequence, uint32_t action) {
    SystemExitBinding *b = find_sequence(sequence);
    if (action != 6) {
        if (b && !b->child) {
            pthread_mutex_lock(&system_lock);
            b->pending = false;
            pthread_mutex_unlock(&system_lock);
        }
        return action;
    }
    if (!b || !b->extra || LD32(b->ui + 0x28) != 6)
        FATAL("[system-exit] Exit action has no matching selected owner\n");
    pthread_mutex_lock(&system_lock);
    b->pending = true;
    pthread_mutex_unlock(&system_lock);
    return 5; // Temporary register only: reuse Title Screen's confirmation.
}

void xcde_system_exit_child(Cpu *c) {
    SystemExitBinding *b = find_sequence(c->x[19]);
    if (!b || !b->pending) return;
    uint64_t child = c->x[22];
    if (!range(child, 0xa8) || LD64(child + 8) != b->sequence)
        FATAL("[system-exit] native confirmation child is not bound to System\n");
    pthread_mutex_lock(&system_lock);
    b->child = child;
    pthread_mutex_unlock(&system_lock);
}

void xcde_system_exit_dialog(Cpu *c) {
    SystemExitBinding *b = find_child(c->x[19]);
    if (!b) return;
    uint64_t descriptor = c->x[2], dialog = c->x[0];
    if (c->x[1] != b->child || dialog != c->x[22] || !range(dialog, 0x1a0)
            || descriptor != c->sp + 0x18 || !range(descriptor, 0xc8)
            || LD64(descriptor + 0x60) != 2 || LD32(descriptor + 0x6c) != 1)
        FATAL("[system-exit] native confirmation descriptor changed\n");
    SystemExitStrings *s = strings();
    call(c, 0x7000591d90ull, descriptor, 0, 0, 0, 0);
    call(c, 0x7000591c10ull, descriptor, (uintptr_t)s->title, 0, 0, 0);
    call(c, 0x7000591d90ull, descriptor + 0x20, 0, 0, 0, 0);
    call(c, 0x7000591c10ull, descriptor + 0x20, (uintptr_t)s->body, 0, 0, 0);
    pthread_mutex_lock(&system_lock);
    b->dialog_context = dialog + 0x100;
    pthread_mutex_unlock(&system_lock);
    xcde_exit_caption_begin(dialog + 0x100);
}

bool xcde_system_exit_reply(Cpu *c) {
    uint64_t callback = c->x[0];
    if (!range(callback, 0x10)) return false;
    SystemExitBinding *b = find_child(LD64(callback + 8));
    if (!b) return false;
    if (LD64(callback) != 0x7000b55c08ull || !b->dialog_context)
        FATAL("[system-exit] native confirmation callback changed\n");
    bool confirmed = (uint32_t)c->x[1] == 0;
    uint64_t context = b->dialog_context;
    pthread_mutex_lock(&system_lock);
    b->pending = false;
    b->child = b->dialog_context = 0;
    pthread_mutex_unlock(&system_lock);
    xcde_exit_caption_end(context);
    // Let the original callback finish as Cancel, so its child never takes
    // the original title-screen transition. Incoming native choice is intact.
    call(c, 0x70003b6350ull, callback, 1, 0, 0, 0);
    if (confirmed) {
        LOG("[system-exit] exit confirmed; requesting ordinary guest cleanup\n");
        am_request_host_exit(plat_now_ns());
    } else LOG("[system-exit] exit cancelled; original System menu retained\n");
    return true;
}

RT_ON_RESET(system_exit_reset) {
    pthread_mutex_lock(&system_lock);
    while (bindings) {
        SystemExitBinding *next = bindings->next;
        free(bindings);
        bindings = next;
    }
    text = NULL;
    pthread_mutex_unlock(&system_lock);
}
