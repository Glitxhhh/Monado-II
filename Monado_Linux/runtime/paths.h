// Where the runtime keeps its files (runtime/paths.c).
// [XCDE] LINUX-004: native Linux uses explicit, disjoint input/state roots.
// Call rt_paths_init_linux before platform logging, precompile or any worker.
// It resolves arguments against the starting cwd, validates existing state,
// relocates output-related environment defaults and makes state the cwd.
// This validation is not syscall confinement; Linux startup separately installs
// the write guard before opening runtime output or starting threads.
//
// Data root: everything the app writes or the user brings - extracted/ (game files), rando/ (seeds), save/,
// cache/ (shaders / pipelines), fonts/ (imported), larecomp.ini. rt_paths_init() picks it and makes it the
// working directory, so the rest of the runtime opens those by their relative names. In order:
//   1. LARECOMP_DATA_ROOT (tests, scratch installs)
//   2. dev / portable mode: the starting directory already has extracted/ (a source checkout: `./build/lasrecomp
//      extracted`, the test scripts) - used as is
//   3. macOS: ~/Library/Application Support/com.peiikai.lasrecomp (the .app bundle itself is never written to)
//      Windows: the folder of the .exe (portable, no installer); refuses to start, with a message, if not writable
//      Linux: $XDG_DATA_HOME/lasrecomp (~/.local/share/lasrecomp)
//
// Resources: files that ship with the app (helper programs, the window icon) - next to the executable (the
// Windows folder, the .app's Contents/MacOS), in the .app's Contents/Resources, or in the source tree.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RT_BUNDLE_ID "com.peiikai.lasrecomp"

void rt_paths_init(void);         // legacy initialization; Linux requires explicit setup first
const char *rt_data_root(void);   // absolute
bool rt_dev_mode(void);           // dev / portable mode (rule 2 above): e.g. a source checkout
#ifdef __linux__
bool rt_paths_init_linux(const char *input_root, const char *state_root, char *error, size_t n);
const char *rt_input_root(void);  // canonical extracted input; never an output directory
#endif

// A shipped file: `name` next to the executable or in Contents/Resources, else `dev_rel` in the source tree (the
// starting directory or the executable's parent; NULL = shipped builds only). Absolute path in out; false = none.
bool rt_resource(const char *name, const char *dev_rel, char *out, size_t n);

// Linux always refuses these deletion APIs: its shared user input is immutable.
// On legacy platforms, removes the named entry of game_dir (an extracted/ folder) only if
// game_dir is <data root>/extracted itself (symlinks resolved; a symlinked extracted/ is refused) and the entry is
// one the setup creates (exefs, romfs, romfs.bin, leftover folders) and not a link. The folder the user picked
// never gets here. true = removed or not there.
bool rt_remove_game_file(const char *game_dir, const char *name);
// All the game files set up there (rt_remove_game_file on each, then the empty folder). Saves, seeds, settings stay.
bool rt_remove_game_files(const char *game_dir);
// Bytes taken by the extracted game files in game_dir (links not followed).
unsigned long long rt_game_files_size(const char *game_dir);

typedef struct SDL_Window SDL_Window;
void rt_window_icon(SDL_Window *w);  // the shipped icon.png, if any

#ifdef __cplusplus
}
#endif
