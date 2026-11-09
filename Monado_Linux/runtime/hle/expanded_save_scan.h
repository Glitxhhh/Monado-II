// [XCDE] LINUX-035: bounded extensions to the native title save scans.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../cpu.h"

void xcde_expanded_scan_continue_begin(uint64_t sequence);
void xcde_expanded_scan_continue_drop(uint64_t sequence);
// Called at the checked native latest-selection label, with x19=sequence.
// True defers the native chooser until the current asynchronous scan completes.
bool xcde_expanded_scan_continue_wait(Cpu *c);
// Called after the native context selection write; keeps the payload index native.
void xcde_expanded_scan_continue_selected(Cpu *c);

// Replaces only the checked call to the native title availability scan. The
// original four-slot scan and its validator remain the first stage.
void xcde_expanded_scan_presence(Cpu *c);
void xcde_expanded_scan_presence_reset(uint64_t checker);
void xcde_expanded_scan_presence_drop(uint64_t checker);
