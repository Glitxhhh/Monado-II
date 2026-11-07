// T147: default block height of block-linear textures: thresholds on 1.5x the height in blocks (>= 128 -> 16 GOBs,
// 64 -> 8, 32 -> 4, 16 -> 2, else 1; tegra_swizzle's block_height_mip0). Fits every case measured on screen. The game's packaged-less UI textures (e.g. an 80x80 BC7 = 20 block rows) were swizzled offline with this
// rule; rounding the division up decoded them scrambled.
#include <stdio.h>

#include "../../runtime/nvn/tegra.h"

static int fails;
static void check(unsigned hb, unsigned want) {
    unsigned got = tegra_default_block_height_log2(hb);
    if (got != want) { printf("FAIL height %u blocks: log2 %u, want %u\n", hb, got, want); fails++; }
}

int main(void) {
    check(1, 0);    // tiny: one GOB
    check(8, 0);    // exactly one GOB
    check(10, 0);   // 15 < 16 -> 1 GOB
    check(11, 1);   // 16 -> 2 GOBs
    check(16, 1);
    check(14, 1);   // 56x56 BC7 (Battle Palette slot): 2 GOBs, seen on screen
    check(20, 1);   // 80x80 BC7 (loading kanji): 2 GOBs (the first rule gave 4)
    check(36, 2);   // 144x144 BC7 (ring glow): 4 GOBs (the first rule gave 8)
    check(24, 2);   // 1.5x = 36 -> 4 GOBs (the rule before gave 2: broken portraits / button icons)
    check(42, 2);   // 1.5x = 63 < 64 -> 4 GOBs
    check(43, 3);   // 64 -> 8 GOBs
    check(126, 4);  // 504 px BC: 16
    check(146, 4);  // capped at 16
    check(1080, 4);
    {   // T148: a smooth image swizzled with 2 GOBs reads smoothest with 2 GOBs (the detector's premise)
        enum { WB = 20, HB = 40, BPB = 4 };
        static uint8_t img[WB * HB * BPB], sw[64 * 1024];
        for (int y = 0; y < HB; y++)
            for (int x = 0; x < WB; x++)
                for (int k = 0; k < BPB; k++) img[(y * WB + x) * BPB + k] = (uint8_t)(y * 2 + x + k * 30);
        tegra_swizzle(img, sw, WB, HB, 1, BPB, 1, 0);
        double r0 = tegra_layout_roughness(sw, sizeof sw, WB, HB, BPB, 0);
        double r1 = tegra_layout_roughness(sw, sizeof sw, WB, HB, BPB, 1);
        double r2 = tegra_layout_roughness(sw, sizeof sw, WB, HB, BPB, 2);
        if (!(r1 >= 0 && r1 < r0 * 0.7 && r1 < r2 * 0.7)) { printf("FAIL roughness bh0 %.2f bh1 %.2f bh2 %.2f\n", r0, r1, r2); fails++; }
    }
    printf(fails ? "test_tegra_block_height: %d failures\n" : "test_tegra_block_height: OK\n", fails);
    return fails != 0;
}
