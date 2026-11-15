// [XCDE] LINUX-040 r3: show the real bonus gem through native summary rows.
// The original eight-record summaries, equipment previews and inventory remain
// untouched. A separate ninth row reaches the existing native renderer, which
// owns the localized name, description, percentage formatting and row geometry.
#include "../hle.h"
#include "../settings.h"
#include "quickstep_native_ui.h"
#include "quickstep_picker.h"
#include "quickstep_slot.h"
#include "quickstep_stats.h"
#include "quickstep_stats_ui.h"

enum { CAPACITY = 32, SUMMARY_BYTES = 0x178, ROWS = 0x30, COUNT = 0x170,
       ROW_BYTES = 0x28, FRAME_BYTES = 0x2b0, LOCAL_ROW = 0x150,
       MEMORY_BYTES = 0x80, INPUT = 0x10, NAME = 0x50, TAIL = 0x70 };
static const uint64_t canary = 0x91c7ea30d46f285bull;
typedef struct {
    Cpu *cpu;
    uint64_t frame, panel, sequence, context, owner_generation, backend_epoch;
    uint64_t baseline_source, current_source, serial, memory;
    uint32_t character, count, baseline_value, owned_rows;
    int quickstep_index;
    QuickstepGem gem;
    uint8_t baseline[SUMMARY_BYTES], current[SUMMARY_BYTES], row[ROW_BYTES];
    bool busy, ready;
} Summary;
static pthread_mutex_t summary_lock = PTHREAD_MUTEX_INITIALIZER;
static Summary summaries[CAPACITY];
static uint64_t serial;

static bool range(uint64_t p, size_t n) {
    return p >= GUEST_LO && p < GUEST_HI && n <= GUEST_HI - p;
}
static uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t u64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static bool snapshot(Cpu *c, Summary *out) {
    bool found = false;
    pthread_mutex_lock(&summary_lock);
    for (unsigned i = 0; i < CAPACITY; ++i)
        if (summaries[i].busy && summaries[i].cpu == c && summaries[i].frame == c->sp) {
            *out = summaries[i]; found = true; break;
        }
    pthread_mutex_unlock(&summary_lock);
    return found;
}
static bool owner_live(const Summary *s,bool check_rows) {
    uint64_t sequence, context, generation, epoch = s->backend_epoch;
    if (!g_settings.quickstep_slot
        || !quickstep_native_stats_context(s->panel, s->character, &sequence, &context, &generation)
        || sequence != s->sequence || context != s->context || generation != s->owner_generation
        || !quickstep_slot_reference_matches(context, s->character, &s->gem, &epoch)
        || !range(s->panel,0x1ac1) || (check_rows && LD64(s->panel+0x1948)!=s->owned_rows)
        || !range(s->baseline_source, SUMMARY_BYTES) || !range(s->current_source, SUMMARY_BYTES)
        || memcmp((void *)(uintptr_t)s->baseline_source, s->baseline, SUMMARY_BYTES)
        || memcmp((void *)(uintptr_t)s->current_source, s->current, SUMMARY_BYTES)) return false;
    return true;
}
static bool live(const Summary *s) {
    if (!owner_live(s,true)) return false;
    pthread_mutex_lock(&summary_lock);
    bool same = false;
    for (unsigned i = 0; i < CAPACITY; ++i)
        if (summaries[i].busy && summaries[i].cpu == s->cpu
            && summaries[i].frame == s->frame && summaries[i].serial == s->serial) same = true;
    pthread_mutex_unlock(&summary_lock);
    return same;
}
static void discard(const Summary *s) {
    pthread_mutex_lock(&summary_lock);
    for (unsigned i = 0; i < CAPACITY; ++i)
        if (summaries[i].busy && summaries[i].serial == s->serial
            && summaries[i].cpu == s->cpu && summaries[i].frame == s->frame) {
            summaries[i].busy = false; summaries[i].ready = false;
        }
    pthread_mutex_unlock(&summary_lock);
}
typedef struct { uint64_t first, second; } Pair;
static Pair call(Cpu *c, uint64_t fn, uint64_t a, uint64_t b, uint64_t d,
                 uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
    Cpu saved = *c;
    uint64_t fpcr = rt_get_fpcr(), fpsr = rt_get_fpsr();
    if ((c->sp & 15) || c->sp < GUEST_LO + 0x2000 || !range(c->sp - 0x2000, 0x2000))
        FATAL("[quickstep-stats] invalid native helper stack\n");
    c->sp -= 0x2000;
    c->x[0]=a; c->x[1]=b; c->x[2]=d; c->x[3]=e;
    c->x[4]=f; c->x[5]=g; c->x[6]=h;
    guest_call(c, fn);
    Pair result = {c->x[0], c->x[1]};
    *c = saved; rt_set_fpcr(fpcr); rt_set_fpsr(fpsr);
    return result;
}
static int effect_index(const uint8_t *summary, uint32_t *count) {
    uint64_t n = u64(summary + COUNT);
    if (n > 8) return -2;
    *count = (uint32_t)n;
    int found = -1;
    for (uint32_t i = 0; i < n; ++i)
        if (u16(summary + ROWS + i * ROW_BYTES) == 77) {
            if (found >= 0) return -2;
            found = (int)i;
        }
    return found;
}
static uint32_t augmented(uint32_t value, uint32_t extra) {
    return value >= 25 || extra >= 25 - value ? 25 : value + extra;
}
static Pair native_name(Cpu *c,const Summary *s) {
    // Use the same skill table/message mapping as native18D3B0. Direct62310
    // performs an uncached field lookup: borrowing another call site's native
    // global cache index would alias an unrelated cached field.
    Pair none={0,0};
    Pair cache=call(c,0x70000ae8e0ull,s->context,0,0,0,0,0,0);
    if (!live(s) || !range(cache.first,0x1078)) return none;
    Pair table=call(c,0x700006b270ull,cache.first,0x14,0,0,0,0,0);
    if (!live(s) || !range(table.first,0x18)) return none;
    memcpy((void *)(uintptr_t)(s->memory+NAME),"name",5);
    Pair field=call(c,0x7000062310ull,table.first,s->memory+NAME,0,0,0,0,0);
    if (!live(s) || !range(field.first,4)) return none;
    Pair data=call(c,0x7000069320ull,LD64(table.first+8),LD32(table.first+0x10),0,0,0,0,0);
    if (!live(s) || !range(data.first,4)) return none;
    Pair message=call(c,0x7000510630ull,data.first,field.first,77,2,0,0,0);
    if (!live(s) || !message.first || message.first>UINT16_MAX) return none;
    Pair result=call(c,0x7000062620ull,table.first,message.first,0,0,0,0,0);
    return live(s) ? result : none;
}
void quickstep_stats_begin(Cpu *c) {
    Summary s = {0};
    s.cpu=c; s.panel=c->x[0]; s.character=(uint32_t)c->x[1];
    s.baseline_source=c->x[2]; s.current_source=c->x[3];
    // Restore ordinary geometry before baseline/OFF/ordinary-preview paths.
    // The UI capsule validates its own structural owner even when disabled.
    quickstep_stats_ui_layout(c,s.panel,false);
    if (!g_settings.quickstep_slot || (c->sp & 15) || c->sp < GUEST_LO + FRAME_BYTES
        || !range(c->sp - FRAME_BYTES, FRAME_BYTES)
        || !quickstep_native_stats_context(s.panel, s.character, &s.sequence,
                                            &s.context, &s.owner_generation)
        || !range(s.sequence, 0x8a18) || !range(s.panel, 0x1ac1)
        || !range(s.baseline_source, SUMMARY_BYTES) || !range(s.current_source, SUMMARY_BYTES)) return;
    // Ordinary item/gem previews retain their native presentation unchanged.
    // The bonus picker requests the zero-selected-UID baseline explicitly.
    uint64_t list=LD64(s.sequence+0x4c10);
    if (!range(list, 0x20)
        || (LD8(list+0x19) && !quickstep_native_picker_active(s.sequence))) return;
    memcpy(s.baseline, (void *)(uintptr_t)s.baseline_source, SUMMARY_BYTES);
    memcpy(s.current, (void *)(uintptr_t)s.current_source, SUMMARY_BYTES);
    uint32_t base_count;
    int base=effect_index(s.baseline, &base_count);
    s.quickstep_index=effect_index(s.current, &s.count);
    s.owned_rows=(uint32_t)LD64(s.panel+0x1948);
    if (base < -1 || s.quickstep_index < -1
        || !quickstep_slot_get(c,s.context,s.character,&s.gem)
        || !quickstep_slot_reference_matches(s.context,s.character,&s.gem,&s.backend_epoch)
        || !owner_live(&s,true)) return;
    bool ninth=s.count==8 && s.quickstep_index<0;
    if (ninth && !quickstep_stats_ui_ensure(c,s.panel,s.character)) return;
    if (!owner_live(&s,false)) return;
    // The renderer's arithmetic loop bound does not establish widget ownership.
    // A full eighth summary needs an actual independently owned twentieth row.
    uint64_t owned_rows=LD64(s.panel+0x1948);
    if (owned_rows>32 || owned_rows < 11+s.count+(s.quickstep_index<0)) return;
    s.owned_rows=(uint32_t)owned_rows;
    if (!owner_live(&s,true)) return;
    s.frame=c->sp-FRAME_BYTES;
    s.baseline_value=augmented(base < 0 ? 0 : u32(s.baseline+ROWS+base*ROW_BYTES+0x18),s.gem.value);
    pthread_mutex_lock(&summary_lock);
    Summary *slot=NULL;
    for (unsigned i=0;i<CAPACITY;++i) {
        if (summaries[i].busy && summaries[i].cpu==c && summaries[i].frame==s.frame)
            FATAL("[quickstep-stats] duplicate native summary frame\n");
        if (!slot && !summaries[i].busy) slot=&summaries[i];
    }
    if (!slot) FATAL("[quickstep-stats] native summary capacity exhausted\n");
    s.memory=slot->memory;
    if (!s.memory) s.memory=(uint64_t)(uintptr_t)hle_alloc(MEMORY_BYTES);
    if (!range(s.memory,MEMORY_BYTES)) FATAL("[quickstep-stats] invalid native summary scratch\n");
    s.serial=++serial; s.busy=true; *slot=s;
    pthread_mutex_unlock(&summary_lock);
    memset((void *)(uintptr_t)s.memory,0,MEMORY_BYTES);
    ST64(s.memory,canary); ST64(s.memory+TAIL,canary);
    // get() resolves native context and may yield. Do not even decode an item
    // through a session which changed while that initial lookup was running.
    if (!live(&s)) { discard(&s); return; }
    uint64_t input=s.memory+INPUT;
    Pair decoded=call(c,0x70000a8290ull,s.context,s.gem.uid,0,input+4,input,0,0);
    if (!(decoded.first&1) || !live(&s) || LD16(input)!=77
        || LD16(input+0xa)!=3 || LD16(input+0xe)!=s.gem.value || LD16(input+0x16)!=25) {
        discard(&s); return;
    }
    // The native skill table supplies its localized name/message pair. No
    // translated text, message ID or font asset is embedded here.
    Pair name=native_name(c,&s);
    if (!live(&s) || !range(name.first,0x18) || !(name.second&0xffff)
        || LD64(s.memory)!=canary || LD64(s.memory+TAIL)!=canary) { discard(&s); return; }
    put16(s.row,77); put64(s.row+8,name.first); put16(s.row+0x10,(uint16_t)name.second);
    put32(s.row+0x18,s.gem.value); put32(s.row+0x1c,25);
    s.row[0x20]=3; s.row[0x21]=s.gem.rank;
    if (ninth) {
        quickstep_stats_ui_layout(c,s.panel,true);
        if (!live(&s)) { discard(&s); return; }
    }
    s.ready=true;
    pthread_mutex_lock(&summary_lock);
    for (unsigned i=0;i<CAPACITY;++i)
        if (summaries[i].busy && summaries[i].serial==s.serial) summaries[i]=s;
    pthread_mutex_unlock(&summary_lock);
}
void quickstep_stats_count(Cpu *c) {
    Summary s;
    if (snapshot(c,&s) && s.ready && live(&s) && c->x[19]==s.panel
        && c->x[21]==s.current_source && c->x[22]==s.baseline_source)
        c->x[8]=s.count+(s.quickstep_index<0);
}
int quickstep_stats_row(Cpu *c) {
    Summary s;
    if (!snapshot(c,&s)) return 0;
    if (!s.ready || !live(&s) || c->x[19]!=s.panel || c->x[21]!=s.current_source
        || c->x[22]!=s.baseline_source
        || !range(c->sp+LOCAL_ROW,ROW_BYTES)) return -1;
    uint64_t i=c->x[20];
    if (i>=s.count+(s.quickstep_index<0)) return -1;
    uint8_t row[ROW_BYTES];
    if (i==s.count) memcpy(row,s.row,ROW_BYTES);
    else {
        memcpy(row,s.current+ROWS+i*ROW_BYTES,ROW_BYTES);
        if ((int)i==s.quickstep_index) {
            put32(row+0x18,augmented(u32(row+0x18),s.gem.value));
            put32(row+0x1c,25);
            if (row[0x21]<s.gem.rank) row[0x21]=s.gem.rank;
        }
    }
    memcpy((void *)(uintptr_t)(c->sp+LOCAL_ROW),row,ROW_BYTES);
    c->x[8]=c->sp+LOCAL_ROW; c->x[9]=u64(row+0x20);
    memcpy(&c->v[0],row,16); memcpy(&c->v[1],row+16,16);
    return 1;
}
void quickstep_stats_compare(Cpu *c) {
    Summary s;
    if (!snapshot(c,&s) || !s.ready || !live(&s) || c->x[19]!=s.panel
        || c->x[21]!=s.current_source || c->x[22]!=s.baseline_source
        || !range(c->sp+LOCAL_ROW,ROW_BYTES)
        || LD16(c->sp+LOCAL_ROW)!=77) return;
    c->x[24]=(uint32_t)(LD32(c->sp+LOCAL_ROW+0x18)-s.baseline_value);
    c->x[26]=0; // Both actual and baseline own this exact persistent bonus.
}
void quickstep_stats_end(Cpu *c) {
    Summary s;
    if (snapshot(c,&s)) {
        if (LD64(s.memory)!=canary || LD64(s.memory+TAIL)!=canary)
            FATAL("[quickstep-stats] native summary scratch overrun\n");
        discard(&s);
    }
}
void quickstep_stats_refresh(Cpu *c,uint64_t sequence) {
    uint64_t seq,context,generation;
    if (!range(sequence,0x8a18)) return;
    uint64_t list=LD64(sequence+0x4c10);
    if(!range(list,0x20)||(LD8(list+0x19)&&!quickstep_native_picker_active(sequence)))return;
    uint32_t ch=LD32(sequence+0x18b8);
    uint64_t panel=LD64(sequence+0x8440);
    if (!quickstep_native_stats_context(panel,ch,&seq,&context,&generation) || seq!=sequence) return;
    (void)generation;
    call(c,0x700023b150ull,context,ch,0,0,panel,0,0);
}
RT_ON_RESET(quickstep_stats_reset) {
    // Runtime reset runs after all guest threads have stopped.
    memset(summaries,0,sizeof summaries); serial=0;
}
