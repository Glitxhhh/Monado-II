// Actual production guest FP dispatch over authored operands; no game input.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../runtime/cpu.h"
#include "../../runtime/platform/guest_fp_windows.h"

typedef struct { uint64_t precision, fpcr, n, m, a, expected; } FmaCase;
static void check(const FmaCase *test) {
    Cpu cpu; memset(&cpu, 0xa6, sizeof cpu);
    cpu.fpcr = test->fpcr;
    cpu.v[1] = test->n; cpu.v[2] = test->m; cpu.v[3] = test->a;
    Cpu expected = cpu; expected.v[0] = test->expected;
    if (test->precision == 64) fp_fma_d(&cpu, 0, 0, 0, 1, 2, 3);
    else { assert(test->precision == 32); fp_fma_s(&cpu, 0, 0, 0, 1, 2, 3); }
    if (memcmp(&cpu, &expected, sizeof cpu)) {
        fprintf(stderr, "FMA mismatch precision=%llu fpcr=%llx n=%llx m=%llx a=%llx got=%llx expected=%llx\n",
                (unsigned long long)test->precision, (unsigned long long)test->fpcr,
                (unsigned long long)test->n, (unsigned long long)test->m, (unsigned long long)test->a,
                (unsigned long long)cpu.v[0], (unsigned long long)test->expected);
        abort();
    }
    assert(fegetround() == FE_TONEAREST && !(_mm_getcsr() & 0xe040));
}
int main(int argc, char **argv) {
    assert(argc == 2 && monado_windows_guest_fp_init());
    // All five finite-operand failures from the first native differential run.
    static const FmaCase regression[] = {
        {64,0,0x0adb6fd3480b352a,0x0adb6fd3480b352a,0x3fdfffffffffffff,0x3fdfffffffffffff},
        {64,0,0x825c400000000000,0x825c400000000000,0xbf63000000000000,0xbf63000000000000},
        {64,0,0x3f,0x3f,0xbc66377d0ae3de5d,0xbc66377d0ae3de5d},
        {64,0,0x807fffff3f7ff000,0x80200000c290e687,0xc2d500003f000000,0xc2d500003f000000},
        {64,0,0x21ceec47c7a82793,0x495ca3c9c307c81c,0xf871d178d9631cde,0xf871d178d9631cde},
    };
    for (unsigned i = 0; i < sizeof regression / sizeof *regression; ++i) check(&regression[i]);
    FILE *input = fopen(argv[1], "rb"); assert(input);
    FmaCase test; size_t count = 0, bytes;
    while ((bytes = fread(&test, 1, sizeof test, input))) {
        assert(bytes == sizeof test); check(&test); ++count;
    }
    assert(!ferror(input) && !fclose(input) && count);
    printf("PASS %zu authored native FMA edge cases plus five retained UCRT regressions, CPU preservation and FP restoration\n", count);
    return 0;
}
