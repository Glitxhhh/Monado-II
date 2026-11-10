// [XCDE] LINUX-040: adapt the original gem picker, without staged equipment writes.
#include "../hle.h"
#include "../settings.h"
#include "quickstep_native_ui.h"
#include "quickstep_picker.h"
#include "quickstep_slot.h"
#include "quickstep_stats.h"

enum { VECTOR = 0xe0, COUNT = 0x1080, CHARACTER = 0x18b8, PANE = 0x18c0,
       UI = 0x1c48, LIST = 0x4c10, SELECTED = 0x24, UI_PANE = 0x18,
       ROW = 0x6c, GEM_BYTES = 0x2c, ITEM_VIEW_BYTES = 0x140 };
typedef struct {
    uint64_t seq, context, generation, ui, list, panel, serial;
    uint32_t character, selected, pane, ui_pane;
    bool active, opening, accepting, closing, restoring;
} Picker;
static pthread_mutex_t picker_lock = PTHREAD_MUTEX_INITIALIZER;
static Picker picker;
typedef struct CloseFrame {
    Cpu *cpu;
    uint64_t sequence, serial, epoch, token;
    struct CloseFrame *next;
} CloseFrame;
// Guest fibers can migrate between host workers while helpers yield. Keep
// these live call-frame leases under the same mutex, never in host TLS.
static CloseFrame *close_frames;
static uint64_t close_epoch, close_token;
typedef struct {
    Cpu *builder;
    uint64_t frame,seq,ui,panel,context,generation,serial,dialog,callback,fallback_callback;
    uint32_t character;
    bool building,published,closing;
} ContextMenu;
static ContextMenu context_menu;
static uint64_t serial;
static unsigned diagnostic_actions;

static bool range(uint64_t p, size_t n) {
    return p >= GUEST_LO && p < GUEST_HI && n <= GUEST_HI - p;
}
static Picker snapshot(void) {
    pthread_mutex_lock(&picker_lock);
    Picker p = picker;
    pthread_mutex_unlock(&picker_lock);
    return p;
}
static ContextMenu menu_snapshot(void) {
    pthread_mutex_lock(&picker_lock); ContextMenu m=context_menu;
    pthread_mutex_unlock(&picker_lock); return m;
}
static bool menu_owner(const ContextMenu *m) {
    uint64_t seq,ctx,gen;
    if (!g_settings.quickstep_slot || !range(m->seq,0x8a20)
        || !range(m->ui,0x70) || LD64(m->seq+UI)!=m->ui
        || LD64(m->seq+0x8440)!=m->panel || LD32(m->seq+CHARACTER)!=m->character
        || (LD32(m->seq+PANE)!=1 && LD32(m->seq+PANE)!=2)
        || (LD32(m->ui+SELECTED)!=5 && LD32(m->ui+SELECTED)!=13)) return false;
    return quickstep_native_stats_context(m->panel,m->character,&seq,&ctx,&gen)
        && seq==m->seq && ctx==m->context && gen==m->generation;
}
static bool menu_dialog(const ContextMenu *m) {
    return range(m->dialog,0x750) && LD64(m->dialog)==0x7000b4d418ull
        && m->callback==m->dialog+0x188 && LD64(m->dialog+0x180)==m->callback
        && range(m->callback,0x10) && LD64(m->callback)==0x7000b4d888ull
        && LD64(m->callback+8)==m->seq;
}
static void menu_end(uint64_t owner_serial) {
    pthread_mutex_lock(&picker_lock);
    if(context_menu.serial==owner_serial) {
        // Retain only exact callback provenance until another completed native
        // menu instance replaces it. A late old callback cannot detach gear.
        context_menu.building=context_menu.published=context_menu.closing=false;
    }
    pthread_mutex_unlock(&picker_lock);
}
bool quickstep_native_picker_context_menu_active(uint64_t seq) {
    ContextMenu m=menu_snapshot();
    if(!m.published || m.seq!=seq) return false;
    bool valid=menu_owner(&m) && menu_dialog(&m)
        && (m.closing || (LD64(seq+0x87a8)==seq+0x87b0
            && LD64(seq+0x87b0)==0x7000b4da00ull));
    if(!valid) menu_end(m.serial);
    return valid;
}
static bool live(const Picker *p) {
    uint64_t ctx, gen; uint32_t ch;
    Picker now = snapshot();
    return p->active && now.active && !now.restoring && p->serial == now.serial
        && g_settings.quickstep_slot && range(p->seq, 0x8a20)
        && range(p->ui, 0x70) && range(p->list, 0xa00)
        && LD64(p->seq + UI) == p->ui && LD64(p->seq + LIST) == p->list
        && LD32(p->seq + CHARACTER) == p->character
        && quickstep_native_bonus_context(p->seq, &ctx, &ch, &gen)
        && ctx == p->context && ch == p->character && gen == p->generation;
}
static bool owned(uint64_t seq) {
    Picker p = snapshot();
    if (p.active && p.seq == seq) return true; // A stale bonus callback must fail closed.
    uint64_t ctx, gen; uint32_t ch;
    return g_settings.quickstep_slot
        && quickstep_native_bonus_context(seq, &ctx, &ch, &gen);
}
static bool same_focus(uint64_t seq, uint64_t context, uint32_t character, uint64_t generation) {
    uint64_t ctx, gen; uint32_t ch;
    return g_settings.quickstep_slot
        && quickstep_native_bonus_context(seq, &ctx, &ch, &gen)
        && ctx == context && ch == character && gen == generation;
}
static bool picker_owner(const Picker *p) {
    uint64_t seq = 0, ctx = 0, gen = 0;
    return range(p->seq, 0x8a20) && range(p->ui, 0x70)
        && LD64(p->seq + UI) == p->ui && LD64(p->seq + LIST) == p->list
        && LD32(p->seq + CHARACTER) == p->character
        && quickstep_native_stats_context(p->panel, p->character, &seq, &ctx, &gen)
        && seq == p->seq && ctx == p->context && gen == p->generation;
}
static uint64_t call(Cpu *c, uint64_t fn, uint64_t a, uint64_t b,
                     uint64_t d, uint64_t e) {
    Cpu saved = *c;
    uint64_t fpcr = rt_get_fpcr(), fpsr = rt_get_fpsr();
    if ((c->sp & 15) || c->sp < GUEST_LO + 0x1000 || !range(c->sp - 0x1000, 0x1000))
        FATAL("[quickstep-picker] invalid native helper stack\n");
    c->sp -= 0x1000;
    c->x[0] = a; c->x[1] = b; c->x[2] = d; c->x[3] = e;
    guest_call(c, fn);
    uint64_t result = c->x[0];
    *c = saved;
    rt_set_fpcr(fpcr); rt_set_fpsr(fpsr);
    return result;
}
static bool identity(uint64_t item, const QuickstepGem *gem) {
    if (!range(item, GEM_BYTES) || LD32(item) != gem->uid
        || LD16(item + 6) != 3 || !LD8(item + 0x10)
        || LD8(item + 0x1a) != 1 || LD16(item + 0x1c) != 77
        || LD8(item + 0x16) < 1 || LD8(item + 0x16) > 6) return false;
    uint8_t fp[32];
    memcpy(fp, (void *)(uintptr_t)(item + 0xc), 4);
    memcpy(fp + 4, (void *)(uintptr_t)(item + 4), 4);
    memcpy(fp + 8, (void *)(uintptr_t)(item + 0x14), 24);
    return !memcmp(fp, gem->fingerprint, sizeof fp);
}
bool quickstep_native_picker_active(uint64_t seq) {
    Picker p = snapshot();
    return p.active && p.seq == seq;
}
void quickstep_native_picker_filter(Cpu *c) {
    Picker p = snapshot();
    if (!p.active || p.restoring || c->x[0] != p.seq + VECTOR) return;
    if (!range(p.seq, COUNT + 8)) return;
    uint64_t keep = 0, rows[QUICKSTEP_GEM_CAPACITY];
    QuickstepGem gems[QUICKSTEP_GEM_CAPACITY];
    size_t n = live(&p) ? quickstep_slot_list(c, p.context, p.character,
                                             gems, QUICKSTEP_GEM_CAPACITY) : 0;
    // The inventory helper can yield through teardown/reuse of the equipment
    // object. A replacement owner must receive no vector/count writes at all.
    if (!live(&p)) return;
    uint64_t count = LD64(p.seq + COUNT);
    if (count <= QUICKSTEP_GEM_CAPACITY && n <= QUICKSTEP_GEM_CAPACITY)
        for (uint64_t i = 0; i < count; ++i) {
            uint64_t item = LD64(p.seq + VECTOR + i * 8);
            for (size_t g = 0; g < n; ++g)
                if (gems[g].availability == QUICKSTEP_FREE && identity(item, &gems[g])) {
                    rows[keep++] = item; break;
                }
        }
    // Compute in host scratch and publish only to this exact current owner.
    // A corrupt count may clear rows only while that ownership remains valid.
    if (!live(&p)) return;
    for (uint64_t i = 0; i < keep; ++i) ST64(p.seq + VECTOR + i * 8, rows[i]);
    for (uint64_t i = keep; i < QUICKSTEP_GEM_CAPACITY; ++i)
        ST64(p.seq + VECTOR + i * 8, 0);
    ST64(p.seq + COUNT, keep);
}
void quickstep_native_picker_count(Cpu *c) {
    Picker p = snapshot();
    if (!p.active || p.restoring || c->x[0] != p.list + 0x58) return;
    if (!live(&p)) { c->x[1] = 0; return; }
    uint64_t count = LD64(p.seq + COUNT);
    if (count > QUICKSTEP_GEM_CAPACITY) count = 0;
    call(c, 0x700020af90ull, p.list, count == 0, 0, 0);
    c->x[1] = live(&p) ? count : 0;
}

static bool resolve(Cpu *c, const Picker *p, uint64_t item, QuickstepGem *out) {
    if (!live(p)) return false;
    QuickstepGem gems[QUICKSTEP_GEM_CAPACITY];
    size_t n = quickstep_slot_list(c, p->context, p->character, gems, QUICKSTEP_GEM_CAPACITY);
    if (n > QUICKSTEP_GEM_CAPACITY || !live(p)) return false;
    for (size_t i = 0; i < n; ++i)
        if (gems[i].availability == QUICKSTEP_FREE && identity(item, &gems[i])) {
            *out = gems[i]; return true;
        }
    return false;
}
// Resolve the exact currently displayed real instance again at confirmation.
static bool choose(Cpu *c, const Picker *p, QuickstepGem *out) {
    if (!live(p)) return false;
    uint64_t count = LD64(p->seq + COUNT);
    int32_t row = (int32_t)LD32(p->list + ROW);
    if (count > QUICKSTEP_GEM_CAPACITY || row < 0 || (uint64_t)row >= count) return false;
    return resolve(c, p, LD64(p->seq + VECTOR + (uint64_t)row * 8), out);
}
static bool displayed(const Picker *p, uint64_t item) {
    if (!live(p)) return false;
    uint64_t count = LD64(p->seq + COUNT);
    if (count > QUICKSTEP_GEM_CAPACITY) return false;
    for (uint64_t i = 0; i < count; ++i)
        if (LD64(p->seq + VECTOR + i * 8) == item) return true;
    return false;
}
void quickstep_native_picker_row_style(Cpu *c) {
    Picker p = snapshot();
    if (!p.active || p.restoring || c->x[20] != p.seq || !range(c->x[0], 0x20)
        || (c->sp & 15) || !range(c->sp, 0x170) || LD64(c->sp + 8) != p.context
        || LD32(c->sp + 0x28) != 3) return;
    // Native316550 has already bound the actual UID's genuine icon/text through
    // 232960. Its remaining disabled flag combines a type3 check with the
    // unrelated ordinary boots/socket2FA520 gate. Change only that one flag for
    // an eligible real instance still present in this exact filtered bonus list.
    uint64_t item = LD64(c->sp + 0x138);
    QuickstepGem gem;
    if (!displayed(&p, item) || !resolve(c, &p, item, &gem)
        || !displayed(&p, item) || LD64(c->sp + 0x138) != item
        || LD64(c->sp + 8) != p.context || LD32(c->sp + 0x28) != 3
        || !identity(item, &gem)) return;
    c->x[1] = 0;
}
void quickstep_native_picker_menu_gate(Cpu *c) {
    Picker p=snapshot(); QuickstepGem gem;
    if(!p.active || c->x[19]!=p.seq || c->x[20]!=p.list
        || !(c->x[0]&1) || (c->sp&15) || !range(c->sp,0x450)
        || !live(&p) || LD32(p.seq+PANE)!=2 || LD32(p.ui+SELECTED)!=13
        || !choose(c,&p,&gem) || !live(&p)) return;
    // This is only the Plus20D callsite's ordinary socket gate. The original
    // branch, native2FA730 nonempty-list check and modal routing remain intact.
    c->x[0]=0;
}
void quickstep_native_picker_menu_item(Cpu *c) {
    uint64_t seq = c->x[19], ctx, gen; uint32_t ch;
    if (!g_settings.quickstep_slot || !range(seq, 0x8a20)
        || (c->sp & 15) || !range(c->sp, 0x450)
        || !quickstep_native_bonus_context(seq, &ctx, &ch, &gen)) return;
    uint64_t ui = LD64(seq + UI);
    uint32_t pane=LD32(seq+PANE);
    if (!range(ui, 0x70) || (pane!=1 && pane!=2)
        || (LD32(ui + SELECTED) != 5 && LD32(ui + SELECTED) != 13)) return;
    uint32_t action = LD32(c->sp + 0x2d0);
    if (action > 6) return;
    bool enabled = false;
    if (action == 0) {
        QuickstepGem gem;
        enabled = quickstep_slot_get(c, ctx, ch, &gem);
    }
    // A native getter may yield through a character/load/teardown transition.
    // Never update a menu record belonging to the replacement equipment owner.
    if (!same_focus(seq, ctx, ch, gen) || LD64(seq + UI) != ui
        || LD32(seq + PANE) != pane
        || (LD32(ui + SELECTED) != 5 && LD32(ui + SELECTED) != 13)
        || LD32(c->sp + 0x2d0) != action) return;
    // The native Remove action is about the character-owned association. Native
    // bulk removal and gear/socket swap retain their labels but stay disabled.
    ST8(c->sp + 0x2d4, enabled);
    pthread_mutex_lock(&picker_lock);
    if(!context_menu.published && !context_menu.closing)
        context_menu=(ContextMenu){.builder=c,.frame=c->sp,.seq=seq,.ui=ui,
            .panel=LD64(seq+0x8440),.context=ctx,.generation=gen,.character=ch,
            .serial=++serial,.building=true};
    pthread_mutex_unlock(&picker_lock);
}
void quickstep_native_picker_menu_opened(Cpu *c) {
    ContextMenu m=menu_snapshot();
    bool attempted=m.building && m.builder==c && m.frame==c->sp && m.seq==c->x[19];
    // This hook runs for ordinary native menus too. A new native instance must
    // clear old callback provenance, including an allocator-reused address.
    if(!attempted) {
        if(m.published && quickstep_native_picker_context_menu_active(m.seq)) return;
        pthread_mutex_lock(&picker_lock);context_menu=(ContextMenu){0};
        pthread_mutex_unlock(&picker_lock);return;
    }
    m.building=false;m.dialog=c->x[21];
    if(range(m.dialog,0x750)) {
        m.fallback_callback=m.dialog+0x188;
        uint64_t reported=LD64(m.dialog+0x180);
        if(range(reported,0x10) && LD64(reported+8)==m.seq) m.callback=reported;
    }
    m.published=same_focus(m.seq,m.context,m.character,m.generation)
        && menu_owner(&m) && menu_dialog(&m)
        && LD64(m.seq+0x87a8)==m.seq+0x87b0
        && LD64(m.seq+0x87b0)==0x7000b4da00ull;
    pthread_mutex_lock(&picker_lock);
    if(context_menu.serial==m.serial) context_menu=m;
    pthread_mutex_unlock(&picker_lock);
}
static void refresh(Cpu *c, const Picker *p) {
    if (!live(p)) return;
    call(c, 0x70002ff870ull, p->seq, 0, 0, 0);
    if (!live(p)) return;
    call(c, 0x70002fea30ull, p->seq, 0, 0, 0);
    if (!live(p)) return;
    call(c, 0x7000240cf0ull, p->list + 0x58, 0, 0, 0);
}
static void action_result(const Picker *p, const char *route, int32_t row,
                          const QuickstepGem *gem, const char *result) {
    pthread_mutex_lock(&picker_lock);
    bool emit = diagnostic_actions < 64;
    if (emit) ++diagnostic_actions;
    pthread_mutex_unlock(&picker_lock);
    if (emit)
        fprintf(stderr, "[quickstep-picker] confirm route=%s seq=%llx character=%u row=%d uid=%x rank=%u result=%s\n",
                route, (unsigned long long)p->seq, p->character, row,
                gem ? gem->uid : 0, gem ? gem->rank : 0, result);
}
static bool begin_accept(const Picker *p) {
    pthread_mutex_lock(&picker_lock);
    bool start = picker.active && picker.serial == p->serial
        && !picker.opening && !picker.accepting && !picker.closing && !picker.restoring;
    if (start) picker.accepting = true;
    pthread_mutex_unlock(&picker_lock);
    return start;
}
static void end_accept(const Picker *p) {
    pthread_mutex_lock(&picker_lock);
    if (picker.serial == p->serial) picker.accepting = false;
    pthread_mutex_unlock(&picker_lock);
}
static bool close_picker(Cpu *c, const Picker *p, bool equipped) {
    pthread_mutex_lock(&picker_lock);
    bool start = picker.active && picker.serial == p->serial && !picker.closing && !picker.restoring;
    if (start) picker.closing = true;
    pthread_mutex_unlock(&picker_lock);
    if (!start) return false;
    bool closed = false;
    if (!live(p)) goto done;
    if (equipped) {
        // Original gem-confirm302A84..302A8C uses this genuine native sound,
        // volume1, without entering its ordinary mutation/rebinding paths.
        // Only the checked bonus assignment above authorizes this feedback.
        Cpu saved = *c;
        const float volume = 1.f;
        c->v[0] = 0; memcpy(&c->v[0], &volume, sizeof volume);
        call(c, 0x70001b75f0ull, p->context, 0x14, 0, 0);
        *c = saved;
        if (!live(p)) goto done;
    }
    call(c, 0x700020a610ull, p->ui, 0, 0, 0);
    if (!live(p)) goto done;
    // This native close only resets presentation and queues list/details close.
    // The bonus guard must span its original2FF5FC close-tail hook; never call
    // the ordinary2FE000 commit or2FC5D0 equipment rebind to obtain a close.
    CloseFrame frame = {.cpu=c,.sequence=p->seq,.serial=p->serial};
    pthread_mutex_lock(&picker_lock);
    frame.epoch=close_epoch;frame.token=++close_token;frame.next=close_frames;close_frames=&frame;
    pthread_mutex_unlock(&picker_lock);
    call(c, 0x70002ff2e0ull, p->seq, 0, 0, 0);
    pthread_mutex_lock(&picker_lock);
    if(frame.epoch==close_epoch)
        for(CloseFrame **at=&close_frames;*at;at=&(*at)->next)
            if(*at==&frame&&(*at)->token==frame.token){*at=frame.next;break;}
    pthread_mutex_unlock(&picker_lock);
    closed = true;
done:;
    // The native tail normally completes this lease. Reconcile only our exact
    // serial if a helper was interrupted or a focused fixture has no tail hook;
    // a replacement picker at the same Seq address must remain untouched.
    Picker now = snapshot();
    if (now.active && now.serial == p->serial) {
        Cpu saved = *c; c->x[0] = p->seq;
        quickstep_native_picker_closed(c); *c = saved;
    }
    return closed;
}
static void accept(Cpu *c, const Picker *p, const char *route) {
    QuickstepGem gem = {0}, assigned = {0};
    int32_t row = -1;
    const char *result = "owner-stale";
    if (!begin_accept(p)) return;
    if (!live(p)) goto done;
    row = (int32_t)LD32(p->list + ROW);
    if (!choose(c, p, &gem)) { result = "selection-not-eligible"; goto done; }
    if (!live(p)) goto done;
    if (!quickstep_slot_set_checked(c, p->context, p->character, gem.uid, gem.fingerprint)) {
        result = "assignment-rejected"; goto done;
    }
    if (!live(p) || !quickstep_slot_get(c, p->context, p->character, &assigned)
        || !live(p) || assigned.uid != gem.uid
        || memcmp(assigned.fingerprint, gem.fingerprint, sizeof gem.fingerprint)) {
        result = "assignment-invalidated"; goto done;
    }
    quickstep_stats_refresh(c, p->seq);
    if (!live(p)) { result = "accepted-owner-changed"; goto done; }
    result = close_picker(c, p, true) ? "accepted" : "accepted-owner-changed";
done:
    end_accept(p);
    action_result(p, route, row, &gem, result);
}
bool quickstep_native_picker_input_event(Cpu *c, uint64_t seq, uint32_t event) {
    if(event==0x20d) {
        ContextMenu m=menu_snapshot();
        if(m.building && m.seq==seq
            && !same_focus(m.seq,m.context,m.character,m.generation)) menu_end(m.serial);
    }
    Picker p = snapshot();
    if (!p.active || p.seq != seq || event != 0x201) return false;
    // Native Confirm checks the ordinary parent boots before dispatch302640.
    // A bonus association has no ordinary socket, so own this one action here.
    accept(c, &p, "native201");
    return true;
}
bool quickstep_native_picker_confirm(Cpu *c) {
    if (!range(c->x[0], 0x20)) return false;
    uint64_t seq = LD64(c->x[0] + 0x18);
    if (!owned(seq)) return false;
    Picker p = snapshot();
    if (p.active && p.seq == seq) accept(c, &p, "callback302640");
    c->x[0] = 1;
    return true;
}
static void clear(Cpu *c, uint64_t seq) {
    uint64_t context, generation; uint32_t character; QuickstepGem gem;
    if (quickstep_native_bonus_context(seq, &context, &character, &generation)
        && quickstep_slot_get(c, context, character, &gem)
        && same_focus(seq, context, character, generation)
        && quickstep_slot_clear_checked(c, context, character, gem.uid, gem.fingerprint)) {
        if (!same_focus(seq, context, character, generation)) return;
        quickstep_stats_refresh(c, seq);
        Picker p = snapshot();
        if (p.active && p.seq == seq) refresh(c, &p);
    }
}
bool quickstep_native_picker_remove(Cpu *c) {
    if (!range(c->x[0], 0x20)) return false;
    uint64_t seq = LD64(c->x[0] + 0x18);
    if (!owned(seq)) return false;
    if (range(c->x[1], 0x10) && LD32(c->x[1] + 8) == 2
        && LD32(c->x[1] + 0xc) == 13) clear(c, seq);
    c->x[0] = 1;
    return true;
}
bool quickstep_native_picker_uid(Cpu *c) {
    uint64_t seq = c->x[0];
    if (!owned(seq)) return false;
    Picker p = snapshot(); QuickstepGem gem;
    uint32_t uid = 0;
    if (p.active && p.seq == seq) {
        if (choose(c, &p, &gem)) uid = gem.uid;
    } else {
        uint64_t context, generation; uint32_t character;
        if (quickstep_native_bonus_context(seq, &context, &character, &generation)
            && quickstep_slot_get(c, context, character, &gem)) uid = gem.uid;
    }
    c->x[0] = uid;
    return true;
}
bool quickstep_native_picker_preview(Cpu *c) { return owned(c->x[0]); }
bool quickstep_native_picker_commit(Cpu *c) { return owned(c->x[0]); }
bool quickstep_native_picker_eligible(Cpu *c) {
    uint64_t seq = c->x[0], ctx, gen; uint32_t ch; QuickstepGem gem;
    if ((uint32_t)c->x[2] != 13 || !owned(seq)) return false;
    bool focus = quickstep_native_bonus_context(seq, &ctx, &ch, &gen);
    if (focus && (uint32_t)c->x[1] != ch) return false;
    bool present = focus && quickstep_slot_get(c, ctx, ch, &gem)
        && same_focus(seq, ctx, ch, gen);
    c->x[0] = present;
    return true;
}
bool quickstep_native_picker_submenu(Cpu *c) {
    if (!range(c->x[0], 0x10)) return false;
    uint64_t seq = LD64(c->x[0] + 8);
    ContextMenu m=menu_snapshot();
    bool known=m.seq==seq && (m.callback==c->x[0] || m.fallback_callback==c->x[0]);
    if (!owned(seq) && !known) return false;
    // An incomplete/stale/forged popup callback is handled without touching
    // either a bonus or any ordinary equipment. Only this typed lease mutates.
    if(!known || !quickstep_native_picker_context_menu_active(seq)
        || m.callback!=c->x[0]) return true;
    pthread_mutex_lock(&picker_lock);
    if(context_menu.serial==m.serial) context_menu.closing=true;
    pthread_mutex_unlock(&picker_lock);
    uint64_t ctx, gen; uint32_t ch;
    if (range(seq, 0x8a20) && quickstep_native_bonus_context(seq, &ctx, &ch, &gen)) {
        int32_t choice = (int32_t)c->x[1];
        uint32_t action = range(c->x[2], 0x24) ? LD32(c->x[2] + 0x20) : UINT32_MAX;
        call(c, 0x70002fa290ull, seq, LD32(seq + 0x87f8), 0, 0);
        if (same_focus(seq, ctx, ch, gen)) {
            // Bulk removal and the gear/socket swap do not operate on a bonus
            // association. Favourite/sort use separate unchanged native routes.
            if (choice != -1 && action == 0) clear(c, seq);
            if (same_focus(seq, ctx, ch, gen)) call(c, 0x7000300320ull, seq, 0, 0, 0);
        }
    }
    menu_end(m.serial);
    return true;
}

bool quickstep_native_picker_details(Cpu *c) {
    uint64_t seq = c->x[0], context, gen; uint32_t character;
    if (!owned(seq)) return false;
    if (!range(seq, 0x8a20) || !quickstep_native_bonus_context(seq, &context, &character, &gen)) return true;
    Picker p = snapshot(); QuickstepGem gem;
    uint32_t old_uid = quickstep_slot_get(c, context, character, &gem) ? gem.uid : 0;
    uint32_t new_uid = p.active && p.seq == seq && choose(c, &p, &gem) ? gem.uid : 0;
    if (!same_focus(seq, context, character, gen)) return true;
    uint64_t details = LD64(seq + 0x8448), tooltip = LD64(seq + 0x5da0);
    if (!range(details, 0x6d8) || !range(tooltip, 8) || c->sp < GUEST_LO + 0x400
        || !range(c->sp - 0x400, 0x400)) return true;
    uint64_t old_view = c->sp - 0x400, new_view = old_view + ITEM_VIEW_BYTES;
    call(c, 0x7000172c40ull, old_view, context, 0, 0);
    if (!same_focus(seq, context, character, gen)) return true;
    call(c, 0x7000172c40ull, new_view, context, 0, 0);
    if (!same_focus(seq, context, character, gen)) return true;
    if (old_uid) call(c, 0x7000172cf0ull, old_view, old_uid, 0, 0);
    if (!same_focus(seq, context, character, gen)) return true;
    if (new_uid) call(c, 0x7000172cf0ull, new_view, new_uid, 0, 0);
    if (!same_focus(seq, context, character, gen)) return true;
    bool visible = p.active && p.seq == seq && live(&p);
    ST32(details + 0x6d4, visible);
    if (visible) call(c, 0x7000237a30ull, details, new_view, old_view, 0);
    else call(c, 0x70002369c0ull, details, old_view, 0, 0);
    if (!same_focus(seq, context, character, gen)) return true;
    call(c, 0x70002369c0ull, tooltip, old_view, 0, 0);
    return true;
}
bool quickstep_native_picker_open(Cpu *c, uint64_t seq) {
    uint64_t ctx, gen; uint32_t ch;
    if (!g_settings.quickstep_slot || !range(seq, 0x8a20)
        || !quickstep_native_bonus_context(seq, &ctx, &ch, &gen)) return false;
    uint64_t ui = LD64(seq + UI), list = LD64(seq + LIST);
    if (!range(ui, 0x70) || !range(list, 0xa00) || !range(LD64(seq + 0x5e20), 8)) return false;
    pthread_mutex_lock(&picker_lock);
    if (picker.active) { pthread_mutex_unlock(&picker_lock); return false; }
    picker = (Picker){.seq=seq,.context=ctx,.generation=gen,.ui=ui,.list=list,
        .panel=LD64(seq+0x8440),
        .serial=++serial,.character=ch,.selected=LD32(ui+SELECTED),
        .pane=LD32(seq+PANE),.ui_pane=LD32(ui+UI_PANE),.active=true,.opening=true};
    Picker p = picker;
    pthread_mutex_unlock(&picker_lock);
    // Establish the independent native owner used for close-time restoration
    // before borrowing any of its selection or inventory cache fields.
    if (!picker_owner(&p)) goto interrupted;
    // Index13 is a bounded native armour-gem enumeration context, never an
    // equipment assignment. No native staged equipment array is modified.
    ST32(seq + PANE, 2); ST32(ui + UI_PANE, 2); ST32(ui + SELECTED, 13);
    call(c, 0x70002fb9b0ull, seq, 1, 0, 0);
    if (!live(&p)) goto interrupted;
    call(c, 0x700020a510ull, ui, 1, 0, 0);
    if (!live(&p)) goto interrupted;
    call(c, 0x70002fec10ull, seq, 0, 0, 0);
    if (!live(&p)) goto interrupted;
    QuickstepGem current;
    int32_t row = LD64(seq + COUNT) ? 0 : -1;
    if (quickstep_slot_get(c, ctx, ch, &current) && live(&p))
        for (uint64_t i = 0; i < LD64(seq + COUNT); ++i)
            if (identity(LD64(seq + VECTOR + i * 8), &current)) { row = (int32_t)i; break; }
    if (!live(&p)) goto interrupted;
    call(c, 0x7000240ef0ull, list + 0x58, (uint32_t)row, 0, 0);
    refresh(c, &p);
    if (!live(&p)) goto interrupted;
    pthread_mutex_lock(&picker_lock);
    bool ready = picker.active && picker.serial == p.serial && picker.opening
        && !picker.closing && !picker.restoring;
    pthread_mutex_unlock(&picker_lock);
    if (!ready) goto interrupted;
    // Original native chooser open2F9358/2F9430 uses sound3, volume1. Play
    // once only after this exact chooser is constructed and still owned. Its
    // opening phase rejects nested Confirm until the yielding sound returns.
    call(c, 0x70001b7600ull, p.context, 0, 0, 0);
    if (!live(&p)) goto interrupted;
    pthread_mutex_lock(&picker_lock);
    ready = picker.active && picker.serial == p.serial && picker.opening
        && !picker.closing && !picker.restoring;
    if (ready) picker.opening = false;
    pthread_mutex_unlock(&picker_lock);
    if (ready) return true;
interrupted:
    // A yielding helper may publish a replacement picker at this same Seq.
    // Retire only our exact serial, never forget that new owner's session.
    pthread_mutex_lock(&picker_lock);
    if (picker.active && picker.serial == p.serial) memset(&picker, 0, sizeof picker);
    pthread_mutex_unlock(&picker_lock);
    return false;
}
void quickstep_native_picker_closed(Cpu *c) {
    Picker p = snapshot();
    if (!p.active || p.seq != c->x[0]) return;
    // A yielded old native close can return after a new picker reuses Seq.
    // Its tail belongs to the caller's exact serial, not that new session.
    bool stale_tail=false;
    pthread_mutex_lock(&picker_lock);
    for (CloseFrame *f=close_frames; f; f=f->next)
        if (f->cpu==c && f->sequence==p.seq) {
            stale_tail=f->serial!=p.serial;
            break;
        }
    pthread_mutex_unlock(&picker_lock);
    if(stale_tail)return;
    // [XCDE] LINUX-044: the bonus list is a temporary native inventory lease.
    // Retire its filter before rebuilding the ordinary cached list. Keep the
    // exact serial active until that yielding helper returns: nested Confirm /
    // Cancel must not reopen, assign or close again during restoration.
    pthread_mutex_lock(&picker_lock);
    bool restoring = picker.active && picker.serial == p.serial && !picker.restoring;
    if (restoring) picker.restoring = true;
    pthread_mutex_unlock(&picker_lock);
    if (!restoring) return;
    bool owner = picker_owner(&p);
    if (owner) {
        // A character/pane callback normally cancels before its native change.
        // If a native route already moved, preserve that actual selection; an
        // old picker may restore only the temporary mode it still owns.
        if (LD32(p.ui + SELECTED) == 13 && LD32(p.ui + UI_PANE) == 2
                && LD32(p.seq + PANE) == 2) {
            ST32(p.ui + SELECTED, p.selected); ST32(p.ui + UI_PANE, p.ui_pane);
            ST32(p.seq + PANE, p.pane);
        }
        // Native Confirm reuses this vector; merely resetting the input
        // controller on Left does not run native navigation's list rebuild.
        // Re-enumerate live inventory rather than restore copied/stale item
        // pointers or fabricate ordinary socket/equipment ownership.
        if (LD32(p.ui + SELECTED) < 20)
            call(c, 0x70002fb9b0ull, p.seq, 1, 0, 0);
    }
    pthread_mutex_lock(&picker_lock);
    bool cleared = picker.active && picker.serial == p.serial;
    if (cleared) memset(&picker, 0, sizeof picker);
    pthread_mutex_unlock(&picker_lock);
    if (cleared && owner && picker_owner(&p)) quickstep_native_bonus_picker_closed(p.seq);
}
void quickstep_native_picker_cancel(Cpu *c, uint64_t seq) {
    Picker p = snapshot();
    if (!p.active || p.seq != seq || p.closing || p.restoring) return;
    (void)close_picker(c, &p, false);
}
void quickstep_native_picker_cancel_callback(Cpu *c) {
    if (!range(c->x[0], 0x20)) return;
    quickstep_native_picker_cancel(c, LD64(c->x[0] + 0x18));
}
void quickstep_native_picker_forget(uint64_t seq) {
    pthread_mutex_lock(&picker_lock);
    if (picker.active && picker.seq == seq) memset(&picker, 0, sizeof picker);
    if(context_menu.seq==seq)
        context_menu.building=context_menu.published=context_menu.closing=false;
    pthread_mutex_unlock(&picker_lock);
}
RT_ON_RESET(quickstep_native_picker_reset) {
    pthread_mutex_lock(&picker_lock);picker=(Picker){0};context_menu=(ContextMenu){0};
    close_frames=NULL;++close_epoch;
    ++serial;diagnostic_actions=0;pthread_mutex_unlock(&picker_lock);
}
