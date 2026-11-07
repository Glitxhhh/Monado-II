// [XCDE] LINUX-007: synthetic FP controls and host-thread environment checks.
// Production helpers are included unchanged in Clang builds. HOST_ONLY lets GCC
// independently check libc/pthread FP behavior without replacing Clang builtins.
#define _GNU_SOURCE
#include <fenv.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <xmmintrin.h>
#include "../../runtime/platform.h"
#ifndef HOST_ONLY
#include "../../runtime/simd.h"
#endif

static _Atomic unsigned failures, checks;
#define CHECK(tag, actual, expected) do { \
    uint64_t got_ = (uint64_t)(actual), want_ = (uint64_t)(expected); \
    atomic_fetch_add(&checks, 1); \
    if (got_ != want_) { atomic_fetch_add(&failures, 1); \
        fprintf(stderr, "FAIL %s: got %016llx expected %016llx\n", tag, \
                (unsigned long long)got_, (unsigned long long)want_); } \
} while (0)
#define NOINLINE __attribute__((noinline))

static uint32_t bitsf(float x) { uint32_t u; memcpy(&u, &x, 4); return u; }
static float valuef(uint32_t u) { float x; memcpy(&x, &u, 4); return x; }

// Volatile inputs and FENV_ACCESS are confined to the independent host probe:
// they must not change how the production helper header is compiled.
static NOINLINE float host_add(float a, float b) {
#ifdef __clang__
#pragma STDC FENV_ACCESS ON
#endif
    volatile float x = a, y = b;
    return x + y;
}

static pthread_barrier_t barrier;
static void *host_worker(void *arg) {
    int mode = (uintptr_t)arg ? FE_DOWNWARD : FE_UPWARD;
    CHECK("pthread inherits parent rounding", fegetround(), FE_TOWARDZERO);
    CHECK("set worker rounding", fesetround(mode), 0);
    CHECK("worker clear flags", feclearexcept(FE_ALL_EXCEPT), 0);
    CHECK("worker set flag", feraiseexcept(FE_DIVBYZERO), 0);
    pthread_barrier_wait(&barrier);
    for (unsigned i = 0; i < 1000; i++) {
        CHECK("per-thread rounding", fegetround(), mode);
        CHECK("directed host arithmetic", bitsf(host_add(1, valuef(0x33800000))),
              mode == FE_UPWARD ? 0x3f800001 : 0x3f800000);
        CHECK("per-thread exception flag", fetestexcept(FE_DIVBYZERO), FE_DIVBYZERO);
    }
    return NULL;
}
static void host_threads(void) {
    fenv_t saved;
    CHECK("save host environment", fegetenv(&saved), 0);
    CHECK("set parent rounding", fesetround(FE_TOWARDZERO), 0);
    CHECK("barrier init", pthread_barrier_init(&barrier, NULL, 2), 0);
    pthread_t a, b;
    CHECK("thread a", pthread_create(&a, NULL, host_worker, (void *)0), 0);
    CHECK("thread b", pthread_create(&b, NULL, host_worker, (void *)1), 0);
    CHECK("join a", pthread_join(a, NULL), 0);
    CHECK("join b", pthread_join(b, NULL), 0);
    CHECK("parent rounding unchanged", fegetround(), FE_TOWARDZERO);
    CHECK("barrier destroy", pthread_barrier_destroy(&barrier), 0);
    CHECK("restore host environment", fesetenv(&saved), 0);
}

static void host_reset(void) {
    fenv_t saved;
    CHECK("save reset probe environment", fegetenv(&saved), 0);
    CHECK("pollute reset probe rounding", fesetround(FE_UPWARD), 0);
    CHECK("pollute reset probe flags", feraiseexcept(FE_INEXACT), 0);
    CHECK("unmask divide-by-zero for reset probe", feenableexcept(FE_DIVBYZERO), 0);
    _mm_setcsr(_mm_getcsr() | (1u << 15) | (1u << 6));
    CHECK("production host reset", plat_guest_fp_init(), true);
    CHECK("reset round to nearest", fegetround(), FE_TONEAREST);
    CHECK("reset exception flags", fetestexcept(FE_ALL_EXCEPT), 0);
    CHECK("reset exception traps", fegetexcept(), 0);
    CHECK("reset MXCSR", _mm_getcsr() & 0xffff, 0x1f80);
    CHECK("restore reset probe environment", fesetenv(&saved), 0);
}

#ifndef HOST_ONLY
static NOINLINE float add32(Cpu *c, float a, float b) { return op_s(c, FOP_ADD, a, b, 0); }
static NOINLINE double add64(Cpu *c, double a, double b) { return op_d(c, FOP_ADD, a, b, 0); }
static NOINLINE float mul32(Cpu *c, float a, float b) { return op_s(c, FOP_MUL, a, b, 0); }
static NOINLINE float div32(Cpu *c, float a, float b) { return op_s(c, FOP_DIV, a, b, 0); }
static NOINLINE float fma32(Cpu *c, float a, float b, float d) { return op_s(c, FOP_MADD, a, b, d); }
static NOINLINE double fma64(Cpu *c, double a, double b, double d) { return op_d(c, FOP_MADD, a, b, d); }
static NOINLINE float int32(Cpu *c, uint64_t n) { return fromint_s(c, n, 32, 0, 0); }
static NOINLINE double int64(Cpu *c, uint64_t n) { return fromint_d(c, n, 64, 0, 0); }
static NOINLINE float narrow(Cpu *c, double n) { return simd_d2s(c, n); }

static void result_cases(void) {
    Cpu c = {0};
    CHECK("initial host rounding", fegetround(), FE_TONEAREST);
    CHECK("initial FTZ/DAZ disabled", _mm_getcsr() & ((1u << 15) | (1u << 6)), 0);
    for (unsigned rm = 0; rm < 4; rm++) {
        c.fpcr = (uint64_t)rm << 22;
        CHECK("float positive half ULP", fb_s(add32(&c, 1, fv_s(0x33800000))), rm == 1 ? 0x3f800001 : 0x3f800000);
        CHECK("float negative half ULP", fb_s(add32(&c, -1, fv_s(0xb3800000))), rm == 2 ? 0xbf800001 : 0xbf800000);
        CHECK("double positive half ULP", fb_d(add64(&c, 1, 0x1p-53)), rm == 1 ? 0x3ff0000000000001ull : 0x3ff0000000000000ull);
        CHECK("double negative half ULP", fb_d(add64(&c, -1, -0x1p-53)), rm == 2 ? 0xbff0000000000001ull : 0xbff0000000000000ull);
        CHECK("positive overflow", fb_s(mul32(&c, fv_s(0x7f7fffff), 2)), rm < 2 ? 0x7f800000 : 0x7f7fffff);
        CHECK("negative overflow", fb_s(mul32(&c, fv_s(0xff7fffff), 2)), rm == 0 || rm == 2 ? 0xff800000 : 0xff7fffff);
        CHECK("int to float rounding", fb_s(int32(&c, 16777217)), rm == 1 ? 0x4b800001 : 0x4b800000);
        CHECK("int to double rounding", fb_d(int64(&c, 9007199254740993ull)), rm == 1 ? 0x4340000000000001ull : 0x4340000000000000ull);
        CHECK("double to float rounding", fb_s(narrow(&c, 0x1.000001p0)), rm == 1 ? 0x3f800001 : 0x3f800000);
        CHECK("FRINTI positive", fb_s(frint_s(&c, 2.5f, 6)), rm == 1 ? 0x40400000 : 0x40000000);
        CHECK("FRINTI negative", fb_s(frint_s(&c, -2.5f, 6)), rm == 2 ? 0xc0400000 : 0xc0000000);
        vf32 v = vop_s(&c, FOP_ADD, (vf32){1,-1,1,-1}, (vf32){0x1p-24f,-0x1p-24f,0x1p-24f,-0x1p-24f}, (vf32){0});
        CHECK("vector positive rounding", fb_s(v[0]), rm == 1 ? 0x3f800001 : 0x3f800000);
        CHECK("vector negative rounding", fb_s(v[1]), rm == 2 ? 0xbf800001 : 0xbf800000);
        CHECK("helper restores host rounding", fegetround(), FE_TONEAREST);
    }
    c.fpcr = 0;
    CHECK("fused float cancellation", fb_s(fma32(&c, -1, 0x1.000002p0f, 0x1.fffffcp-1f)), 0xa8800000);
    CHECK("fused double cancellation", fb_d(fma64(&c, -1, 0x1.0000000000001p0, 0x1.ffffffffffffep-1)), 0xb970000000000000ull);
    CHECK("denormal input retained", fb_s(add32(&c, fv_s(1), fv_s(0x00800000))), 0x00800001);
    CHECK("denormal result retained", fb_s(mul32(&c, fv_s(0x00800000), 0.5f)), 0x00400000);
    c.fpcr = FPCR_FZ;
    CHECK("FZ input", fb_s(add32(&c, fv_s(1), fv_s(0x00800000))), 0x00800000);
    CHECK("FZ positive output", fb_s(mul32(&c, fv_s(0x00800000), 0.5f)), 0);
    CHECK("FZ negative output", fb_s(mul32(&c, fv_s(0x80800000), 0.5f)), 0x80000000);
    CHECK("FZ tininess before rounding", fb_s(mul32(&c, fv_s(0x00800000), fv_s(0x3f7fffff))), 0);
    c.fpcr = 0;
    CHECK("signalling NaN takes precedence", fb_s(add32(&c, fv_s(0x7fc00011), fv_s(0xff800022))), 0xffc00022);
    CHECK("first quiet NaN payload", fb_s(add32(&c, fv_s(0xffc00011), fv_s(0x7fc00022))), 0xffc00011);
    c.fpcr = FPCR_DN;
    CHECK("default NaN", fb_s(add32(&c, fv_s(0xffc00011), 1)), 0x7fc00000);
    c.fpcr = 0;
    CHECK("minimum signed zero", fb_s(minmax_s(&c, FOP_MIN, 0, fv_s(0x80000000))), 0x80000000);
    CHECK("maximum signed zero", fb_s(minmax_s(&c, FOP_MAX, 0, fv_s(0x80000000))), 0);
    CHECK("FP to int saturates positive", toint_s(&c, INFINITY, 3, 0, 32, 0), 0x7fffffff);
    CHECK("FP to int saturates negative", toint_s(&c, -INFINITY, 3, 0, 32, 0), 0x80000000);
    CHECK("FP to int NaN", toint_s(&c, fv_s(0x7fc00011), 3, 0, 32, 0), 0);
}

static void *guest_worker(void *arg) {
    Cpu c = {0};
    unsigned rm = (uintptr_t)arg ? 2 : 1;
    c.fpcr = (uint64_t)rm << 22;
    pthread_barrier_wait(&barrier);
    for (unsigned i = 0; i < 2000; i++) {
        CHECK("concurrent guest directed result", fb_s(add32(&c, 1, 0x1p-24f)), rm == 1 ? 0x3f800001 : 0x3f800000);
        CHECK("guest helper leaves caller nearest", fegetround(), FE_TONEAREST);
        CHECK("guest FPCR retained", c.fpcr, (uint64_t)rm << 22);
    }
    return NULL;
}
static void guest_threads(void) {
    pthread_t a, b;
    CHECK("guest barrier init", pthread_barrier_init(&barrier, NULL, 2), 0);
    CHECK("guest thread a", pthread_create(&a, NULL, guest_worker, (void *)0), 0);
    CHECK("guest thread b", pthread_create(&b, NULL, guest_worker, (void *)1), 0);
    CHECK("guest join a", pthread_join(a, NULL), 0);
    CHECK("guest join b", pthread_join(b, NULL), 0);
    CHECK("guest barrier destroy", pthread_barrier_destroy(&barrier), 0);
}

// This separately requested architectural probe fails until missing cumulative
// FPSR behavior is implemented; the runner records the open gap.
static void fpsr_probe(void) {
    Cpu c = {0};
    volatile float result = div32(&c, 1, 0); (void)result;
    CHECK("FPSR divide-by-zero cumulative flag", c.fpsr & 2, 2);
    result = div32(&c, 0, 0);
    CHECK("FPSR invalid operation cumulative flag", c.fpsr & 1, 1);
    c.fpcr = FPCR_FZ; result = add32(&c, fv_s(1), 1);
    CHECK("FPSR input denormal flag", c.fpsr & 0x80, 0x80);
    c.v[1] = 0x7f; c.v[2] = 1;
    sl_gen(&c, LOP_SQADD, 0, 1, 0, 0, 0, 0, 1, 0, 0, 2, 0, 0, -1, 0);
    CHECK("saturating result", (uint64_t)c.v[0], 0x7f);
    CHECK("FPSR cumulative saturation flag", c.fpsr & (1ull << 27), 1ull << 27);
}
static void host_pollution_probe(void) {
    fenv_t saved; fegetenv(&saved);
    unsigned csr = _mm_getcsr();
    Cpu c = {0};
    fesetround(FE_UPWARD);
    CHECK("production guest entry reset", plat_guest_fp_init(), true);
    CHECK("kernel entry establishes nearest", fegetround(), FE_TONEAREST);
    CHECK("guest nearest after host pollution", fb_s(add32(&c, 1, 0x1p-24f)), 0x3f800000);
    fesetround(FE_TONEAREST);
    _mm_setcsr(_mm_getcsr() | (1u << 15) | (1u << 6));
    CHECK("production guest entry denormal reset", plat_guest_fp_init(), true);
    CHECK("kernel entry enables denormals", _mm_getcsr() & ((1u << 15) | (1u << 6)), 0);
    CHECK("guest denormal after host pollution", fb_s(add32(&c, fv_s(1), fv_s(0x00800000))), 0x00800001);
    _mm_setcsr(csr); fesetenv(&saved);
}
#endif

int main(int argc, char **argv) {
    printf("host rounding=%d MXCSR=%08x\n", fegetround(), _mm_getcsr());
#ifdef HOST_ONLY
    (void)argc; (void)argv;
    host_reset(); host_threads();
    puts("HOST_ONLY: independent libc/pthread environment checks; production SIMD helpers not compiled");
#else
    if (argc == 2 && !strcmp(argv[1], "--fpsr")) fpsr_probe();
    else if (argc == 2 && !strcmp(argv[1], "--host-pollution")) host_pollution_probe();
    else if (argc == 1) { result_cases(); host_pollution_probe(); host_reset(); guest_threads(); host_threads(); }
    else return 2;
#endif
    printf("checks=%u failures=%u\n", atomic_load(&checks), atomic_load(&failures));
    return atomic_load(&failures) ? 1 : 0;
}
