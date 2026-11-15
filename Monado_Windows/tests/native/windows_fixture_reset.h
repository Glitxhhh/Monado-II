// Exercise production COFF reset registrations between native section markers.
#pragma once
#include "../../runtime/hle.h"
__attribute__((used, section(".rst$a"))) static const ResetFn fixture_reset_begin = NULL;
__attribute__((used, section(".rst$z"))) static const ResetFn fixture_reset_end = NULL;
static void fixture_run_resets(void) {
    for (const ResetFn *entry = &fixture_reset_begin + 1; entry < &fixture_reset_end; ++entry)
        if (*entry) (*entry)();
}
