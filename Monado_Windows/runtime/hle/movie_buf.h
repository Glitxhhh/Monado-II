// movie::Buffer as the HLE keeps it (opaque to the game except through its methods, <= 0x40 bytes), and how a
// decoder output reaches it. Header-only so tests/native/test_movie_buf.c can check it without FFmpeg.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint64_t base, capacity;
    int32_t offset, size;
    int64_t i64;
    uint32_t u32;
    int32_t i32;
} MBuffer;
_Static_assert(sizeof(MBuffer) <= 0x40, "movie::Buffer size");

// [XCDE] T118: Decoder::GetOutputBuffer(slot, Buffer*). The game may hand in a Buffer built over its own memory
// (Buffer(void *, size) + SetRange(0, size)) and then read that memory, never the Buffer: the decoded data must be
// copied there. XCDE's movie player does this for video (its uploader shows LCG "static" while its frame buffer is
// never written). With no memory of its own, or too little, the Buffer points at the decoder's slot instead. A
// Buffer still pointing at one of the decoder's slots (a reused Buffer object: the audio path) is not the game's
// memory. Returns true when the data was copied into the game's memory.
static inline bool mbuf_fill_output(MBuffer *b, uint8_t *const *slots, int nslots, int idx, uint32_t slot_cap, uint32_t size) {
    bool ours = false;
    for (int k = 0; k < nslots; k++)
        ours |= slots[k] && b->base >= (uint64_t)(uintptr_t)slots[k] && b->base < (uint64_t)(uintptr_t)slots[k] + slot_cap;
    if (b->base && !ours && b->capacity >= size) {
        memcpy((void *)(uintptr_t)b->base, slots[idx], size);
        b->offset = 0;
        b->size = (int32_t)size;
        return true;
    }
    memset(b, 0, sizeof *b);
    b->base = (uint64_t)(uintptr_t)slots[idx];
    b->capacity = slot_cap;
    b->size = (int32_t)size;
    return false;
}
