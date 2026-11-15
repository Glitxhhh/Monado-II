// [XCDE] WINPARITY-004: authored PE table ABI adapted from accepted Linux R5.
// Generated game instructions/tables are built locally from the player's input.
// This entry is the only public export of that private game-code DLL.
#include "../gamelib.h"

extern const uint64_t main_fn_addrs[], sdk_fn_addrs[], main_fn_count, sdk_fn_count;
extern const GuestFn main_fn_ptrs[], sdk_fn_ptrs[];
static const GameLibTables tables = {GAMELIB_ABI, GAMELIB_ID, main_fn_addrs, sdk_fn_addrs,
    main_fn_ptrs, sdk_fn_ptrs, &main_fn_count, &sdk_fn_count};
GAMELIB_EXPORT const GameLibTables *lasgame_tables(void) { return &tables; }
