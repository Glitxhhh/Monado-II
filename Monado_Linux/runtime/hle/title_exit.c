// [XCDE] LINUX-033: authored adaptation of the title UI's existing spare row and
// native confirmation. No layout, font, texture, localized game text or lifted
// implementation is embedded here. The selected game's checked patches supply
// these ABI anchors; all original menu actions remain owned by the guest.
#include "../hle.h"
#include "../services/am.h"
#include "title_exit.h"
#include "title_exit_model.h"
#include "desktop_exit_caption.h"

enum {
    TITLE_UI = 0x90, TITLE_UI_POINTER = 0x110, TITLE_ROWS = 0x44,
    TITLE_SELECTED = 0x7c, TITLE_DIALOG = 0x200,
};

typedef struct {
    char label[32], title[32], body[96];
    int32_t no;
} TitleExitStrings;

static pthread_mutex_t title_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t title_ui, pending_sequence;
static TitleExitStrings *title_strings;

static bool guest_range(uint64_t address, uint64_t size) {
    return address >= GUEST_LO && address < GUEST_HI && size <= GUEST_HI - address;
}

static bool title_owner(uint64_t ui, uint64_t *sequence) {
    if (!guest_range(ui, 0x80) || (ui & 7) || ui < GUEST_LO + TITLE_UI) return false;
    uint64_t owner = ui - TITLE_UI;
    if (!guest_range(owner, 0x280) || LD64(owner + TITLE_UI_POINTER) != ui) return false;
    *sequence = owner;
    return true;
}

static TitleExitStrings *strings(void) {
    pthread_mutex_lock(&title_lock);
    if (!title_strings) {
        title_strings = hle_alloc(sizeof *title_strings);
        memcpy(title_strings->label, "Exit to desktop", sizeof "Exit to desktop");
        memcpy(title_strings->title, "Exit game?", sizeof "Exit game?");
        memcpy(title_strings->body, "Exit to desktop? Unsaved progress will be lost.",
               sizeof "Exit to desktop? Unsaved progress will be lost.");
        title_strings->no = 1;
    }
    TitleExitStrings *result = title_strings;
    pthread_mutex_unlock(&title_lock);
    return result;
}

// Same guest-thread calling convention as the existing authored party helpers:
// no nested call may leak its CPU/FP/TLS/register/stack state into a hook site.
static void call(Cpu *c, uint64_t function, uint64_t x0, uint64_t x1) {
    Cpu saved = *c;
    if (!guest_range(c->sp, 1) || c->sp < GUEST_LO + 0x400)
        FATAL("[title-exit] invalid guest helper stack\n");
    c->x[0] = x0;
    c->x[1] = x1;
    c->sp = (c->sp - 0x400) & ~15ull;
    guest_call(c, function);
    *c = saved;
}

bool xcde_title_exit_configure(uint64_t ui) {
    uint64_t sequence;
    if (!title_owner(ui, &sequence)) {
        LOG("[title-exit] title owner mismatch; original menu preserved\n");
        return false;
    }
    TitleExitRow before[7], updated[7];
    memcpy(before, (const void *)(uintptr_t)(ui + TITLE_ROWS), sizeof before);
    memcpy(updated, before, sizeof updated);
    if (!xcde_title_exit_insert_rows(updated)) {
        LOG("[title-exit] no verified spare title row; original menu preserved\n");
        pthread_mutex_lock(&title_lock);
        uint64_t old_pending = title_ui == ui ? pending_sequence : 0;
        if (title_ui == ui) title_ui = pending_sequence = 0;
        pthread_mutex_unlock(&title_lock);
        if (old_pending) xcde_exit_caption_end(old_pending + TITLE_DIALOG + 0x10);
        return false;
    }
    int32_t selected = (int32_t)LD32(ui + TITLE_SELECTED), next = -1;
    if (selected >= 0 && selected < 7 && before[selected].action >= 0)
        for (int i = 0; i < 7; ++i)
            if (updated[i].action == before[selected].action) next = i;
    memcpy((void *)(uintptr_t)(ui + TITLE_ROWS), updated, sizeof updated);
    ST32(ui + TITLE_SELECTED, next);
    pthread_mutex_lock(&title_lock);
    uint64_t old_pending = pending_sequence;
    title_ui = ui;
    pending_sequence = 0;
    pthread_mutex_unlock(&title_lock);
    if (old_pending) xcde_exit_caption_end(old_pending + TITLE_DIALOG + 0x10);
    LOG("[title-exit] spare title row configured; existing actions preserved\n");
    return true;
}

bool xcde_title_exit_row_is_exit(uint64_t ui, uint64_t row) {
    uint64_t sequence;
    if (!title_owner(ui, &sequence) || row < ui + TITLE_ROWS || row >= ui + TITLE_ROWS + 7 * 8
            || (row - ui - TITLE_ROWS) % 8) return false;
    pthread_mutex_lock(&title_lock);
    bool known = title_ui == ui;
    pthread_mutex_unlock(&title_lock);
    return known && (int32_t)LD32(row) == 7;
}

bool xcde_title_exit_label(Cpu *c) {
    if (!xcde_title_exit_row_is_exit(c->x[19], c->x[25])) return false;
    if (c->x[0] != c->x[28] || !guest_range(c->x[28], 0x60))
        FATAL("[title-exit] invalid title text node\n");
    call(c, 0x700057c210ull, c->x[28], (uintptr_t)strings()->label);
    return true;
}

uint32_t xcde_title_exit_action(uint64_t sequence, uint32_t action) {
    if (action != 7) return action;
    uint64_t ui = sequence + TITLE_UI, owner;
    if (!title_owner(ui, &owner) || owner != sequence)
        FATAL("[title-exit] exit action has no matching title owner\n");
    int32_t selected = (int32_t)LD32(ui + TITLE_SELECTED);
    if (selected < 0 || selected >= 7 || !xcde_title_exit_row_is_exit(ui, ui + TITLE_ROWS + selected * 8))
        FATAL("[title-exit] exit action has no matching selected row\n");
    pthread_mutex_lock(&title_lock);
    pending_sequence = sequence;
    pthread_mutex_unlock(&title_lock);
    // Only the temporary action register uses the original confirmation route.
    // Its native reply is always completed as Cancel, never a story-mode switch.
    return 6;
}

bool xcde_title_exit_pending(uint64_t sequence) {
    pthread_mutex_lock(&title_lock);
    bool pending = sequence && pending_sequence == sequence && title_ui == sequence + TITLE_UI;
    pthread_mutex_unlock(&title_lock);
    return pending;
}

void xcde_title_exit_dialog(Cpu *c) {
    uint64_t sequence = c->x[19], descriptor = c->x[1];
    if (!xcde_title_exit_pending(sequence)) return;
    if (c->x[0] != sequence + TITLE_DIALOG || descriptor != c->sp + 0x20
            || !guest_range(descriptor, 0x80) || LD64(descriptor + 0x68) != 2
            || LD32(descriptor + 0x74) != 1)
        FATAL("[title-exit] confirmation descriptor differs from its checked layout\n");
    TitleExitStrings *text = strings();
    call(c, 0x7000591d90ull, descriptor + 8, 0);
    call(c, 0x7000591c10ull, descriptor + 8, (uintptr_t)text->title);
    call(c, 0x7000591d90ull, descriptor + 0x28, 0);
    call(c, 0x7000591c10ull, descriptor + 0x28, (uintptr_t)text->body);
    xcde_exit_caption_begin(sequence + TITLE_DIALOG + 0x10);
    // Choice references, count, style, default Cancel, callbacks and ownership
    // remain exactly as constructed by the original native dialog.
}

bool xcde_title_exit_reply(Cpu *c) {
    uint64_t callback = c->x[0], reply = c->x[1];
    if (!guest_range(callback, 0x18) || !guest_range(reply, 4)) return false;
    uint64_t sequence = LD64(callback + 0x10);
    if (!xcde_title_exit_pending(sequence)) return false;
    // The native delegate has a clone operation: its address may differ from
    // the title object's inline delegate. Match the checked type and owners.
    if (LD64(callback) != 0x7000b56608ull || LD64(callback + 8) != sequence + TITLE_DIALOG)
        FATAL("[title-exit] confirmation callback owner mismatch\n");
    bool confirmed = (int32_t)LD32(reply) == 0;
    pthread_mutex_lock(&title_lock);
    pending_sequence = 0;
    pthread_mutex_unlock(&title_lock);
    xcde_exit_caption_end(sequence + TITLE_DIALOG + 0x10);
    // Re-enter the original callback with our own No value after clearing the
    // pending hook. Do not mutate the UI-owned reply or copy its implementation.
    call(c, 0x70003c5ef0ull, callback, (uintptr_t)&strings()->no);
    if (confirmed) {
        LOG("[title-exit] exit confirmed; requesting ordinary guest cleanup\n");
        am_request_host_exit(plat_now_ns());
    } else LOG("[title-exit] exit cancelled; original title controller retained\n");
    return true;
}

void xcde_title_exit_check_dispatch(uint64_t sequence, uint32_t action) {
    (void)sequence;
    if (action == 7) FATAL("[title-exit] exit action escaped native confirmation\n");
}

RT_ON_RESET(title_exit_reset) {
    pthread_mutex_lock(&title_lock);
    uint64_t old_pending = pending_sequence;
    title_ui = pending_sequence = 0;
    title_strings = NULL;
    pthread_mutex_unlock(&title_lock);
    if (old_pending) xcde_exit_caption_end(old_pending + TITLE_DIALOG + 0x10);
}
