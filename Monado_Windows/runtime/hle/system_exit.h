// Authored Linux adaptation of the checked DE 2.0.0 System menu ABI.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../cpu.h"
void xcde_system_exit_construct(Cpu *c);
void xcde_system_exit_destroy(Cpu *c);
void xcde_system_exit_update(Cpu *c);
bool xcde_system_exit_help(Cpu *c);
bool xcde_system_exit_selected(Cpu *c);
uint32_t xcde_system_exit_action(uint64_t sequence, uint32_t action);
void xcde_system_exit_child(Cpu *c);
void xcde_system_exit_dialog(Cpu *c);
bool xcde_system_exit_reply(Cpu *c);
