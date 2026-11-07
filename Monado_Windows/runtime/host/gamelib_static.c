// The recompiled game linked statically into the executable (development builds); replaces gamelib.c. [XCDE]
#include <string.h>

#include "../gamelib.h"

extern const uint64_t main_fn_addrs[], sdk_fn_addrs[], main_fn_count, sdk_fn_count;
extern const GuestFn main_fn_ptrs[], sdk_fn_ptrs[];

static const GameLibTables tables = {GAMELIB_ABI, GAMELIB_ID, main_fn_addrs, sdk_fn_addrs, main_fn_ptrs, sdk_fn_ptrs,
                                     &main_fn_count, &sdk_fn_count};

bool gamelib_load(const char *game_dir, char *why, size_t why_size) { (void)game_dir; (void)why; (void)why_size; return true; }
const GameLibTables *gamelib_tables(void) { return &tables; }
bool gamelib_present(const char *game_dir) { (void)game_dir; return true; }
