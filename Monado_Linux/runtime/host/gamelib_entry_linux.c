// [XCDE] LINUX-103: only the table getter is exported from local game code.
// Authored ABI glue; no guest instructions, assets or generated data are shipped.
#include "../gamelib.h"

extern const uint64_t main_fn_addrs[], sdk_fn_addrs[], main_fn_count, sdk_fn_count;
extern const GuestFn main_fn_ptrs[], sdk_fn_ptrs[];
static const GameLibTables tables = {GAMELIB_ABI, GAMELIB_ID, main_fn_addrs, sdk_fn_addrs,
    main_fn_ptrs, sdk_fn_ptrs, &main_fn_count, &sdk_fn_count};
GAMELIB_EXPORT const GameLibTables *lasgame_tables(void) { return &tables; }
