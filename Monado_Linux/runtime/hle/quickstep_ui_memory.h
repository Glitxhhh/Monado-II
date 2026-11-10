// [XCDE] LINUX-040 r4: conservative optional native UI allocation preflight.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../cpu.h"

// Samples the real active native arena under its own mutex. Releases that mutex
// before returning; this is a policy check, NOT an atomic allocation reservation.
// Preserves Cpu/FP and never changes allocator selection, capacity or ownership.
bool quickstep_ui_memory_allow(Cpu *, uint64_t headroom, const char *consumer);
