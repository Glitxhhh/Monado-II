// [XCDE] LINUX-039: authored character-owned Quick Step slot contracts.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../cpu.h"

enum { QUICKSTEP_CHARACTER_CAPACITY = 16, QUICKSTEP_GEM_CAPACITY = 500,
       QUICKSTEP_FINGERPRINT_BYTES = 32 };

typedef struct {
    uint32_t character, uid;
    uint8_t fingerprint[QUICKSTEP_FINGERPRINT_BYTES];
} QuickstepEntry;

typedef struct {
    uint32_t story, count;
    QuickstepEntry entries[QUICKSTEP_CHARACTER_CAPACITY];
} QuickstepSnapshot;

typedef enum {
    // Ordinary attachment includes a socket on an unworn inventory item.
    QUICKSTEP_FREE = 0, QUICKSTEP_ORDINARY_EQUIPPED = 1,
    QUICKSTEP_ASSIGNED_ELSEWHERE = 2,
} QuickstepAvailability;

typedef struct {
    uint32_t uid, character;
    uint8_t fingerprint[QUICKSTEP_FINGERPRINT_BYTES];
    uint8_t rank, value, availability;
    bool assigned;
} QuickstepGem;

// Guest-thread APIs. UI commands must provide the fingerprint shown when the
// command was created; stale instance-slot reuse must never select a new gem.
size_t quickstep_slot_list(Cpu *c, uint64_t context, uint32_t character,
                          QuickstepGem *out, size_t capacity);
bool quickstep_slot_get(Cpu *c, uint64_t context, uint32_t character, QuickstepGem *out);
// Non-yielding, read-only publication guard after get(): epoch0 captures the
// current adoption; a nonzero token requires that exact adoption and instance.
bool quickstep_slot_reference_matches(uint64_t context, uint32_t character,
                                     const QuickstepGem *expected, uint64_t *epoch);
bool quickstep_slot_set_checked(Cpu *c, uint64_t context, uint32_t character,
                               uint32_t uid, const uint8_t fingerprint[32]);
// Native Remove must identify the association shown to the user. A helper yield
// or a load/new game cannot clear a replacement association in another epoch.
bool quickstep_slot_clear_checked(Cpu *c, uint64_t context, uint32_t character,
                                 uint32_t expected_uid, const uint8_t expected_fingerprint[32]);

// Persistence owns confirmed load/new-game adoption and suspends before load.
// 'playing_data' is the decoded active native object, never a scanned payload.
void quickstep_slot_adopt(uint64_t playing_data, uint32_t story,
                         const QuickstepSnapshot *snapshot);
void quickstep_slot_suspend(void);
void quickstep_slot_snapshot(QuickstepSnapshot *out);
// Filter a save's point-in-time references against that exact serialized native
// payload; later acquisitions/reused UIDs cannot leak into an older snapshot.
bool quickstep_slot_snapshot_for_save(uint64_t serialized_raw, QuickstepSnapshot *out);
// Run after a confirmed load has finalized its story, and before serialization.
// Revalidates inactive assignments too and rebuilds native derived statistics.
void quickstep_slot_tick(Cpu *c, uint64_t context);

// Checked native hooks; preserve caller CPU/FP except explicitly documented
// effect-loop registers. All ordinary equipment, inventory and caps stay native.
void quickstep_slot_effects(Cpu *c);
void quickstep_slot_effects_end(Cpu *c);
void quickstep_slot_effects_finish(Cpu *c);
// Native 17B81C write boundary: X8=gear+18, X9=socket*8, W19=incoming gem UID.
// Executes before the original STR, preserving every CPU/FP register.
void quickstep_slot_native_equip(Cpu *c);
void quickstep_slot_native_consume(Cpu *c);
void quickstep_slot_native_destroy(Cpu *c);
