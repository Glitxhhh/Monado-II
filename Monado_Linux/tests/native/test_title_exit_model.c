// [XCDE] LINUX-033: synthetic row bytes only; no guest input, SDL or graphics.
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "../../runtime/hle/title_exit_model.h"

_Static_assert(sizeof(TitleExitRow) == 8, "authored row layout must be eight bytes");
static unsigned long scenarios;

static TitleExitRow authored_row(int32_t action, unsigned index) {
    TitleExitRow row = {action, (uint8_t)(index % 3 == 0 ? 0 : index % 3 == 1 ? 1 : 255),
                       {(uint8_t)(0x31 + index), (uint8_t)(0x51 + index), (uint8_t)(0x71 + index)}};
    return row;
}

static void check_valid(TitleExitRow rows[7]) {
    TitleExitRow before[7];
    memcpy(before, rows, sizeof before);
    assert(xcde_title_exit_insert_rows(rows));
    unsigned original = 0, exits = 0;
    bool actions_started = false;
    for (unsigned i = 0; i < 7; ++i) {
        if (rows[i].action == -1) {
            const TitleExitRow blank = {-1, 0, {0, 0, 0}};
            assert(!memcmp(rows + i, &blank, sizeof blank));
            assert(!actions_started); // All unused widgets stay before the rows.
        } else {
            actions_started = true;
            if (rows[i].action == 7) {
                assert(i && rows[i - 1].action == 4);
                const TitleExitRow exit_row = {7, 1, {0, 0, 0}};
                assert(!memcmp(rows + i, &exit_row, sizeof exit_row));
                ++exits;
            } else {
                while (original < 7 && before[original].action == -1) ++original;
                assert(original < 7);
                assert(!memcmp(rows + i, before + original, sizeof rows[i]));
                ++original;
            }
        }
    }
    while (original < 7 && before[original].action == -1) ++original;
    assert(original == 7 && exits == 1);
    // This model is a once-per-setup transform, not an action7 input consumer.
    memcpy(before, rows, sizeof before);
    assert(!xcde_title_exit_insert_rows(rows));
    assert(!memcmp(rows, before, sizeof before));
    ++scenarios;
}

static void check_invalid(TitleExitRow rows[7]) {
    TitleExitRow before[7];
    memcpy(before, rows, sizeof before);
    assert(!xcde_title_exit_insert_rows(rows));
    assert(!memcmp(rows, before, sizeof before));
    ++scenarios;
}

// Enumerate physical layouts, including every order and every blank placement.
// Assertions check preservation/position invariants instead of reimplementing
// the transform as a second expected-output algorithm.
static void layouts(TitleExitRow rows[7], unsigned slot, unsigned used, unsigned count) {
    if (slot == 7) {
        if ((used & (1u << 4)) && count <= 6) {
            TitleExitRow sample[7];
            memcpy(sample, rows, sizeof sample);
            check_valid(sample);
        }
        return;
    }
    rows[slot] = authored_row(-1, slot);
    layouts(rows, slot + 1, used, count);
    if (count == 6) return;
    for (unsigned action = 0; action < 7; ++action) {
        if (used & (1u << action)) continue;
        rows[slot] = authored_row((int32_t)action, slot);
        layouts(rows, slot + 1, used | (1u << action), count + 1);
    }
}

int main(void) {
    assert(!xcde_title_exit_insert_rows(NULL));
    TitleExitRow rows[7];
    // A realistic sparse menu leaves Extra Story last and inserts Exit directly
    // after Options. Disabled rows and their authored padding remain unchanged.
    const int32_t sparse[7] = {0, -1, 3, 4, -1, 6, -1};
    for (unsigned i = 0; i < 7; ++i) rows[i] = authored_row(sparse[i], i);
    TitleExitRow extra = rows[5], disabled_options = rows[3];
    assert(!disabled_options.enabled);
    check_valid(rows);
    assert(rows[4].action == 4 && rows[5].action == 7 && rows[6].action == 6);
    assert(!memcmp(rows + 4, &disabled_options, sizeof disabled_options));
    assert(!memcmp(rows + 6, &extra, sizeof extra));

    const int32_t native_empty_flags[7] = {-1, -1, -1, -1, 0, 4, 6};
    const int32_t with_exit[7] = {-1, -1, -1, 0, 4, 7, 6};
    for (unsigned i = 0; i < 7; ++i) rows[i] = authored_row(native_empty_flags[i], i);
    extra = rows[6];
    check_valid(rows);
    for (unsigned i = 0; i < 7; ++i) assert(rows[i].action == with_exit[i]);
    assert(!memcmp(rows + 6, &extra, sizeof extra));

    // Every combination of the native constructor's three optional entries
    // keeps its final mode widget and moves earlier entries up by at most one.
    for (unsigned flags = 0; flags < 8; ++flags) {
        for (unsigned i = 0; i < 7; ++i) rows[i] = authored_row(-1, i);
        rows[6] = authored_row(6, 6);
        unsigned next = 5;
        if (flags & 4) { rows[5] = authored_row(5, 5); next = 4; }
        rows[next] = authored_row(4, next);
        if (flags & 2) { --next; rows[next] = authored_row(3, next); }
        if (flags & 1) { --next; rows[next] = authored_row(2, next); }
        --next; rows[next] = authored_row(0, next);
        TitleExitRow native[7]; memcpy(native, rows, sizeof native);
        check_valid(rows);
        assert(!memcmp(rows + 6, native + 6, sizeof rows[6]));
        for (unsigned i = 0; i < 7; ++i) {
            if (native[i].action < 0) continue;
            unsigned j = 0;
            while (j < 7 && rows[j].action != native[i].action) ++j;
            assert(j < 7 && j <= i && i - j <= 1);
        }
    }

    layouts(rows, 0, 0, 0);
    for (unsigned i = 0; i < 7; ++i) rows[i] = authored_row((int32_t)i, i);
    check_invalid(rows); // Seven valid existing actions cannot hold an eighth.
    for (unsigned i = 0; i < 7; ++i) rows[i] = authored_row(-1, i);
    check_invalid(rows); // No Options, including an entirely blank menu.
    rows[0] = authored_row(0, 0); rows[1] = authored_row(6, 1);
    check_invalid(rows);
    const int32_t bad[] = {-2, INT32_MIN, 7, 8, INT32_MAX};
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; ++i) {
        for (unsigned j = 0; j < 7; ++j) rows[j] = authored_row(-1, j);
        rows[1] = authored_row(4, 1); rows[5] = authored_row(bad[i], 5);
        check_invalid(rows);
    }
    for (int32_t action = 0; action <= 6; ++action) {
        for (unsigned i = 0; i < 7; ++i) rows[i] = authored_row(-1, i);
        rows[0] = authored_row(4, 0);
        rows[2] = authored_row(action, 2); rows[6] = authored_row(action, 6);
        check_invalid(rows);
    }
    printf("title Exit row model: %lu synthetic scenarios passed; original row bytes/order preserved\n", scenarios);
    return 0;
}
