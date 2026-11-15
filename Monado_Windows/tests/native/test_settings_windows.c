// [XCDE] WINPARITY-003: actual parser/serializer and native failure preservation.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../runtime/settings.h"
#include "../../runtime/host/settings_transaction_windows.h"

bool bind_settings_load(const char *k, const char *v) { (void)k; (void)v; return false; }
void bind_settings_save(FILE *f) { fputs("bind_key_a = Return\n", f); }
int bind_slot_from_name(const char *name) { return !strcmp(name, "a") ? 0 : -1; }
static void put(const char *path, const char *value) {
    int fd = ws_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600); assert(fd >= 0);
    assert(_write(fd, value, (unsigned)strlen(value)) == (int)strlen(value)); assert(!_close(fd));
}
static char *get(const char *path) {
    int fd = ws_open(path, O_RDONLY); assert(fd >= 0);
    char *text = calloc(1, 1048577); assert(text);
    int n = _read(fd, text, 1048576); assert(n >= 0); assert(!_close(fd)); return text;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    char path[4096]; int length = snprintf(path, sizeof path, "%s/config space \xc3\xa9.ini", argv[1]);
    assert(length > 0 && (size_t)length < sizeof path);
    wchar_t wide[4096]; assert(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 4096));
    assert(!_wputenv_s(L"XCDE_INI", wide));
    put(path, "# personal comment\r\ncustom_key = unchanged\r\nbind_key_future = Custom\r\ncheat_exp = true\ncheat_exp_percent = 137\ninput_rumble = 0\nupscale_sharpness = 0\nquality_preset = performance\nresolution = 2\n");
    settings_load(); assert(g_settings.cheat_exp && g_settings.cheat_exp_percent == 137);
    assert(!g_settings.input_rumble && !g_settings.show_fps_overlay && !g_settings.quickstep_slot);
    assert(g_settings.upscale_sharpness == 0.f && g_settings.res_mode == 2); // loading preset never reapplies it
    const int points[] = {0, 1, 99, 100, 101, 137, 1999, 2000};
    for (unsigned i = 0; i < sizeof points / sizeof *points; ++i) {
        g_settings.cheat_exp_percent = points[i]; settings_save(); settings_load();
        assert(g_settings.cheat_exp && g_settings.cheat_exp_percent == points[i]);
        char *text = get(path);
        assert(strstr(text, "custom_key = unchanged\r\n"));
        assert(strstr(text, "bind_key_future = Custom\r\n"));
        assert(strstr(text, "# personal comment\r\n"));
        assert(!strstr(strstr(text, "# personal comment") + 1, "# personal comment"));
        free(text);
    }
    char *before = get(path);
    HANDLE locked = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    assert(locked != INVALID_HANDLE_VALUE);
    g_settings.cheat_exp_percent = 300; settings_save();
    char *after = get(path); assert(!strcmp(before, after)); free(after); CloseHandle(locked);
    free(before); assert(!ws_unlink(path));
    puts("PASS: native Unicode settings, defaults, percentages, unknown lines and locked replacement preservation");
    return 0;
}
