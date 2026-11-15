// [XCDE] WINPARITY-001: each guest thread starts with RN, masked exceptions,
// cleared sticky flags and gradual underflow, independently of its creator.
#pragma once
#include <stdbool.h>
#include <xmmintrin.h>

static inline bool monado_windows_guest_fp_init(void) {
    __asm__ volatile("fninit" ::: "memory");
    _mm_setcsr(0x1f80);
    unsigned short control, status;
    __asm__ volatile("fnstcw %0" : "=m"(control));
    __asm__ volatile("fnstsw %0" : "=m"(status));
    return (control & 0x0c3f) == 0x003f && !(status & 0x3f) && _mm_getcsr() == 0x1f80;
}
