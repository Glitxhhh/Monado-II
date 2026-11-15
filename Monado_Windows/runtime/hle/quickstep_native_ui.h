// [XCDE] LINUX-040: native Quick Step equipment focus session contract.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../cpu.h"

// True only for the current character's owned bonus-slot focus/picker session.
// Callers must revalidate the generation after any native helper which can yield.
bool quickstep_native_bonus_context(uint64_t seq, uint64_t *context,
                                   uint32_t *character, uint64_t *generation);
void quickstep_native_bonus_picker_closed(uint64_t seq);
// Read-only owner lease for native gem-effect presentation. Unlike the picker
// query this deliberately does not require bonus focus, visibility or allocation.
// Re-query and compare sequence/context/generation after a yielding native call.
bool quickstep_native_stats_context(uint64_t panel, uint32_t character,
                                   uint64_t *sequence, uint64_t *context,
                                   uint64_t *generation);
