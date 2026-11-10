// [XCDE] LINUX-040 r3: native equipment effect-summary presentation.
#pragma once
#include "../cpu.h"

void quickstep_stats_begin(Cpu *c);
void quickstep_stats_count(Cpu *c);
// 0: execute original copy; 1: supplied native stack row, resume2393CC;
// -1: this owned frame became stale, stop its extra rows at239508.
int quickstep_stats_row(Cpu *c);
void quickstep_stats_compare(Cpu *c);
void quickstep_stats_end(Cpu *c);
// Native baseline calculation only: no selected equipment or socket replacement.
void quickstep_stats_refresh(Cpu *c, uint64_t sequence);
