// tegra_deswizzle / tegra_swizzle (GOB-row fast path, threaded for big surfaces) against the reference per-16-byte
// address formula, on random data for a spread of sizes, bytes per block, block heights and depths.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tegra.h"

static uint64_t ref_gob_offset(uint32_t xb, uint32_t y, uint32_t z, uint32_t width_gobs, uint32_t bh_log2, uint32_t bd_log2,
                               uint64_t slice_blocks_rows) {
    uint32_t gob_x = xb >> 6, gob_y = y >> 3;
    uint32_t block_y = gob_y >> bh_log2, gob_in_block_y = gob_y & ((1u << bh_log2) - 1);
    uint32_t block_z = z >> bd_log2, z_in_block = z & ((1u << bd_log2) - 1);
    uint64_t block_size = 512ull << (bh_log2 + bd_log2);
    uint64_t off = ((uint64_t)block_z * slice_blocks_rows + block_y) * width_gobs * block_size;
    off += (uint64_t)gob_x * block_size;
    off += ((uint64_t)z_in_block << bh_log2 | gob_in_block_y) * 512;
    uint32_t xi = xb & 63, yi = y & 7;
    off += ((xi >> 5) << 8) | ((yi >> 1) << 6) | (((xi >> 4) & 1) << 5) | ((yi & 1) << 4) | (xi & 0x0F);
    return off;
}

static void ref_deswizzle(const uint8_t *src, uint8_t *dst, uint32_t wb, uint32_t hb, uint32_t d, uint32_t bpb, uint32_t bh, uint32_t bd) {
    uint32_t row = wb * bpb, wg = (row + 63) / 64, gr = 8u << bh;
    uint64_t br = (hb + gr - 1) / gr;
    for (uint32_t z = 0; z < d; z++)
        for (uint32_t y = 0; y < hb; y++)
            for (uint32_t x = 0; x < row; x += 16) {
                uint32_t n = row - x < 16 ? row - x : 16;
                memcpy(dst + ((uint64_t)z * hb + y) * row + x, src + ref_gob_offset(x, y, z, wg, bh, bd, br), n);
            }
}

int main(void) {
    static const uint32_t sizes[][3] = {{1, 1, 1}, {7, 5, 1}, {16, 16, 1}, {33, 70, 1}, {64, 64, 1}, {300, 200, 1},
                                        {1024, 1024, 1}, {2048, 1024, 1}, {16, 16, 16}, {40, 24, 6}};
    static const uint32_t bpbs[] = {1, 2, 4, 8, 16};
    int fails = 0, cases = 0;
    srand(1);
    for (unsigned si = 0; si < sizeof sizes / sizeof sizes[0]; si++)
        for (unsigned bi = 0; bi < 5; bi++)
            for (uint32_t bh = 0; bh <= 4; bh++) {
                uint32_t wb = sizes[si][0], hb = sizes[si][1], d = sizes[si][2], bpb = bpbs[bi], bd = d > 1 ? 2 : 0;
                uint32_t row = wb * bpb, wg = (row + 63) / 64, gr = 8u << bh;
                uint64_t br = (hb + gr - 1) / gr;
                uint64_t ssz = (uint64_t)wg * 512 * (1u << bh) * (1u << bd) * br * ((d + (1u << bd) - 1) >> bd) + 64;
                uint64_t lsz = (uint64_t)row * hb * d;
                uint8_t *src = malloc(ssz), *a = calloc(1, lsz + 1), *b = calloc(1, lsz + 1), *back = calloc(1, ssz);
                for (uint64_t i = 0; i < ssz; i++) src[i] = (uint8_t)rand();
                ref_deswizzle(src, a, wb, hb, d, bpb, bh, bd);
                tegra_deswizzle(src, b, wb, hb, d, bpb, bh, bd);
                cases++;
                if (memcmp(a, b, lsz)) { fails++; printf("FAIL deswizzle %ux%ux%u bpb %u bh %u\n", wb, hb, d, bpb, bh); }
                // swizzle back: every byte the linear surface maps to must come back the same
                tegra_swizzle(a, back, wb, hb, d, bpb, bh, bd);
                uint8_t *c = calloc(1, lsz + 1);
                tegra_deswizzle(back, c, wb, hb, d, bpb, bh, bd);
                if (memcmp(a, c, lsz)) { fails++; printf("FAIL round trip %ux%ux%u bpb %u bh %u\n", wb, hb, d, bpb, bh); }
                free(src); free(a); free(b); free(back); free(c);
            }
    printf("tegra copy: %d cases, %d failures\n", cases, fails);
    return fails != 0;
}
