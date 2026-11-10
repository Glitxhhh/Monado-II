// [XCDE] LINUX-040: authored native control adapter. All art, icon frames,
// fonts, focus animation, item descriptions and gem list widgets remain loaded
// native game resources. No original twenty-entry array or live node is reused.
#include "../hle.h"
#include "../settings.h"
#include "quickstep_native.h"
#include "quickstep_slot.h"
#include "quickstep_picker.h"
#include "quickstep_ui_memory.h"
#include <float.h>
#include <stdatomic.h>

enum { SEQ_UI = 0x18d8, SEQ_UI_PTR = 0x1c48, SEQ_CHAR = 0x18b8,
       SEQ_BYTES = 0x8a18, UI_BYTES = 0x370, MEMORY_BYTES = 0xa00, ITEM_BYTES = 0x140,
       ITEM = 0x400, RESULT = 0x580, WRAP = 0x5a0, COLOR = 0x5c0, PATH = 0x600,
       LOCAL = 0x800, MATRIX = 0x840, BOUNDS = 0x860,
       MAX_NODES = 512, MAX_COMPONENTS = 2048 };
_Static_assert(UI_BYTES <= ITEM && ITEM + ITEM_BYTES <= RESULT, "native UI/item separation");
_Static_assert(RESULT + 16 <= WRAP && WRAP + 16 <= PATH && PATH + 0x180 <= MEMORY_BYTES,
               "native scratch separation");
static const char slot_path[] = "Equip_mc/EquipAnim_mc/Jem_08_mc";
static const char icon_path[] = "Equip_mc/EquipAnim_mc/Jem_08_mc/rect_icon_Jem_mc";
static const char badge_path[] = "Equip_mc/EquipAnim_mc/Jem_08_mc/rect_item_jem_slot_mc";
static const char under_path[] = "Equip_mc/EquipAnim_mc/Jem_08_mc/jem_under_mc";
static const uint64_t ui_vtable = 0x7000b460d8ull;
static const uint64_t root_vtable = 0x7000b66c18ull;

typedef struct {
    uint64_t sequence, ui, context, root, lower, slot, icon, badge, backing, under, memory, generation;
    uint64_t frame, outline;
    uint64_t last_query, reveal_start;
    float target_left, center_x, center_y, projection_x, projection_y;
    const char *failure_stage;
    uint32_t failure_detail;
    uint32_t character, uid;
    uint8_t fingerprint[32];
    bool focus, visible, busy, failed, dirty;
} NativeSlot;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static NativeSlot state;
static uint64_t serial;
// Global draw hooks must reject other controls without entering this mutex.
static _Atomic uint64_t filtered_row;
static uint64_t window_resource;
#ifndef QUICKSTEP_NATIVE_RESOURCE_CRC
#define QUICKSTEP_NATIVE_RESOURCE_CRC 0xddd165e2u
#endif

static bool range(uint64_t at, size_t bytes) {
    return at >= GUEST_LO && at < GUEST_HI && bytes <= GUEST_HI - at;
}
static bool fail(NativeSlot *s, const char *stage, uint32_t detail) {
    s->failure_stage = stage; s->failure_detail = detail; return false;
}
static void report_failure(const NativeSlot *s) {
    fprintf(stderr, "[quickstep-native] control unavailable stage=%s detail=%u; ordinary equipment unchanged\n",
            s->failure_stage ? s->failure_stage : "unknown", s->failure_detail);
}
static bool character_supported(uint32_t ch) {
    return (ch >= 1 && ch <= 8) || ch == 14 || ch == 15;
}
static bool identity(const NativeSlot *s) {
    return s->sequence && range(s->sequence, SEQ_BYTES) && s->ui == s->sequence + SEQ_UI
        && range(s->ui, UI_BYTES) && LD64(s->sequence + SEQ_UI_PTR) == s->ui
        && LD64(s->ui) == ui_vtable && LD64(s->ui + 0x10) == s->context
        && LD32(s->sequence + SEQ_CHAR) == s->character;
}
static bool current(const NativeSlot *s) {
    pthread_mutex_lock(&state_lock);
    bool live = state.generation == s->generation && state.sequence == s->sequence;
    pthread_mutex_unlock(&state_lock);
    return live && identity(s);
}

// Native helpers can yield. Preserve every register, FP state, TLS and exclusive
// monitor and never hold the ownership mutex while executing guest code.
static uint64_t call(Cpu *c, uint64_t fn, uint64_t a, uint64_t b,
                     uint64_t d, uint64_t e, uint64_t f, uint64_t result,
                     const float *scalars, uint32_t *float_result) {
    Cpu saved = *c;
    uint64_t fpcr = rt_get_fpcr(), fpsr = rt_get_fpsr();
    if ((c->sp & 15) || c->sp < GUEST_LO + 0x2000 || !range(c->sp - 0x2000, 0x2000))
        FATAL("[quickstep-native] invalid native helper stack\n");
    c->sp -= 0x2000;
    c->x[0] = a; c->x[1] = b; c->x[2] = d; c->x[3] = e; c->x[4] = f;
    c->x[8] = result;
    if (scalars) {
        c->v[0] = c->v[1] = 0;
        memcpy(&c->v[0], &scalars[0], 4); memcpy(&c->v[1], &scalars[1], 4);
    }
    guest_call(c, fn);
    uint64_t value = c->x[0];
    if (float_result) memcpy(float_result, &c->v[0], 4);
    *c = saved; rt_set_fpcr(fpcr); rt_set_fpsr(fpsr);
    return value;
}
static uint64_t plain(Cpu *c, uint64_t fn, uint64_t a, uint64_t b, uint64_t d,
                      uint64_t e, uint64_t f) {
    return call(c, fn, a, b, d, e, f, 0, NULL, NULL);
}
static void visible(Cpu *c, uint64_t node, bool show) {
    if (range(node, 0x280)) plain(c, 0x700074ea30ull, node, show, 0, 0, 0);
}
static uint64_t find(Cpu *c, const NativeSlot *s, const char *path, bool component) {
    size_t n = strlen(path) + 1;
    if (n > 0x180) return 0;
    memcpy((void *)(uintptr_t)(s->memory + PATH), path, n);
    return plain(c, component ? 0x700057a770ull : 0x700057aeb0ull,
                 s->root, s->memory + PATH, 0, 0, 0);
}
static void play(Cpu *c, const NativeSlot *s, uint64_t node, const char *label) {
    if (!range(node, 0x288) || !range(LD64(node + 0x280), 0x24)) return;
    ST64(s->memory + WRAP, node);
    memcpy((void *)(uintptr_t)(s->memory + PATH), label, strlen(label) + 1);
    plain(c, 0x7000749c90ull, s->memory + WRAP, s->memory + PATH, 0, 0, 0);
}
static void ordinary_focus(Cpu *c, const NativeSlot *s, bool restore) {
    if (!current(s)) return;
    uint32_t selected = LD32(s->ui + 0x24);
    if (selected >= 20) return;
    if (restore) {
        // Native selection setter reconstructs the original row controller and
        // callback without changing its index or any equipment/inventory data.
        plain(c, 0x7000208fe0ull, s->ui, selected, 0, 0, 0);
        return;
    }
    plain(c, 0x70001b31c0ull, s->ui + 0x30, 0, 0, 0, 0);
    if (!current(s)) return;
    const char *path = selected == 5 ? "Equip_mc/EquipAnim_mc/Weapon_06_mc" : slot_path;
    memcpy((void *)(uintptr_t)(s->memory + PATH), path, strlen(path) + 1);
    uint64_t original = LD64(s->ui + 8);
    uint64_t row = plain(c, 0x700057aeb0ull, original, s->memory + PATH, 0, 0, 0);
    if (current(s)) play(c, s, row, "off");
}
static void transform(Cpu *c, uint64_t node, bool scale, float x, float y) {
    const float values[] = { x, y };
    call(c, scale ? 0x700074e750ull : 0x700074e6a0ull,
         node, 0, 0, 0, 0, 0, values, NULL);
}
static void color(Cpu *c, const NativeSlot *s, uint64_t node,
                  float r, float g, float b, bool offset) {
    float rgba[] = {r, g, b, offset ? 0.f : 1.f};
    memcpy((void *)(uintptr_t)(s->memory + COLOR), rgba, sizeof rgba);
    ST64(s->memory + WRAP, node);
    plain(c, offset ? 0x7000749f20ull : 0x7000749f10ull,
          s->memory + WRAP, s->memory + COLOR, 0, 0, 0);
}
static void retire_frame(Cpu *c, uint64_t node) {
    if (!range(node, 0x380) || LD64(node) != 0x7000b72bb8ull) return;
    visible(c, node, false);
    plain(c, 0x700074d290ull, node, 0, 0, 0, 0);
}
static bool load_window(NativeSlot *s) {
    if (window_resource) return true;
    char name[4096];
    if (!g_romfs_dir || snprintf(name, sizeof name, "%s/menu/image/common_window_white_c.wilay", g_romfs_dir)
            >= (int)sizeof name) return fail(s, "window-path", 0);
    FILE *file = fopen(name, "rb");
    if (!file) return fail(s, "window-open", 0);
    unsigned char *bytes = malloc(49153);
    if (!bytes) { fclose(file); return fail(s, "window-buffer", 0); }
    size_t n = fread(bytes, 1, 49153, file);
    bool read_ok = !ferror(file); fclose(file);
    uint32_t crc = UINT32_MAX;
    for (size_t i=0;i<n;i++) {
        crc^=bytes[i];
        for (unsigned bit=0;bit<8;bit++) crc=(crc>>1)^(0xedb88320u & (0u-(crc&1)));
    }
    crc=~crc;
    bool valid = read_ok && n == 49152 && crc == QUICKSTEP_NATIVE_RESOURCE_CRC;
    uint32_t magic = 0, version = 0, graphics = 0, definition = 0;
    if (valid) {
        memcpy(&magic, bytes, 4); memcpy(&version, bytes + 4, 4);
        memcpy(&graphics, bytes + 0x14, 4);
        valid = magic == 0x4448414c && version == 0x2713 && graphics < n - 0x18;
    }
    if (valid) {
        memcpy(&definition, bytes + graphics, 4);
        valid = definition <= n - graphics - 4 && bytes[graphics + definition + 3] == 9;
    }
    if (!valid) { free(bytes); return fail(s, "window-resource-contract", crc); }
    uint64_t raw = (uint64_t)(uintptr_t)hle_alloc(n);
    if (!range(raw, n)) { free(bytes); return fail(s, "window-resource-buffer", 0); }
    memcpy((void *)(uintptr_t)raw, bytes, n); free(bytes);
    // Native scene disposal is deferred. Keep the shared immutable raw resource
    // until guest reset, including after the equipment screen is closed.
    window_resource = raw;
    return true;
}
static bool create_frames(Cpu *c, NativeSlot *s, uint64_t original) {
    if (!load_window(s)) return false;
    // Policy headroom, not a reservation or a complete GPU/cache cost bound.
    // The native allocator/texture cache remain authoritative after this query.
    if (!quickstep_ui_memory_allow(c, 512 * 1024, "quickstep-window"))
        return fail(s, "window-arena-budget", 0);
    if (!current(s)) return fail(s,"window-budget-session",0);
    uint64_t source_lower = LD64(original + 0x38);
    if (!range(source_lower, 0xcc)) return fail(s, "window-source-layer", 0);
    uint32_t layer = LD32(source_lower + 0xc8);
    uint64_t *nodes[] = { &s->outline, &s->frame };
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t node = plain(c, 0x7000749710ull, 0, window_resource, 0, 0, 0);
        // Keep every returned owned node in this retired session's journal.
        // A new owner receives different scratch while this call is in flight.
        *nodes[i] = node;
        if (!current(s)) return fail(s,"window-native-session",i);
        if (!range(node, 0x380) || LD64(node) != 0x7000b72bb8ull
                || LD64(node + 0xc0) != window_resource || LD32(node + 0x2b8) != 9
                || LD16(node + 0x1a8) != 32 || LD16(node + 0x1aa) != 32)
            return fail(s, "window-native-result", i);
        visible(c, node, false);
        ST64(s->memory + WRAP, node);
        plain(c, 0x7000749f90ull, s->memory + WRAP, layer, 0, 0, 0);
        if (!current(s)) return fail(s, "window-session", i);
    }
    return true;
}
static void release(Cpu *c, const NativeSlot *s) {
    pthread_mutex_lock(&state_lock);
    if (state.generation==s->generation && state.slot==s->slot)
        atomic_store_explicit(&filtered_row, 0, memory_order_release);
    pthread_mutex_unlock(&state_lock);
    retire_frame(c, s->outline); retire_frame(c, s->frame);
    if (!range(s->root, 0x98) || LD64(s->root) != root_vtable) return;
    visible(c, LD64(s->root + 0x38), false);
    // The factory result carries one owned reference. Its deleting destructor
    // unlinks the wrapper, releases the asset handle and marks lower scene nodes
    // for the native scene manager; manually freeing those nodes is unsafe.
    uint32_t refs = LD32(s->root + 0x18);
    if (!refs || refs > 0x100000) FATAL("[quickstep-native] invalid owned resource reference\n");
    ST32(s->root + 0x18, refs - 1);
    if (refs == 1) plain(c, 0x700057a590ull, s->root, 0, 0, 0, 0);
}

// Keep the independent boots controller, genuine slot underlayer and glyphs.
// Its baked wide background is filtered at the checked draw-batch boundary;
// native children and mask disposal still follow the native path.
static bool prune(Cpu *c, NativeSlot *s) {
    if (!range(s->root, 0x98) || LD64(s->root) != root_vtable)
        return fail(s, "prune-root", 0);
    uint64_t nodes = LD64(s->root + 0x68), count = LD64(s->root + 0x70);
    uint64_t parts = LD64(s->root + 0x78), n_parts = LD64(s->root + 0x80);
    if (!count || count > MAX_NODES) return fail(s, "descriptor-count", (uint32_t)count);
    if (n_parts > MAX_COMPONENTS) return fail(s, "component-count", (uint32_t)n_parts);
    if (!range(nodes, count * 32)) return fail(s, "descriptor-buffer", 0);
    if (n_parts && !range(parts, n_parts * 16)) return fail(s, "component-buffer", 0);
    s->slot = find(c, s, slot_path, false);
    s->icon = find(c, s, icon_path, true);
    s->badge = find(c, s, badge_path, true);
    s->under = find(c, s, under_path, false);
    if (!current(s)) return fail(s, "prune-session", 0);
    if (!range(s->slot, 0x3d0)) return fail(s, "slot-lookup", 0);
    if (!range(s->icon, 0x48)) return fail(s, "icon-lookup", 0);
    if (!range(s->badge, 0x48) || !range(LD64(s->badge + 0x38), 0x280))
        return fail(s, "badge-lookup", 0);
    if (!range(s->under, 0x3d0)) return fail(s, "underlay-lookup", 0);
    if (!range(LD64(s->icon + 0x38), 0x280)) return fail(s, "icon-node", 0);
    // Reject alias wrappers or reused live visual content, even when lookup
    // succeeds. Both upper and lower icon identities must be independent.
    uint64_t original_root = LD64(s->ui + 8);
    memcpy((void *)(uintptr_t)(s->memory + PATH), icon_path, sizeof icon_path);
    uint64_t original_icon = plain(c, 0x700057a770ull, original_root, s->memory + PATH, 0, 0, 0);
    if (!current(s)) return fail(s, "prune-session", 1);
    if (!range(original_icon, 0x48)) return fail(s, "original-icon-lookup", 0);
    if (original_icon == s->icon) return fail(s, "icon-alias", 0);
    if (LD64(original_icon + 0x38) == LD64(s->icon + 0x38)) return fail(s, "icon-node-alias", 0);
    memcpy((void *)(uintptr_t)(s->memory + PATH), badge_path, sizeof badge_path);
    uint64_t original_badge = plain(c, 0x700057a770ull, original_root, s->memory + PATH, 0, 0, 0);
    if (!current(s)) return fail(s, "prune-session", 2);
    if (!range(original_badge, 0x48) || original_badge == s->badge
        || LD64(original_badge + 0x38) == LD64(s->badge + 0x38)) return fail(s, "badge-alias", 0);
    memcpy((void *)(uintptr_t)(s->memory + PATH), under_path, sizeof under_path);
    uint64_t original_under = plain(c, 0x700057aeb0ull, original_root, s->memory + PATH, 0, 0, 0);
    if (!current(s)) return fail(s, "prune-session", 3);
    if (!range(original_under, 0x3d0) || original_under == s->under)
        return fail(s, "underlay-alias", 0);
    bool keep[MAX_NODES] = {0};
    int selected = -1, under = -1;
    for (uint64_t i = 0; i < count; ++i) {
        if (LD64(nodes + i * 32) == s->slot) selected = (int)i;
        else if (LD64(nodes + i * 32) == s->under) under = (int)i;
    }
    if (selected < 0) return fail(s, "slot-descriptor", 0);
    if (under < 0) return fail(s, "underlay-descriptor", 0);
    int at = selected;
    for (uint64_t depth = 0; depth < count && at >= 0; ++depth) {
        if ((uint64_t)at >= count || keep[at]) return fail(s, "ancestor-chain", (uint32_t)at);
        keep[at] = true;
        at = (int32_t)LD32(nodes + (uint64_t)at * 32 + 0x14);
    }
    if (at != -1 || !keep[0]) return fail(s, "ancestor-root", (uint32_t)at);
    if ((int32_t)LD32(nodes + (uint64_t)under * 32 + 0x14) != selected)
        return fail(s, "underlay-parent", 0);
    keep[under] = true;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t node = LD64(nodes + i * 32);
        if (!range(node, 0x280)) return fail(s, "descriptor-node", (uint32_t)i);
        visible(c, node, i != 0 && keep[i]);
        if (!current(s)) return fail(s, "descriptor-session", (uint32_t)i);
    }
    for (uint64_t i = 0; i < n_parts; ++i) {
        uint64_t part = LD64(parts + i * 16);
        if (!part) continue;
        if (!range(part, 0x48) || !range(LD64(part + 0x38), 0x280))
            return fail(s, "component-node", (uint32_t)i);
        visible(c, LD64(part + 0x38), LD64(part + 0x38) == LD64(s->icon + 0x38)
            || LD64(part + 0x38) == LD64(s->badge + 0x38));
        if (!current(s)) return fail(s, "component-session", (uint32_t)i);
    }
    return current(s) || fail(s, "transform-session", 0);
}
static uint64_t original_part(Cpu *c, const NativeSlot *s, const char *path) {
    memcpy((void *)(uintptr_t)(s->memory + PATH), path, strlen(path) + 1);
    uint64_t upper = plain(c, 0x700057a770ull, LD64(s->ui + 8), s->memory + PATH, 0, 0, 0);
    return current(s) && range(upper, 0x48) && range(LD64(upper + 0x38), 0x220) ? LD64(upper + 0x38) : 0;
}
static float scalar(uint64_t at) { float f; memcpy(&f, (void *)(uintptr_t)at, 4); return f; }
typedef struct { float x, y, xx, yy, yx, xy; } Affine;
static Affine compose(Affine p, Affine q) {
    return (Affine){p.x+p.xx*q.x+p.xy*q.y, p.y+p.yx*q.x+p.yy*q.y,
        p.xx*q.xx+p.xy*q.yx, p.yy*q.yy+p.yx*q.xy,
        p.yx*q.xx+p.yy*q.yx, p.xx*q.xy+p.xy*q.yy};
}
static bool box(Cpu *c, NativeSlot *s, uint64_t source, Affine parent, float out[4]) {
    // Build the real native local image matrix into private scratch. Reading a
    // computed on-screen box alone fails when no ordinary gem socket is drawn.
    memcpy((void *)(uintptr_t)(s->memory + LOCAL), (void *)(uintptr_t)(source + 0x148), 0x3c);
    plain(c, 0x70007576c0ull, s->memory + MATRIX, s->memory + LOCAL, source + 0x194, 0, 0);
    if (!current(s)) return false;
    Affine local; memcpy(&local, (void *)(uintptr_t)(s->memory + MATRIX), sizeof local);
    Affine world = compose(parent, local);
    memcpy((void *)(uintptr_t)(s->memory + MATRIX), &world, sizeof world);
    float empty[] = {-FLT_MAX,-FLT_MAX,FLT_MAX,FLT_MAX};
    memcpy((void *)(uintptr_t)(s->memory + BOUNDS), empty, sizeof empty);
    plain(c, 0x700074e310ull, s->memory + BOUNDS, s->memory + MATRIX, source + 0x194, 0, 0);
    memcpy(out, (void *)(uintptr_t)(s->memory + BOUNDS), sizeof empty);
    return current(s) && isfinite(out[0]) && isfinite(out[1]) && isfinite(out[2]) && isfinite(out[3])
        && out[0]>out[2] && out[1]>out[3];
}
static bool geometry(Cpu *c, NativeSlot *s, float *row_axis) {
    uint64_t original = LD64(s->ui + 8);
    if (!range(original, 0x98) || LD64(original + 0x70) != 273) return false;
    uint64_t nodes = LD64(original + 0x68);
    if (!range(nodes, 273 * 32)) return false;
    uint64_t anim = LD64(nodes + 3 * 32);
    if (!range(anim, 0x3d0) || !range(LD64(anim + 0x2c8), 0x38)) return false;
    uint64_t frames = LD64(LD64(anim + 0x2c8) + 0x30);
    if (!range(frames, 17 * 0x6c)) return false;
    // Selected immutable cont02 mapping: EquipAnim frame16 is the always
    // present Weapon06 boots row at (11,277); frame8 Jem08 is at (264,277).
    // This uses the current composed native frame, including entry zoom and
    // common ancestor translation, even for naked/fixed/no-socket boots.
    Affine boots; memcpy(&boots, (void *)(uintptr_t)(frames + 16 * 0x6c), sizeof boots);
    if (!isfinite(boots.x) || !isfinite(boots.y) || !isfinite(boots.xx) || !isfinite(boots.yy)
            || boots.xx<0.1f || boots.xx>4 || boots.yy<0.1f || boots.yy>4
            || fabsf(boots.xy)>0.00001f || fabsf(boots.yx)>0.00001f) return false;
    uint64_t config_ptr = LD64(0x7000b8a278ull);
    if (!range(config_ptr, 8) || !range(LD64(config_ptr), 0x50)) return false;
    uint64_t dims = LD64(LD64(config_ptr) + 0x48);
    if (!range(dims, 0x5ec)) return false;
    s->projection_x = scalar(dims + 0x5e4) * 0.5f;
    s->projection_y = scalar(dims + 0x5e8) * -0.5f;
    if (!(s->projection_x>0.1f && s->projection_x<4 && s->projection_y>0.1f && s->projection_y<4)) return false;
    uint64_t icon = original_part(c,s,icon_path), badge = original_part(c,s,badge_path);
    if (!icon || !badge || !current(s)) return false;
    uint64_t source_row=LD64(nodes+42*32);
    if(!range(source_row,0x3d0) || LD32(source_row+0x3c4)!=3
            || !range(LD64(source_row+0x3b8),3*32)) return false;
    uint64_t defs=LD64(source_row+0x3b8);
    const int16_t rects[2][4]={{-12,-12,12,12},{-8,-8,8,7}};
    if(memcmp((void *)(uintptr_t)(defs+32+24),rects[0],8)
            || memcmp((void *)(uintptr_t)(defs+64+24),rects[1],8)
            || LD16(icon+0x1a8)!=40 || LD16(icon+0x1aa)!=40
            || LD16(badge+0x1a8)!=24 || LD16(badge+0x1aa)!=24) return false;
    Affine row = compose(boots,(Affine){253.f/1280,0,1,1,0,0});
    // Actual75E9C0 applies each native component's allocation rectangle before
    // drawing its image. A40px image is bound into24px here; omitting this
    // native step was the earlier small/left alignment error. These selected
    // binding rectangles are validated above, and actual ARM differential
    // probes cover both unity and non-uniform current ancestor scales.
    Affine main = compose(row,(Affine){3.f/1280,2.f/1280,.6f,.6f,0,0});
    Affine label = compose(row,(Affine){17.5f/1280,15.5f/1280,13.f/24,12.1875f/24,0,0});
    float a[4],b[4];
    if (!box(c,s,icon,main,a) || !box(c,s,badge,label,b)) return false;
    float left=fminf(a[2],b[2])*1280, right=fmaxf(a[0],b[0])*1280;
    float top=fminf(a[3],b[3])*720, bottom=fmaxf(a[1],b[1])*720;
    // The existing OUTER dark frame is490units wide, not the214unit gem
    // cream card or its over-wide text field. Keep the new frame beyond it.
    s->target_left = (boots.x + boots.xx*(490.f-11.f)/1280)*s->projection_x*1280 + 4;
    s->center_x = (left+right)*0.5f; s->center_y=(top+bottom)*0.5f;
    *row_axis = boots.xx*s->projection_x;
    return left>100 && right<1000 && right>left && right-left<160
        && top>80 && bottom<720 && s->target_left>right && s->target_left<1100;
}
static bool presentation(Cpu *c, NativeSlot *s, uint64_t now) {
    float axis;
    if (!geometry(c,s,&axis) || !current(s)) return false;
    if (!s->reveal_start || now<s->reveal_start) s->reveal_start=now;
    double t=(double)(now-s->reveal_start)/120000000.0;
    if(t>1) t=1;
    float hidden=(float)((1-t)*(1-t)*(1-t));
    float left=s->target_left+hidden*18;
    // Retain the entire real icon/underlayer/badge group at1x. Native cached
    // parent scaling is accounted for instead of squeezing a baked wide row.
    transform(c,s->slot,true,1,1);
    transform(c,s->slot,false,(left+30-s->center_x)/axis,0);
    uint64_t glyphs[]={LD64(s->icon+0x38),LD64(s->badge+0x38),s->under};
    for(unsigned i=0;i<3;i++) {
        transform(c,glyphs[i],true,1,1); transform(c,glyphs[i],false,0,0);
        color(c,s,glyphs[i],1,1,1,false); color(c,s,glyphs[i],0,0,0,true);
        if(!current(s)) return false;
    }
    // A real native socket-only layer is below the neutral hex and S badge.
    // Coloring that layer never paints the hex interior or filled gem art.
    if(!s->uid) {
        // Multiply only: retain the native texture's transparent texels even
        // if a backend uses premultiplied blending. Never add RGB to alpha0.
        color(c,s,s->under,0,1.25f,0,false);
    }
    uint64_t windows[]={s->outline,s->frame};
    for(unsigned i=0;i<2;i++) {
        float w=i?60:64,h=i?44:48,margin=i?0:2;
        // This clean native nine-patch has a32px canvas and16px corners.
        // Native753EC0 keeps those corners fixed while its centre stretches.
        transform(c,windows[i],true,w/(32*s->projection_x),h/(32*s->projection_y)*(1280.f/720));
        transform(c,windows[i],false,(left-margin)/s->projection_x,
                  (s->center_y-h*0.5f)/s->projection_y*(1280.f/720));
        if(!current(s)) return false;
    }
    color(c,s,s->outline,s->focus?1.f:.29f,s->focus?1.f:.27f,s->focus?.15f:.25f,false);
    color(c,s,s->outline,0,0,0,true);
    return current(s);
}
static bool create(Cpu *c, NativeSlot *s) {
    uint64_t factory = LD64(s->sequence + 0x28), original = LD64(s->ui + 8);
    if (!range(factory, 0x190)) return fail(s, "factory-buffer", 0);
    uint64_t vtable = LD64(factory);
    // Native equipment resource ID5 uses the derived1B79B0 loader. Its factory
    // dispatch1B7A30 delegates to585040 and preserves the selector at+188.
    // The base loader is independently proved too; accept only these two exact
    // classes and their exact virtual factory methods, never an arbitrary class.
    uint64_t method = vtable == 0x7000b432a8ull ? 0x70001b7a30ull
        : vtable == 0x7000b671f0ull ? 0x7000585040ull : 0;
    if (!method) return fail(s, "factory-class", (uint32_t)vtable);
    if (!range(vtable, 0x28)) return fail(s, "factory-vtable-buffer", 0);
    if (LD64(vtable + 0x20) != method)
        return fail(s, "factory-method", (uint32_t)LD64(vtable + 0x20));
    if (!range(original, 0x98)) return fail(s, "original-root", 0);
    uint64_t original_lower=LD64(original+0x38);
    if (!s->memory) s->memory = (uint64_t)(uintptr_t)hle_alloc(MEMORY_BYTES);
    if (!range(s->memory, MEMORY_BYTES)) return fail(s, "scratch-buffer", 0);
    memset((void *)(uintptr_t)s->memory, 0, MEMORY_BYTES);
    // Separate conservative policy for the remaining full native equipment
    // instance. This is neither a measured upper bound nor an atomic reserve.
    if (!quickstep_ui_memory_allow(c,4*1024*1024,"quickstep-gem-control"))
        return fail(s,"factory-arena-budget",0);
    if (!current(s)) return fail(s,"factory-budget-session",0);
    if (!create_frames(c, s, original)) return false;
    call(c, 0x7000580bc0ull, factory, 0, 0, 0, 0, s->memory + RESULT, NULL, NULL);
    s->root = LD64(s->memory + RESULT);
    // Reject captured numeric aliases even if the owner retired while the
    // factory yielded. A failed stale call must never dispose original art.
    if (s->root == original) {
        s->root=0; return fail(s,"factory-result-alias",0);
    }
    if (!range(s->root, 0x98)) return fail(s, "factory-result", 0);
    if (LD64(s->root) != root_vtable) return fail(s, "factory-result-class", (uint32_t)LD64(s->root));
    s->lower = LD64(s->root + 0x38);
    // An independently created layout shares immutable templates, never a live
    // root. Reject aliases before changing any visibility or transform.
    if (!range(s->lower, 0x3d0)) return fail(s, "factory-lower-node", 0);
    if (s->lower == original_lower) {
        s->root = 0; return fail(s, "factory-node-alias", 0);
    }
    if (!current(s)) return fail(s,"factory-result-session",0);
    visible(c, s->lower, false);
    if (!current(s)) return fail(s, "factory-session", 0);
    ST64(s->memory + 8, s->root); ST64(s->memory + 0x10, s->context);
    ST32(s->memory + 0x24, UINT32_MAX);
    return prune(c, s);
}

static void sync_ancestors(Cpu *c, const NativeSlot *s) {
    // Mirror the original page/ancestor native timeline position. The clone
    // starts inside the already-open equipment screen instead of replaying its
    // entry animation. The bonus row itself retains its separate native focus.
    uint64_t nodes = LD64(s->root + 0x68), n = LD64(s->root + 0x70);
    uint64_t original = LD64(s->ui + 8), source = LD64(original + 0x68);
    if (LD64(original + 0x70) != n || n > MAX_NODES || !range(source, n * 32)) return;
    for (uint64_t i = 0; i < n; ++i) {
        uint64_t node = LD64(nodes + i * 32);
        if (node == s->slot || node == s->under || (i && !(LD32(node + 0xb0) & 1))) continue;
        uint64_t from = LD64(source + i * 32);
        if (!range(from, 0x288) || !range(LD64(from + 0x280), 0x24)) continue;
        ST64(s->memory + WRAP, from);
        uint32_t index = (uint32_t)plain(c, 0x70007499d0ull, s->memory + WRAP, 0, 0, 0, 0);
        uint32_t time = 0;
        call(c, 0x7000749a10ull, s->memory + WRAP, 0, 0, 0, 0, 0, NULL, &time);
        if (!current(s)) return;
        if (index == UINT32_MAX) continue;
        ST64(s->memory + WRAP, node);
        plain(c, 0x7000749c00ull, s->memory + WRAP, index, 0, 0, 0);
        float values[2] = {0}; memcpy(&values[0], &time, 4);
        call(c, 0x7000749a50ull, s->memory + WRAP, 0, 0, 0, 0, 0, values, NULL);
        if (!current(s)) return;
    }
}
static void refresh(Cpu *c, NativeSlot *s) {
    QuickstepGem item = {0};
    (void)quickstep_slot_get(c, s->context, s->character, &item);
    if (!current(s)) return;
    if (!s->dirty && item.uid == s->uid && !memcmp(item.fingerprint, s->fingerprint, 32)) {
        s->last_query = plat_now_ns(); return;
    }
    plain(c, 0x7000172c40ull, s->memory + ITEM, s->context, 0, 0, 0);
    plain(c, 0x7000172cf0ull, s->memory + ITEM, item.uid, 0, 0, 0);
    if (!current(s)) return;
    // This is a private, minimal UI view into our independent native resource.
    // The gem-only binder reads +8/+10/+24, writes its own availability/cache
    // cells, and never touches equipment/fashion wrappers or ordinary sockets.
    plain(c, 0x7000209240ull, s->memory, 13, s->memory + ITEM, 1, 0);
    if (!current(s)) return;
    // Binding can reveal text/favourite components; restore the native compact control.
    if (!prune(c, s)) { s->failed = true; report_failure(s); return; }
    s->uid = item.uid; memcpy(s->fingerprint, item.fingerprint, 32);
    s->last_query = plat_now_ns(); s->dirty = false;
    play(c,s,s->under,"slot");
    if (s->focus) ordinary_focus(c, s, false);
    // The borrowed native animation describes a214unit wide gem row. Keep it
    // OFF permanently; only our separately bounded small frame owns focus.
    if (current(s)) play(c, s, s->slot, "off");
}

void quickstep_native_construct(Cpu *c) {
    uint64_t sequence = c->x[19], ui = c->x[20];
    if (!g_settings.quickstep_slot || !range(sequence, SEQ_BYTES) || ui != sequence + SEQ_UI
            || !range(ui, UI_BYTES) || LD64(sequence + SEQ_UI_PTR) != ui || LD64(ui) != ui_vtable) return;
    pthread_mutex_lock(&state_lock);
    atomic_store_explicit(&filtered_row, 0, memory_order_release);
    NativeSlot previous = state;
    state = (NativeSlot){ .sequence = sequence, .ui = ui, .memory = previous.busy ? 0 : previous.memory,
                         .generation = ++serial };
    pthread_mutex_unlock(&state_lock);
    if (previous.sequence) quickstep_native_picker_forget(previous.sequence);
    if (previous.root && !previous.busy) release(c, &previous);
}
void quickstep_native_release(Cpu *c) {
    pthread_mutex_lock(&state_lock);
    NativeSlot previous = {0};
    if (state.ui == c->x[0]) {
        atomic_store_explicit(&filtered_row, 0, memory_order_release);
        previous = state;
        state = (NativeSlot){ .memory = previous.busy ? 0 : previous.memory, .generation = ++serial };
    }
    pthread_mutex_unlock(&state_lock);
    if (previous.sequence) quickstep_native_picker_forget(previous.sequence);
    if (previous.root && !previous.busy) release(c, &previous);
}
void quickstep_native_release_sequence(Cpu *c) {
    pthread_mutex_lock(&state_lock);
    uint64_t ui = state.sequence == c->x[0] ? state.ui : 0;
    pthread_mutex_unlock(&state_lock);
    if (ui) { Cpu saved = *c; c->x[0] = ui; quickstep_native_release(c); *c = saved; }
}
void quickstep_native_tick(Cpu *c) {
    pthread_mutex_lock(&state_lock);
    NativeSlot s = state;
    if (!s.sequence || s.ui != c->x[0] || s.busy) { pthread_mutex_unlock(&state_lock); return; }
    state.busy = true;
    pthread_mutex_unlock(&state_lock);
    bool valid = range(s.sequence, SEQ_BYTES) && LD64(s.sequence + SEQ_UI_PTR) == s.ui
        && range(s.ui, UI_BYTES) && LD64(s.ui) == ui_vtable;
    bool picker = valid && quickstep_native_picker_active(s.sequence);
    // This strict typed lease queries our owner API; call it outside the lock.
    bool menu = valid && quickstep_native_picker_context_menu_active(s.sequence);
    uint32_t character = valid ? LD32(s.sequence + SEQ_CHAR) : 0;
    uint64_t context = valid ? LD64(s.ui + 0x10) : 0;
    bool equipment = valid && g_settings.quickstep_slot && character_supported(character)
        && LD8(s.ui + 0x21) && LD8(s.ui + 0x29) && (!LD8(s.ui + 0x20) || picker || menu);
    if (character != s.character || context != s.context) {
        s.character = character; s.context = context; s.focus = false; s.dirty = true;
        if (!s.root) s.failed = false;
        pthread_mutex_lock(&state_lock);
        if (state.generation == s.generation) {
            s.generation = ++serial; state.generation = s.generation;
            state.character = character; state.context = context; state.focus = false;
        }
        pthread_mutex_unlock(&state_lock);
    }
    bool boots = equipment && (LD32(s.ui + 0x24) == 13
        || (LD32(s.ui + 0x24) == 5 && !LD8(s.ui + 0x328 + 13)));
    bool show = equipment && (boots || s.focus || picker || menu);
    if (!show && s.focus) {
        if (valid && LD8(s.ui + 0x29)) ordinary_focus(c, &s, true);
        s.focus = false;
    }
    if (!show || picker || menu) s.reveal_start = 0;
    if (show && !s.root && !s.failed) {
        if (!create(c, &s)) {
            release(c, &s);
            s.root = s.lower = s.slot = s.icon = s.badge = s.under = s.frame = s.outline = 0; s.failed = true;
            report_failure(&s);
        } else s.dirty = true;
    }
    bool presented = false;
    if (s.root && current(&s)) {
        if (show && !picker && !menu && !s.failed) {
            uint64_t now = plat_now_ns();
            if (s.dirty || !s.last_query || now < s.last_query || now - s.last_query > 100000000ull)
                refresh(c, &s);
            // A native equip/list close can restore the ordinary controller
            // after the transition event. Reconcile it on each visible base
            // frame; never clear a chooser or completed context-menu owner.
            if (s.focus && current(&s)) ordinary_focus(c,&s,false);
            if (current(&s)) sync_ancestors(c, &s);
            if (current(&s)) presented = presentation(c, &s, now);
        }
        if (current(&s)) {
            // Publish exact private ownership before any native scene can draw
            // the newly visible row. No native input controller is attached to
            // this visual-only clone, so its old wide art cannot become a hit
            // target; keyboard/gamepad focus uses only the routed action lease.
            pthread_mutex_lock(&state_lock);
            if (state.generation == s.generation) {
                state.root=s.root; state.slot=s.slot; state.lower=s.lower;
                atomic_store_explicit(&filtered_row,s.slot,memory_order_release);
            }
            pthread_mutex_unlock(&state_lock);
            bool draw=show && !picker && !menu && !s.failed && presented;
            visible(c,s.lower,draw); visible(c,s.frame,draw); visible(c,s.outline,draw);
        }
    }
    s.visible = show && s.root && !s.failed && (presented || picker || menu);
    pthread_mutex_lock(&state_lock);
    bool retired=state.generation!=s.generation;
    if (state.generation == s.generation) { s.busy = false; state = s; }
    pthread_mutex_unlock(&state_lock);
    // Teardown during a yielding helper retires the session immediately but
    // leaves its independently owned native objects alive until that helper
    // has returned. Never destroy its descriptor storage on the nested stack.
    if (retired) release(c,&s);
}

bool quickstep_native_bonus_context(uint64_t seq, uint64_t *context,
                                   uint32_t *character, uint64_t *generation) {
    pthread_mutex_lock(&state_lock);
    NativeSlot s = state;
    pthread_mutex_unlock(&state_lock);
    bool valid = g_settings.quickstep_slot && s.sequence == seq && s.focus
        && s.visible && s.root && !s.failed && identity(&s);
    if (valid) {
        if (context) *context = s.context;
        if (character) *character = s.character;
        if (generation) *generation = s.generation;
    }
    return valid;
}
void quickstep_native_bonus_picker_closed(uint64_t seq) {
    pthread_mutex_lock(&state_lock);
    if (state.sequence == seq && state.focus) state.dirty = true;
    pthread_mutex_unlock(&state_lock);
}
bool quickstep_native_stats_context(uint64_t panel, uint32_t character,
                                   uint64_t *sequence, uint64_t *context,
                                   uint64_t *generation) {
    pthread_mutex_lock(&state_lock);
    NativeSlot s = state;
    pthread_mutex_unlock(&state_lock);
    bool valid = g_settings.quickstep_slot && character_supported(character)
        && character == s.character && identity(&s) && panel
        && range(panel, 8) && LD64(s.sequence + 0x8440) == panel;
    if (valid) {
        if (sequence) *sequence = s.sequence;
        if (context) *context = s.context;
        if (generation) *generation = s.generation;
    }
    return valid;
}
bool quickstep_native_input(Cpu *c, uint64_t seq, QuickstepNativeAction action) {
    pthread_mutex_lock(&state_lock);
    NativeSlot s = state;
    pthread_mutex_unlock(&state_lock);
    if (!g_settings.quickstep_slot || seq != s.sequence || !s.visible || s.failed
            || s.busy || !identity(&s) || quickstep_native_picker_active(seq)) return false;
    if (!s.focus) {
        if (action != QUICKSTEP_NATIVE_RIGHT) return false;
        uint32_t selected = LD32(s.ui + 0x24);
        if (selected != 13 && !(selected == 5 && !LD8(s.ui + 0x328 + 13))) return false;
        s.focus = true; s.dirty = true;
        pthread_mutex_lock(&state_lock);
        if (state.generation == s.generation) { state.focus = true; state.dirty = true; }
        pthread_mutex_unlock(&state_lock);
        ordinary_focus(c, &s, false);
        if (current(&s)) play(c, &s, s.slot, "off");
        return true;
    }
    if (action == QUICKSTEP_NATIVE_CONFIRM) {
        (void)quickstep_native_picker_open(c, seq);
        return true;
    }
    if (action == QUICKSTEP_NATIVE_RIGHT) return true;
    pthread_mutex_lock(&state_lock);
    if (state.generation == s.generation) { state.focus = false; state.dirty = true; }
    pthread_mutex_unlock(&state_lock);
    ordinary_focus(c, &s, true);
    if (current(&s)) play(c, &s, s.slot, "off");
    return action == QUICKSTEP_NATIVE_LEFT || action == QUICKSTEP_NATIVE_BACK;
}
bool quickstep_native_input_event(Cpu *c) {
    // Checked mid-function native event-router ABI: X19 = Seq, X20 = event.
    // Popups/list children have already declined this event at the hook site.
    uint64_t event = c->x[20];
    if (!range(event, 5)) return false;
    // Bit0 is initial press versus held repeat, not press versus release.
    // The native producer emits no matching action on release; accepting its
    // flag0 direction repeats preserves the original navigation behavior.
    uint32_t id = LD32(event);
    if (id < 0x201 || id > 0x215) return false;
    if (quickstep_native_picker_input_event(c, c->x[19], id)) return true;
    // Native Plus/context menu owns this event and its complete modal flow.
    // Preserve our focus lease until its typed native dialog closes.
    if (id == 0x20d) return false;
    QuickstepNativeAction action = id == 0x201 ? QUICKSTEP_NATIVE_CONFIRM
        : id == 0x202 ? QUICKSTEP_NATIVE_BACK : id == 0x207 ? QUICKSTEP_NATIVE_LEFT
        : id == 0x208 ? QUICKSTEP_NATIVE_RIGHT : QUICKSTEP_NATIVE_OTHER;
    return quickstep_native_input(c, c->x[19], action);
}
bool quickstep_native_filter_background(Cpu *c) {
    uint64_t node=c->x[19];
    if (!node || atomic_load_explicit(&filtered_row,memory_order_acquire)!=node) return false;
    pthread_mutex_lock(&state_lock);
    NativeSlot s=state;
    // Character/context values can change before the next equipment tick.
    // They govern input/effects, not ownership of this private draw batch:
    // rejecting the capsule then would expose its wide native focus artwork.
    bool owned=s.slot==node && s.root && !s.failed && s.sequence
        && range(s.sequence,SEQ_BYTES) && s.ui==s.sequence+SEQ_UI && range(s.ui,UI_BYTES)
        && LD64(s.sequence+SEQ_UI_PTR)==s.ui && LD64(s.ui)==ui_vtable
        && range(s.root,0x98) && LD64(s.root)==root_vtable && range(node,0x3d0)
        && range(s.lower,0x3d0) && LD64(s.root+0x38)==s.lower
        && atomic_load_explicit(&filtered_row,memory_order_relaxed)==node;
    // Preserve the native batch bookkeeping immediately after the skipped LDR.
    if (owned) ST8(node+0x3c8,0);
    pthread_mutex_unlock(&state_lock);
    return owned;
}
RT_ON_RESET(quickstep_native_reset) {
    pthread_mutex_lock(&state_lock);
    atomic_store_explicit(&filtered_row, 0, memory_order_release);
    state = (NativeSlot){ .generation = ++serial };
    window_resource=0;
    pthread_mutex_unlock(&state_lock);
}
