// [XCDE] LINUX-033: authored title-menu row transform, independent of guest memory.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int32_t action;
    uint8_t enabled;
    uint8_t pad[3];
} TitleExitRow;

// Called once on freshly constructed rows. Unknown/duplicate actions, missing
// Options or an already full menu are rejected without changing any input byte.
static inline bool xcde_title_exit_insert_rows(TitleExitRow rows[7]) {
    if (!rows) return false;
    TitleExitRow existing[6];
    unsigned count = 0, seen = 0;
    for (unsigned i = 0; i < 7; ++i) {
        int32_t action = rows[i].action;
        if (action == -1) continue;
        if (action < 0 || action > 6 || count == 6 || (seen & (1u << action))) return false;
        seen |= 1u << action;
        existing[count++] = rows[i];
    }
    if (!(seen & (1u << 4))) return false;
    TitleExitRow updated[7];
    const TitleExitRow blank = {-1, 0, {0, 0, 0}};
    const TitleExitRow exit_row = {7, 1, {0, 0, 0}};
    for (unsigned i = 0; i < 7; ++i) updated[i] = blank;
    // Native widgets are right aligned; the final mode row keeps its physical
    // widget when it was the last original action (including its divider).
    unsigned next = 7 - (count + 1);
    for (unsigned i = 0; i < count; ++i) {
        updated[next++] = existing[i];
        if (existing[i].action == 4) updated[next++] = exit_row;
    }
    for (unsigned i = 0; i < 7; ++i) rows[i] = updated[i];
    return true;
}
