// Tegra X1 block-linear layout. A GOB is 64 bytes x 8 rows; blocks stack GOBs vertically
// (block height) and in depth (block depth). Levels shrink their block height as they shrink.
#include "tegra.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdbool.h>

static uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
static uint64_t align_up(uint64_t a, uint64_t b) { return (a + b - 1) / b * b; }

uint32_t tegra_default_block_height_log2(uint32_t height_in_blocks) {
    // [XCDE] T147: the largest power of two not above the height in GOBs (rounded up), at most 16. Measured on the game's
    // own swizzled UI textures (BC7), each checked on screen: 56x56 (14 block rows) -> 2 GOBs (Battle Palette slots),
    // 80x80 (20) -> 2 (loading-screen kanji, matches a reference video), 144x144 (36) -> 4 (ring glow). Rounding up
    // to a power of two (the old rule) scrambled the last two; truncating rows / 8 first scrambled the first.
    // Final rule (same family as block_depth_for below, and the tegra_swizzle crate's block_height_mip0 used by the
    // Xenoblade texture tools): thresholds on 1.5x the height in blocks. Both power-of-two rules tried before broke a
    // different set of HUD textures (portraits, button icons, the enemy bar); this one fits every measured case.
    uint32_t h = height_in_blocks + height_in_blocks / 2;
    return h >= 128 ? 4 : h >= 64 ? 3 : h >= 32 ? 2 : h >= 16 ? 1 : 0;
}

// [XCDE] 3D textures: blocks are one GOB high and stack slices instead (measured on the game's 16x16x16 colour grading
// volumes: only bh 0 / bd 4 gives an ordered cube; same rule as ssbu-recomp and the tegra_swizzle crate).
static uint32_t block_depth_for(uint32_t depth) {
    uint32_t d = depth + depth / 2;
    return d >= 16 ? 4 : d >= 8 ? 3 : d >= 4 ? 2 : d >= 2 ? 1 : 0;
}

void tegra_layout_init(TexLayout *t) {
    uint32_t hb = div_up(t->height, t->fmt->bh);
    t->block_height_log2 = t->depth > 1 ? 0 : tegra_default_block_height_log2(hb);
    t->block_depth_log2 = t->depth > 1 ? block_depth_for(t->depth) : 0;
    {   // [XCDE] T147 (debug): XCDE_BH_TEST=<w>x<h>:<log2> forces the block height of 2D textures of that size
        const char *e = getenv("XCDE_BH_TEST");
        unsigned w, h, l;
        if (e && sscanf(e, "%ux%u:%u", &w, &h, &l) == 3 && t->width == w && t->height == h && t->depth <= 1) t->block_height_log2 = l;
    }
}

// Block height shrinks with the level so a block never exceeds the level height.
static uint32_t level_block_height(uint32_t bh_log2, uint32_t hb) {
    while (bh_log2 > 0 && (8u << (bh_log2 - 1)) >= hb) bh_log2--;
    return bh_log2;
}

static uint32_t level_block_depth(uint32_t bd_log2, uint32_t d) {
    while (bd_log2 > 0 && (1u << (bd_log2 - 1)) >= d) bd_log2--;
    return bd_log2;
}

static uint64_t level_size(const TexLayout *t, uint32_t level, LevelLayout *out) {
    uint32_t w = t->width >> level, h = t->height >> level, d = t->depth >> level;
    if (!w) w = 1;
    if (!h) h = 1;
    if (!d) d = 1;
    uint32_t wb = div_up(w, t->fmt->bw), hb = div_up(h, t->fmt->bh);
    uint32_t bh = level_block_height(t->block_height_log2, hb);
    uint32_t bd = level_block_depth(t->block_depth_log2, d);
    uint32_t gob_rows = 8u << bh, gob_depth = 1u << bd;
    uint64_t width_gobs = div_up(wb * t->fmt->bpb, 64);
    uint64_t size = width_gobs * 512 * (1u << bh) * (1u << bd) * div_up(hb, gob_rows) * div_up(d, gob_depth);
    if (out) {
        out->w = w; out->h = h; out->d = d;
        out->wb = wb; out->hb = hb;
        out->block_height_log2 = bh;
        out->block_depth_log2 = bd;
    }
    return size;
}

LevelLayout tegra_level(const TexLayout *t, uint32_t level) {
    LevelLayout l = {0};
    uint64_t off = 0;
    for (uint32_t i = 0; i < level; i++) off += level_size(t, i, NULL);
    level_size(t, level, &l);
    l.offset = off;
    return l;
}

uint64_t tegra_layer_size(const TexLayout *t) {
    uint64_t size = 0;
    for (uint32_t i = 0; i < t->levels; i++) size += level_size(t, i, NULL);
    if (t->layers > 1) {
        // layers are aligned to the level-0 block size - [XCDE] T154: at least 16 GOBs (8 KB). Measured on the game's
        // own 64x64 BC6H cube maps (time-of-day ambient probes): each face holds 15 GOBs of data (our level sizes,
        // level 0 two GOBs high) followed by one empty GOB, i.e. faces 8192 bytes apart. Aligned to the 2-GOB block
        // (7680) faces 1-5 were read shifted: the field's ambient light came out navy blue (cliffs in shade).
        uint32_t bh = t->block_height_log2 < 4 ? 4 : t->block_height_log2;
        size = align_up(size, 512ull << bh);
    }
    return size;
}

uint64_t tegra_total_size(const TexLayout *t) {
    return tegra_layer_size(t) * (t->layers ? t->layers : 1);
}

// Offset of byte (x bytes, y rows, z slices) within a block-linear surface.
static inline uint64_t gob_offset(uint32_t xb, uint32_t y, uint32_t z, uint32_t width_gobs, uint32_t bh_log2,
                                  uint32_t bd_log2, uint64_t slice_blocks_rows) {
    uint32_t gob_x = xb >> 6, gob_y = y >> 3;
    uint32_t block_y = gob_y >> bh_log2, gob_in_block_y = gob_y & ((1u << bh_log2) - 1);
    uint32_t block_z = z >> bd_log2, z_in_block = z & ((1u << bd_log2) - 1);
    uint64_t block_size = 512ull << (bh_log2 + bd_log2);
    uint64_t off = ((uint64_t)block_z * slice_blocks_rows + block_y) * width_gobs * block_size;
    off += (uint64_t)gob_x * block_size;
    off += ((uint64_t)z_in_block << bh_log2 | gob_in_block_y) * 512;
    // within the GOB: 64x8 bytes arranged in 16-byte sectors
    uint32_t xi = xb & 63, yi = y & 7;
    off += ((xi >> 5) << 8) | ((yi >> 1) << 6) | (((xi >> 4) & 1) << 5) | ((yi & 1) << 4) | (xi & 0x0F);
    return off;
}

typedef struct {
    const uint8_t *src; uint8_t *dst; uint32_t wb, hb, d, bpb, bh_log2, bd_log2; bool to_linear;
    uint32_t y0, y1;  // rows [y0, y1) of every slice
} CopyJob;

// [XCDE] perf: one GOB row (64 bytes) at a time - its four 16-byte sectors sit at fixed offsets from the GOB's base,
// so the full address is computed once per 64 bytes instead of per 16 (texture loads were ~280 ms hitches).
static void copy_rows(const CopyJob *j) {
    uint32_t row_bytes = j->wb * j->bpb;
    uint32_t width_gobs = (row_bytes + 63) / 64;
    uint32_t gob_rows = 8u << j->bh_log2;
    uint64_t blocks_rows = (j->hb + gob_rows - 1) / gob_rows;
    for (uint32_t z = 0; z < j->d; z++) {
        for (uint32_t y = j->y0; y < j->y1; y++) {
            uint8_t *lin = (j->to_linear ? j->dst : (uint8_t *)j->src) + ((uint64_t)z * j->hb + y) * row_bytes;
            uint8_t *blk = j->to_linear ? (uint8_t *)j->src : j->dst;
            uint32_t yi = y & 7;
            uint32_t rowpart = ((yi >> 1) << 6) | ((yi & 1) << 4);
            for (uint32_t x = 0; x < row_bytes; x += 64) {
                uint64_t g = gob_offset(x, y, z, width_gobs, j->bh_log2, j->bd_log2, blocks_rows) - rowpart;  // GOB base
                uint32_t n = row_bytes - x < 64 ? row_bytes - x : 64;
                for (uint32_t s = 0; s < n; s += 16) {
                    uint32_t m = n - s < 16 ? n - s : 16;
                    uint64_t o = g + (((s >> 5) << 8) | rowpart | (((s >> 4) & 1) << 5));
                    if (j->to_linear) memcpy(lin + x + s, blk + o, m);
                    else memcpy(blk + o, lin + x + s, m);
                }
            }
        }
    }
}

static void *copy_thread(void *arg) { copy_rows(arg); return NULL; }

static void copy_surface(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                         uint32_t bh_log2, uint32_t bd_log2, bool to_linear) {
    CopyJob base = {src, dst, wb, hb, d, bpb, bh_log2, bd_log2, to_linear, 0, hb};
    uint64_t bytes = (uint64_t)wb * bpb * hb * d;
    enum { MAXT = 8 };
    int nt = bytes >= (2u << 20) ? MAXT : bytes >= (512u << 10) ? 4 : 1;  // big surfaces: split the rows across threads
    if (nt > (int)hb) nt = 1;
    if (nt == 1) { copy_rows(&base); return; }
    CopyJob jobs[MAXT];
    pthread_t th[MAXT];
    bool started[MAXT] = {0};
    for (int i = 0; i < nt; i++) {
        jobs[i] = base;
        jobs[i].y0 = (uint32_t)((uint64_t)hb * i / nt);
        jobs[i].y1 = (uint32_t)((uint64_t)hb * (i + 1) / nt);
        if (i > 0) started[i] = pthread_create(&th[i], NULL, copy_thread, &jobs[i]) == 0;
    }
    copy_rows(&jobs[0]);
    for (int i = 1; i < nt; i++) {
        if (started[i]) pthread_join(th[i], NULL);
        else copy_rows(&jobs[i]);
    }
}

void tegra_deswizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                     uint32_t bh_log2, uint32_t bd_log2) {
    copy_surface(src, dst, wb, hb, d, bpb, bh_log2, bd_log2, true);
}

void tegra_swizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb,
                   uint32_t bh_log2, uint32_t bd_log2) {
    copy_surface(src, dst, wb, hb, d, bpb, bh_log2, bd_log2, false);
}

double tegra_layout_roughness(const uint8_t *src, uint64_t avail, uint32_t wb, uint32_t hb, uint32_t bpb, uint32_t bh_log2) {
    uint64_t width_gobs = div_up(wb * bpb, 64), rows = 8ull << bh_log2;
    uint64_t need = width_gobs * 512 * (1ull << bh_log2) * ((hb + rows - 1) / rows);
    if (need > avail || hb < 2 || !wb || !bpb) return -1;
    uint8_t *lin = malloc((uint64_t)wb * hb * bpb);
    if (!lin) return -1;
    tegra_deswizzle(src, lin, wb, hb, 1, bpb, bh_log2, 0);
    uint32_t nb = bpb < 4 ? bpb : 4;
    uint64_t sum = 0, n = 0;
    for (uint32_t y = 0; y + 1 < hb; y++)
        for (uint32_t x = 0; x < wb; x++) {
            const uint8_t *a = lin + ((uint64_t)y * wb + x) * bpb, *b = a + (uint64_t)wb * bpb;
            for (uint32_t k = 0; k < nb; k++) sum += a[k] > b[k] ? a[k] - b[k] : b[k] - a[k];
            n += nb;
        }
    free(lin);
    return n ? (double)sum / n : -1;
}
