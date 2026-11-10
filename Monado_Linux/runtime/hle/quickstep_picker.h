// [XCDE] LINUX-040: native gem list restricted to a character-owned bonus slot.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../cpu.h"

bool quickstep_native_picker_open(Cpu *c, uint64_t sequence);
bool quickstep_native_picker_active(uint64_t sequence);
// Runs at the native input router before ordinary boot/socket validation.
// Only an active bonus picker's Confirm0x201 is consumed; preserves CPU/FP.
// A verified assignment plays the native equip sound and closes the native
// picker. Rejected/stale selections stay open without success feedback.
bool quickstep_native_picker_input_event(Cpu *c, uint64_t sequence, uint32_t event);
void quickstep_native_picker_cancel(Cpu *c, uint64_t sequence);
// Checked callback ABI adapter; generated code needs only Cpu, not HLE bounds.
void quickstep_native_picker_cancel_callback(Cpu *c);
void quickstep_native_picker_forget(uint64_t sequence);

// Prefix hooks preserve CPU/FP, except count overrides W1; handled callbacks
// return W0=1, UID getter returns W0=the actual selected UID, and eligibility
// returns W0=whether this exact character currently owns a bonus association.
// Submenu is a void callback and preserves CPU/FP.
void quickstep_native_picker_filter(Cpu *c); // 183A60: X0=native vector.
void quickstep_native_picker_count(Cpu *c);  // 241B40: X0=list+58, W1=count.
// Exact native row-style callsite316604: X20=sequence, SP=ItemView,
// X0=the already-bound native row. Only W1's disabled flag may change.
void quickstep_native_picker_row_style(Cpu *c);
// Inline native context-menu item2FA178: X19=sequence, SP+2D0=action,
// SP+2D4=enabled. Preserves CPU/FP; only that native flag byte may change.
void quickstep_native_picker_menu_item(Cpu *c);
// Exact Plus20D parent-gate2F8CD8: X19=sequence,X20=active main list.
// Only W0 may change; native modal/empty-list gates still execute.
void quickstep_native_picker_menu_gate(Cpu *c);
// Completed native context-dialog publication2F9A3C: X19=sequence,X21=dialog.
void quickstep_native_picker_menu_opened(Cpu *c);
// Typed native popup + original owner lease, independent of visual visibility.
bool quickstep_native_picker_context_menu_active(uint64_t sequence);
bool quickstep_native_picker_confirm(Cpu *c); // 302640: callback+18=sequence.
bool quickstep_native_picker_remove(Cpu *c);  // 3020D0: callback+18=sequence.
bool quickstep_native_picker_uid(Cpu *c);     // 3004E0: X0=sequence.
bool quickstep_native_picker_details(Cpu *c); // 2FF870: X0=sequence.
bool quickstep_native_picker_preview(Cpu *c); // 2FAE60: X0=sequence.
bool quickstep_native_picker_commit(Cpu *c);  // 2FE000: X0=sequence.
bool quickstep_native_picker_eligible(Cpu *c); // 2FFD90: X0=sequence,W2=index.
bool quickstep_native_picker_submenu(Cpu *c); // 3035D0: callback+8=sequence.
void quickstep_native_picker_closed(Cpu *c);  // 2FF5FC: X0=sequence.
