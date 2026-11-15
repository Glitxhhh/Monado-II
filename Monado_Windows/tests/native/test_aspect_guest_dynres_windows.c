// [XCDE] LINUX-103: actual virtual-RomFS config policy with authored input.
// No game files, generated code, renderer, GPU or user settings are read.
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef MONADO_ASPECT_VIRTUAL_SOURCE
#define MONADO_ASPECT_VIRTUAL_SOURCE "../../runtime/services/romfs_virtual.c"
#endif
#include MONADO_ASPECT_VIRTUAL_SOURCE

Settings g_settings;
// Unrelated storage/renderer boundaries must never execute for INI fixtures.
ssize_t pread(int fd, void *out, size_t size, off_t offset) { (void)fd; (void)out; (void)size; (void)offset; abort(); }
uint64_t vk_frame_counter(void) { abort(); }
void plat_print_backtrace(void) { abort(); }
void settings_load(void) {} // Boundary: use the authored selected settings below.
bool settings_mod_enabled(const char *name) { (void)name; return true; }
bool kbicons_wanted(const char *path) { (void)path; return false; }
bool kbicons_init(void) { return false; }
uint8_t *kbicons_make(const char *path, uint64_t *length) { (void)path; (void)length; return NULL; }

static const char original[] = "# authored fixture\r\nred_Auto=on\r\nred_AtMinX=0.7\r\nred_AtMinY=0.7\r\nunrelated=retained\r\n";
static const char missing[] = "# authored replacement without dynamic setting\nred_AtMinX=0.7\nunrelated=retained\n";

static void mkdir_ok(const char *path) { assert(mkdir(path, 0700) == 0 || errno == EEXIST); }
static void write_input(const char *path, const char *text) {
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text)); assert(fclose(f) == 0);
}
static char *read_input(const char *path) {
    FILE *f = fopen(path, "rb"); assert(f);
    assert(fseek(f, 0, SEEK_END) == 0); long length = ftell(f); assert(length >= 0); rewind(f);
    char *text = malloc((size_t)length + 1); assert(text);
    assert(fread(text, 1, (size_t)length, f) == (size_t)length); text[length] = 0;
    assert(fclose(f) == 0); return text;
}
static char *fixture_overlay(const char *path, const char *text, int mode, int fps, int resolution) {
    memset(&g_settings, 0, sizeof g_settings);
    g_settings.game_blur = true; g_settings.aspect = mode;
    g_settings.upscaler = UPSCALE_BILINEAR; g_settings.fps = fps; g_settings.res_mode = resolution;
    write_input(path, text);
    File file = {.path = (char *)path, .size = strlen(text)};
    maybe_edit(&file); // Production settings/virtualINI path, not a copied parser.
    char *result = malloc((size_t)file.size + 1); assert(result);
    if (file.mem) memcpy(result, file.mem, (size_t)file.size);
    else memcpy(result, text, (size_t)file.size);
    result[file.size] = 0; free(file.mem);
    char *after = read_input(path); assert(!strcmp(after, text)); free(after);
    return result;
}
static void disabled(const char *text) {
    assert(strstr(text, "red_Auto=off\r\n"));
    assert(!strstr(text, "red_Auto=on"));
    assert(strstr(text, "unrelated=retained"));
    assert(strstr(text, "red_AtMinX=0.7"));
}
static void native_modes(const char *path) {
    const int modes[] = {ASPECT_UNLOCKED, ASPECT_16_10, ASPECT_21_9, ASPECT_32_9};
    const int rates[] = {30, 60, 90, 120, 144, 0};
    const int resolutions[] = {RES_MATCH, RES_DYNAMIC, RES_Q50, 1, 2};
    for (unsigned a = 0; a < sizeof modes / sizeof *modes; ++a)
        for (unsigned f = 0; f < sizeof rates / sizeof *rates; ++f)
            for (unsigned r = 0; r < sizeof resolutions / sizeof *resolutions; ++r) {
                char *text = fixture_overlay(path, original, modes[a], rates[f], resolutions[r]);
                disabled(text); free(text);
            }
}
static void original_modes(const char *path) {
    const int modes[] = {ASPECT_ORIGINAL, -1, N_ASPECTS, 200};
    const int rates[] = {30, 60, 120, 0};
    for (unsigned a = 0; a < sizeof modes / sizeof *modes; ++a)
        for (unsigned f = 0; f < sizeof rates / sizeof *rates; ++f) {
            char *text = fixture_overlay(path, original, modes[a], rates[f], RES_MATCH);
            assert(!strcmp(text, original)); free(text);
        }
}
int main(int argc, char **argv) {
    assert(argc == 2); assert(!getenv("XCDE_LIBNX"));
    mkdir_ok("monolib"); mkdir_ok("monolib/shader");
    const char *path = "monolib/shader/lib_nx.ini";
    if (!strcmp(argv[1], "modes")) native_modes(path);
    else if (!strcmp(argv[1], "original")) original_modes(path);
    else if (!strcmp(argv[1], "missing")) {
        char *text = fixture_overlay(path, missing, ASPECT_21_9, 120, RES_MATCH); disabled(text); free(text);
        text = fixture_overlay(path, missing, ASPECT_ORIGINAL, 120, RES_MATCH); assert(!strcmp(text, missing)); free(text);
    } else if (!strcmp(argv[1], "mod")) {
        mkdir_ok("mods"); mkdir_ok("mods/authored"); mkdir_ok("mods/authored/romfs");
        mkdir_ok("mods/authored/romfs/monolib"); mkdir_ok("mods/authored/romfs/monolib/shader");
        path = "mods/authored/romfs/monolib/shader/lib_nx.ini";
        char *text = fixture_overlay(path, original, ASPECT_32_9, 120, RES_DYNAMIC); disabled(text); free(text);
        text = fixture_overlay(path, original, ASPECT_ORIGINAL, 120, RES_DYNAMIC); assert(!strcmp(text, original)); free(text);
    } else if (!strcmp(argv[1], "override")) {
        assert(setenv("XCDE_LIBNX", "red_Auto=on,unrelated=diagnostic", 1) == 0);
        char *text = fixture_overlay(path, original, ASPECT_21_9, 120, RES_MATCH);
        assert(strstr(text, "red_Auto=off\r\n") && !strstr(text, "red_Auto=on"));
        assert(strstr(text, "unrelated=diagnostic\r\n")); free(text);
        text = fixture_overlay(path, original, ASPECT_ORIGINAL, 120, RES_MATCH);
        assert(strstr(text, "red_Auto=on\r\n")); free(text);
        assert(unsetenv("XCDE_LIBNX") == 0);
    } else if (!strcmp(argv[1], "malformed")) {
        const char *inputs[] = {
            "red_Auto\nunrelated=retained\nred_AtMinX=0.7\n",
            "# red_Auto=on\nunrelated=retained\nred_AtMinX=0.7",
            "red_Auto=on\nred_Auto = on\nunrelated=retained\nred_AtMinX=0.7\n",
            " red_Auto=on\nunrelated=retained\nred_AtMinX=0.7",
        };
        for (unsigned i = 0; i < sizeof inputs / sizeof *inputs; ++i) {
            char *text = fixture_overlay(path, inputs[i], ASPECT_UNLOCKED, 120, RES_MATCH);
            // Comments/malformed keys may contain the text 'red_Auto=on';
            // the appended exact assignment must determine the real setting.
            const char *last = NULL, *at = text;
            while ((at = strstr(at, "red_Auto="))) { last = at; at += 9; }
            assert(last && !strncmp(last, "red_Auto=off\r\n", 14));
            assert(strstr(text, "unrelated=retained")); free(text);
        }
    } else assert(!"unknown scenario");
    puts("native aspect guest-resolution policy: PASS"); return 0;
}
