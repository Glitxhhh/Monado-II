// Headless host stubs for the first boot (M3a): settings and randomizer hooks of the LAS services, no renderer.
// [XCDE] Replaced by the real settings layer in M4/M5.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../settings.h"
#include "../rando.h"

// (settings: runtime/host/settings_ini.c in the window build, settings_boot.c in the headless one)

void rando_init(void) {}
const char *rando_seed_dir(void) { return NULL; }
uint32_t rando_word(uint64_t addr) { (void)addr; return 0; }
bool rando_romfs_override(const char *path, char *out, size_t cap) { (void)path; (void)out; (void)cap; return false; }
void rando_boot(void) {}
void rando_select(const char *seed) { (void)seed; }

// LAS-only hooks that services/fs.c calls (save editor, flag extension, romfs read hook): nothing in XCDE.
void romfs_file_read_hook(const char *path) { (void)path; }
void save_editor_file_read(const char *path, uint64_t addr, int64_t off, int64_t size) { (void)path; (void)addr; (void)off; (void)size; }
void flags_ext_save_read(const char *path, uint64_t addr, int64_t off, int64_t size) { (void)path; (void)addr; (void)off; (void)size; }
void flags_ext_save_written(const char *path, uint64_t addr, int64_t off, int64_t size) { (void)path; (void)addr; (void)off; (void)size; }
