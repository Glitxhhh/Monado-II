// [XCDE] LINUX-033: native hooks into the DE 2.0.0 title controller.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../cpu.h"

bool xcde_title_exit_configure(uint64_t ui);
bool xcde_title_exit_row_is_exit(uint64_t ui, uint64_t row);
bool xcde_title_exit_label(Cpu *c);
uint32_t xcde_title_exit_action(uint64_t sequence, uint32_t action);
bool xcde_title_exit_pending(uint64_t sequence);
void xcde_title_exit_dialog(Cpu *c);
bool xcde_title_exit_reply(Cpu *c);
void xcde_title_exit_check_dispatch(uint64_t sequence, uint32_t action);
