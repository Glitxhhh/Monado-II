// [XCDE] perf: the clock conversions in runtime/kernel.c (rt_ticks) and runtime/platform/windows.c (plat_now_ns) use
// 64-bit quotient/remainder forms instead of 128-bit multiply-divide. Both must give exactly the 128-bit result.
#include <stdint.h>
#include <stdio.h>

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

int main(void) {
    int fail = 0;
    const uint64_t freqs[] = {10000000ull, 24000000ull, 3000000000ull, 14318180ull, 1ull, 17999999999ull};
    for (int i = 0; i < 4000000; i++) {
        uint64_t v = next() >> (next() & 63);  // all magnitudes
        uint64_t a = (uint64_t)((unsigned __int128)v * 192 / 10000), b = v / 625 * 12 + v % 625 * 12 / 625;
        if (a != b && fail++ < 5) printf("rt_ticks %llu: %llu vs %llu\n", (unsigned long long)v, (unsigned long long)a, (unsigned long long)b);
        uint64_t f = freqs[i % 6], c = v >> (i % 3 == 0 ? 0 : 8);
        unsigned __int128 w = (unsigned __int128)c * 1000000000u / f;
        if (w >> 64) continue;  // (the 128-bit form overflows uint64 too: not a value a clock reaches)
        uint64_t x = (uint64_t)w, y = c / f * 1000000000ull + c % f * 1000000000ull / f;
        if (x != y && fail++ < 10) printf("now_ns %llu/%llu: %llu vs %llu\n", (unsigned long long)c, (unsigned long long)f, (unsigned long long)x, (unsigned long long)y);
    }
    printf("time math: %s\n", fail ? "FAIL" : "OK");
    return fail != 0;
}
