// Authored lifetime adapter for one additional genuine native gem-effect row.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../cpu.h"
bool quickstep_stats_ui_ensure(Cpu *, uint64_t panel, uint32_t character);
void quickstep_stats_ui_layout(Cpu *, uint64_t panel, bool ninth);
void quickstep_stats_ui_tick(Cpu *);
void quickstep_stats_ui_release_panel(Cpu *);
void quickstep_stats_ui_release_sequence(Cpu *);
void quickstep_stats_ui_release_ui(Cpu *);
