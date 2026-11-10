// [XCDE] LINUX-039: authored optional metadata, never a native save-format change.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "quickstep_slot.h"

enum { QUICKSTEP_SAVE_BYTES = 0x153f30, QUICKSTEP_META_MAX = 744 };
typedef struct {
    bool tracked, deleted;
    char name[32];
    size_t bytes;
    uint8_t metadata[QUICKSTEP_META_MAX];
    uint8_t payload_hash[32];
} QuickstepSaveWrite;

void quickstep_save_set_required(bool required);
void quickstep_save_serializing(Cpu *c); // AF760 entry: context=x1; reconcile first
void quickstep_save_serialized(Cpu *c); // checked AFa80: raw=x19, context=x20
void quickstep_save_intent(uint32_t story, uint16_t file, bool autosave);
void quickstep_save_loading(void); // suspend effects, retain confirmed intent
void quickstep_save_adopt(Cpu *c); // checked AFE60: playing=x20, raw=x22
void quickstep_save_loaded(Cpu *c); // checked 54DF24: context=x19, story finalized
// Full native read87040: task=x0, file=x1, auto=x2, destination=task+150.
// Intent is keyed by that destination; speculative async reads never adopt it.
void quickstep_save_read_intent(Cpu *c);
// Checked uncompressed reader5C4234: copied header at x8, source sp+70.
void quickstep_save_header_copy(Cpu *c);
// Checked completed native typed copies, preserving raw-file provenance.
void quickstep_save_cache_copy(Cpu *c); //39A424: destination=x19, source=x20
void quickstep_save_selected_copy(Cpu *c); //3992B4: destination=x21, source=*[x19]
void quickstep_save_continue_intent(Cpu *c); // after035 corrected logical selection
void quickstep_save_new_game(uint64_t playing_data, uint32_t story);
void quickstep_save_newgame_begin(Cpu *c); // ADCC0 entry: suspend and clear intent
void quickstep_save_newgame_end(Cpu *c); // AE860: finalized root=x19

// FS observations never adopt a scanned payload. Legacy/short files without
// optional metadata remain ordinary native loads. All paths are mount-relative.
void quickstep_save_observe(const char *root, const char *path,
                           const void *payload, int64_t offset, int64_t bytes);
int quickstep_save_prepare(const char *root, const char *path,
                          const void *payload, int64_t offset, int64_t bytes,
                          QuickstepSaveWrite *plan);
int quickstep_save_stage(const char *root, const QuickstepSaveWrite *plan);
int quickstep_save_changed(const char *root, const char *path, const char *destination);
int quickstep_save_flush(const char *root);
void quickstep_save_committed(const char *root);

// Small bounded codec helpers are public to authored synthetic fixtures only.
void quickstep_save_hash(const void *data, size_t bytes, uint8_t out[32]);
int quickstep_save_encode(const char *name, const uint8_t payload_hash[32],
                         uint64_t payload_bytes, const QuickstepSnapshot *snapshot,
                         uint8_t out[QUICKSTEP_META_MAX], size_t *bytes);
int quickstep_save_decode(const char *name, const uint8_t payload_hash[32],
                         uint64_t payload_bytes, const uint8_t *data, size_t bytes,
                         QuickstepSnapshot *snapshot);
