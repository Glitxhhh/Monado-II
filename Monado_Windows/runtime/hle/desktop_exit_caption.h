// [XCDE] LINUX-033: caption ownership shared by native desktop-exit dialogs.
#pragma once
#include <stdint.h>
#include "../cpu.h"

// The native MsgClient's result context is the endpoint carried by its packet.
// Register only while constructing an authored desktop-exit confirmation.
void xcde_exit_caption_begin(uint64_t context);
void xcde_exit_caption_end(uint64_t context);
void xcde_exit_caption_render(Cpu *c);
