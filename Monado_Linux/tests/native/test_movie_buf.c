// T118: Decoder::GetOutputBuffer into a Buffer over the game's own memory must copy the frame there (runtime/hle/movie_buf.h).
#include <stdio.h>
#include <stdlib.h>

#include "hle/movie_buf.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void) {
    static uint8_t s0[64], s1[64], game[64], small[8];
    uint8_t *slots[2] = {s0, s1};
    for (int i = 0; i < 64; i++) s0[i] = (uint8_t)(i * 7 + 1), s1[i] = (uint8_t)(i * 3 + 2);

    // the game's own memory (Buffer(void *, size) + SetRange(0, size)): data copied in, its pointer kept
    memset(game, 0xEE, sizeof game);
    MBuffer b = {.base = (uint64_t)(uintptr_t)game, .capacity = sizeof game, .offset = 0, .size = sizeof game};
    CHECK(mbuf_fill_output(&b, slots, 2, 1, 64, 48));
    CHECK(b.base == (uint64_t)(uintptr_t)game);
    CHECK(b.size == 48 && b.offset == 0);
    CHECK(memcmp(game, s1, 48) == 0);
    CHECK(game[48] == 0xEE);  // nothing past the frame

    // an empty Buffer (Buffer()): it points at the decoder's slot
    MBuffer e = {0};
    CHECK(!mbuf_fill_output(&e, slots, 2, 0, 64, 40));
    CHECK(e.base == (uint64_t)(uintptr_t)s0 && e.capacity == 64 && e.size == 40);

    // the same Buffer object again for the next slot (it still points at slot 0): no copy over slot 0
    uint8_t keep[64];
    memcpy(keep, s0, sizeof keep);
    CHECK(!mbuf_fill_output(&e, slots, 2, 1, 64, 40));
    CHECK(e.base == (uint64_t)(uintptr_t)s1);
    CHECK(memcmp(keep, s0, sizeof keep) == 0);

    // game memory too small: never written past its end; the slot is used instead
    memset(small, 0xEE, sizeof small);
    MBuffer s = {.base = (uint64_t)(uintptr_t)small, .capacity = sizeof small};
    CHECK(!mbuf_fill_output(&s, slots, 2, 0, 64, 40));
    CHECK(s.base == (uint64_t)(uintptr_t)s0);
    for (int i = 0; i < 8; i++) CHECK(small[i] == 0xEE);

    printf(fails ? "test_movie_buf: %d failures\n" : "test_movie_buf: OK\n", fails);
    return fails != 0;
}
