// [XCDE] LINUX-002: binary single-instruction SIMD differential harness.
// Authored synthetic instructions only; actual runtime Cpu/SIMD helpers.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include "cpu.h"

#ifdef NDEBUG
#error "SIMD validation requires assertions enabled"
#endif
#ifdef __FAST_MATH__
#error "SIMD validation requires strict floating-point semantics"
#endif

extern GuestFn tests[];
extern int ntests;
#define BUF UINT64_C(0x7200000000)
#define BUF_SIZE 0x20000
#define CODE UINT64_C(0x7210000000)
#define CODE_SIZE 0x400000

// Any unexpected runtime operation is a test error, never a successful stub.
void rt_dispatch(Cpu *c) { (void)c; abort(); }
void rt_svc(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; abort(); }
void rt_brk(Cpu *c, uint32_t imm, uint64_t pc) { (void)c; (void)imm; (void)pc; abort(); }
void rt_udf(Cpu *c, uint64_t pc) { (void)c; (void)pc; abort(); }
uint64_t rt_ticks(void) { abort(); }

typedef struct {
    uint64_t k, fpcr, x[32], sp, nzcv;
    __uint128_t v[32];
    uint64_t store, reserved;
} Input;
_Static_assert(sizeof(Input) == 816 && offsetof(Input, v) == 288, "binary input layout");
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "little-endian harness protocol");

static void fail(const char *message) {
    fprintf(stderr, "SIMD harness: %s\n", message);
    exit(2);
}

static void *map_input(const char *path, uintptr_t address, size_t capacity, size_t minimum) {
    FILE *f = fopen(path, "rb");
    if (!f) fail("cannot open input image");
    if (fseek(f, 0, SEEK_END)) fail("cannot seek input image");
    long size = ftell(f);
    if (size <= 0 || (unsigned long)size > capacity || (size_t)size < minimum)
        fail("invalid input image size");
    if (fseek(f, 0, SEEK_SET)) fail("cannot rewind input image");
#ifdef _WIN32
    void *p = VirtualAlloc((void *)address, capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *p = mmap((void *)address, capacity, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) p = NULL;
#endif
    if (p != (void *)address) fail("cannot reserve exact guest test mapping");
    if (fread(p, 1, (size_t)size, f) != (size_t)size || ferror(f)) fail("cannot read input image");
    if (fclose(f)) fail("cannot close input image");
    return p;
}

static uint32_t crc32_buffer(const unsigned char *p) {
    static uint32_t table[256];
    static int initialized;
    if (!initialized) {
        for (unsigned i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (unsigned bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ ((crc & 1) ? UINT32_C(0xedb88320) : 0);
            table[i] = crc;
        }
        initialized = 1;
    }
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0; i < BUF_SIZE; ++i) crc = (crc >> 8) ^ table[(crc ^ p[i]) & 255];
    return ~crc;
}

int main(int argc, char **argv) {
    if (argc != 5) fail("expected code, buffer, cases, new output paths");
    if (ntests <= 0 || (size_t)ntests > CODE_SIZE / 4) fail("invalid test table size");
    map_input(argv[1], CODE, CODE_SIZE, (size_t)ntests * 4);
    unsigned char *buffer = map_input(argv[2], BUF, BUF_SIZE, BUF_SIZE);
    unsigned char *pristine = malloc(BUF_SIZE);
    if (!pristine) fail("cannot allocate pristine buffer");
    memcpy(pristine, buffer, BUF_SIZE);
    FILE *cases = fopen(argv[3], "rb");
    if (!cases) fail("cannot open case file");
    // Exclusive creation preserves evidence from earlier attempts.
    FILE *out = fopen(argv[4], "wbx");
    if (!out) fail("cannot create fresh output file");
    Input in;
    size_t count = 0, got;
    while ((got = fread(&in, 1, sizeof in, cases)) != 0) {
        if (got != sizeof in) fail("truncated input record");
        if (in.k >= (uint64_t)ntests || in.store > 1 || in.reserved != 0)
            fail("invalid input record");
        memcpy(buffer, pristine, BUF_SIZE);
        Cpu c = {0};
        memcpy(c.x, in.x, sizeof c.x);
        c.sp = in.sp; c.nzcv = in.nzcv; c.fpcr = in.fpcr;
        c.pc = CODE + 4 * in.k; c.excl_addr = UINT64_MAX;
        memcpy(c.v, in.v, sizeof c.v);
        tests[in.k](&c);
        uint64_t result[98]; // 31 X, SP, NZCV, memory CRC32, 32 V pairs.
        memcpy(result, c.x, 31 * sizeof(uint64_t));
        result[31] = c.sp; result[32] = c.nzcv & UINT64_C(0xf0000000);
        result[33] = in.store ? crc32_buffer(buffer) : 0;
        memcpy(result + 34, c.v, sizeof c.v);
        if (fwrite(result, 1, sizeof result, out) != sizeof result) fail("cannot write output record");
        ++count;
    }
    if (ferror(cases)) fail("cannot read cases");
    if (!count) fail("empty case file");
    if (fclose(cases) || fclose(out)) fail("cannot finish output");
    free(pristine);
    return 0;
}
