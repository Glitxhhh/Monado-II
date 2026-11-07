// [XCDE] LINUX-009: actual settings/binding parser, synthetic files only.
// Unrelated UI/input sections are discarded at link time; SDL's real scancode
// conversion is retained. No window, controller, game, or human config is used.
#define _GNU_SOURCE
#include <assert.h>
#include <math.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include "../../runtime/host/settings_ini.c"
#include "../../runtime/hle/bindings.c"

static Settings defaults;

static void reset(void) {
    g_settings = defaults;
    g_window_w = 1280; g_window_h = 720;
    g_nmods = 0; g_party_switch = false;
    strcpy(g_button_icons, "auto");
    memcpy(g_bind_key, k_def_key, sizeof g_bind_key);
    memcpy(g_bind_key2, k_def_key2, sizeof g_bind_key2);
    memcpy(g_bind_pad, k_def_pad, sizeof g_bind_pad);
    g_mouse_look = 1; g_mouse_sens = 50; g_mouse_invert = false;
}

static void put(const char *path, const char *value) {
    FILE *f = fopen(path, "w"); assert(f);
    assert(fputs(value, f) >= 0); assert(!fclose(f));
}

static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb"); assert(f);
    assert(!fseek(f, 0, SEEK_END)); long n = ftell(f); assert(n >= 0);
    rewind(f); char *s = malloc((size_t)n + 1); assert(s);
    assert(fread(s, 1, n, f) == (size_t)n); s[n] = 0;
    assert(!fclose(f)); return s;
}

static void load(const char *value) {
    put("xcde.ini", value); settings_load();
}

static const char valid[] =
    "\xef\xbb\xbf# launcher configuration\n[settings]\n"
    " fullscreen = 1 \r\nwindow = 1920x1080\nresolution = 75%\n"
    "upscaler = fsr\nupscale_quality = balanced\nantialias = smaa\n"
    "depth_of_field = 0\ngame_blur = 0\nvsync = 0\nlauncher_lang = es\n"
    "stick_deadzone = 17\nparty_switch = 1\nvolume = 73\nmovie_audio = 0\n"
    "show_fps = 1\nfps = 60\nlanguage = ja\ncutscenes = 30\nmenu_navigation_speed = fast\n"
    "mod_test_pack = 0\nmod_icons_keyboard = 1\nbutton_icons = keyboard\n"
    "bind_layout = 2\nbind_key_a = F3\nbind_key2_a = Mouse X1\n"
    "bind_key_b = none\nbind_pad_a = ltrigger\n"
    "mouse_look = 2\nmouse_sens = 63\nmouse_invert = 1\n";

static void check_valid(void) {
    assert(g_settings.fullscreen && g_window_w == 1920 && g_window_h == 1080);
    assert(g_settings.res_mode == RES_Q75 && g_settings.upscaler == UPSCALE_FSR1);
    assert(g_settings.upscale_quality == UQ_BALANCED && g_settings.antialias == AA_SMAA);
    assert(g_settings.dof_off && !g_settings.game_blur && !g_settings.vsync && !g_settings.ui_lang);
    assert(g_settings.stick_deadzone == 17 && g_party_switch);
    assert(fabsf(g_settings.volume - .73f) < .0001f);
    assert(!g_settings.movie_audio && g_settings.show_fps && g_settings.fps == 60);
    assert(!strcmp(settings_language(), "ja") && g_settings.cutscene_mode == 1);
    assert(g_settings.menu_navigation_speed == MENU_NAV_FAST);
    assert(!settings_mod_enabled("test_pack") && settings_mod_enabled("icons_keyboard"));
    assert(!strcmp(g_button_icons, "keyboard"));
    assert(g_bind_key[BIND_A] == SDL_SCANCODE_F3 && g_bind_key2[BIND_A] == BIND_MOUSE_X1);
    assert(g_bind_key[BIND_B] == 0 && g_bind_pad[BIND_A] == PADB_LTRIGGER);
    assert(g_mouse_look == 2 && g_mouse_sens == 63 && g_mouse_invert);
}

static void inspect(void) {
    // Numeric arrays avoid escaping arbitrary user text; exact binding values
    // allow the GUI driver to compare the actual saved file with runtime state.
    printf("{\"fullscreen\":%d,\"window\":[%d,%d],\"resolution\":%d,\"upscaler\":%d,"
           "\"upscale_quality\":%d,\"frame_gen\":%d,\"cutscenes\":%d,\"antialias\":%d,"
           "\"depth_of_field\":%d,\"game_blur\":%d,\"vsync\":%d,\"launcher_lang\":%d,"
           "\"stick_deadzone\":%d,\"party_switch\":%d,\"volume\":%.9g,\"movie_audio\":%d,"
           "\"show_fps\":%d,\"fps\":%d,\"language\":%d,\"mouse_look\":%d,"
           "\"mouse_sens\":%d,\"mouse_invert\":%d,\"menu_navigation_speed\":%d,\"key\":[",
           g_settings.fullscreen, g_window_w, g_window_h, g_settings.res_mode, g_settings.upscaler,
           g_settings.upscale_quality, g_settings.frame_gen, g_settings.cutscene_mode, g_settings.antialias,
           !g_settings.dof_off, g_settings.game_blur, g_settings.vsync, g_settings.ui_lang,
           g_settings.stick_deadzone, g_party_switch, g_settings.volume, g_settings.movie_audio,
           g_settings.show_fps, g_settings.fps, g_settings.language, g_mouse_look, g_mouse_sens, g_mouse_invert,
           g_settings.menu_navigation_speed);
    for (int i = 0; i < BIND_KEY_COUNT; ++i) printf("%s%d", i ? "," : "", g_bind_key[i]);
    printf("],\"key2\":[");
    for (int i = 0; i < BIND_KEY_COUNT; ++i) printf("%s%d", i ? "," : "", g_bind_key2[i]);
    printf("],\"pad\":[");
    for (int i = 0; i < BIND_PAD_COUNT; ++i) printf("%s%d", i ? "," : "", g_bind_pad[i]);
    puts("]}");
}

int main(int argc, char **argv) {
    assert(argc >= 2); defaults = g_settings;
    unsetenv("XCDE_INI"); unsetenv("LARECOMP_HIDDEN");
    unsetenv("XCDE_DETERMINISTIC"); unsetenv("XCDE_INPUT_REPLAY");
    const char *mode = argv[1];
    if (!strcmp(mode, "inspect")) {
        assert(argc == 3); assert(!setenv("XCDE_INI", argv[2], 1));
        FILE *f = fopen(argv[2], "r"); assert(f); assert(!fclose(f));
        settings_load(); inspect(); return 0;
    }
    if (!strcmp(mode, "roundtrip")) {
        load(valid); check_valid(); settings_save(); reset(); settings_load(); check_valid();
    } else if (!strcmp(mode, "menu-navigation")) {
        assert(defaults.menu_navigation_speed == MENU_NAV_NORMAL);
        load("menu_navigation_speed = fast\nfps = 120\nvolume = 73\nbind_key_a = F3\n");
        assert(g_settings.menu_navigation_speed == MENU_NAV_FAST);
        assert(g_settings.fps == 120 && fabsf(g_settings.volume - .73f) < .0001f);
        assert(g_bind_key[BIND_A] == SDL_SCANCODE_F3);
        settings_save();
        char *s = read_all("xcde.ini");
        assert(strstr(s, "menu_navigation_speed = fast\n")); free(s);
        reset(); settings_load();
        assert(g_settings.menu_navigation_speed == MENU_NAV_FAST && g_settings.fps == 120);
        assert(fabsf(g_settings.volume - .73f) < .0001f && g_bind_key[BIND_A] == SDL_SCANCODE_F3);
        // Reload a missing key instead of merely starting from a fresh struct.
        load("fps = 60\n");
        assert(g_settings.menu_navigation_speed == MENU_NAV_NORMAL && g_settings.fps == 60);
        load(" \tmenu_navigation_speed \t= \tfast \t\r\n");
        assert(g_settings.menu_navigation_speed == MENU_NAV_FAST);
        const char *invalid[] = {"", "FAST", "Fast", "1", "2", "-1", "faster", "fast extra"};
        for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) {
            char text[128];
            snprintf(text, sizeof text, "menu_navigation_speed = %s\n", invalid[i]);
            g_settings.menu_navigation_speed = MENU_NAV_FAST;
            load(text); assert(g_settings.menu_navigation_speed == MENU_NAV_NORMAL);
        }
        load("menu_navigation_speed = fast\nmenu_navigation_speed = normal\n");
        assert(g_settings.menu_navigation_speed == MENU_NAV_NORMAL);
        load("menu_navigation_speed = normal\nmenu_navigation_speed = fast\n");
        assert(g_settings.menu_navigation_speed == MENU_NAV_FAST);
        load("menu_navigation_speed = fast\nmenu_navigation_speed = invalid\n");
        assert(g_settings.menu_navigation_speed == MENU_NAV_NORMAL);
        settings_save(); s = read_all("xcde.ini");
        assert(strstr(s, "menu_navigation_speed = normal\n")); free(s);
        g_settings.menu_navigation_speed = MENU_NAV_FAST; settings_load();
        assert(g_settings.menu_navigation_speed == MENU_NAV_NORMAL);
    } else if (!strcmp(mode, "selection")) {
        put("xcde.ini", "fps = 60\n"); put("selected.ini", "fps = 120\n");
        settings_load(); assert(g_settings.fps == 60);
        const char *flags[] = {"LARECOMP_HIDDEN", "XCDE_DETERMINISTIC", "XCDE_INPUT_REPLAY"};
        for (unsigned i = 0; i < sizeof flags / sizeof *flags; ++i) {
            reset(); assert(!setenv(flags[i], "1", 1)); settings_load(); assert(g_settings.fps == 30);
            assert(!setenv("XCDE_INI", "selected.ini", 1)); settings_load(); assert(g_settings.fps == 120);
            unsetenv(flags[i]); unsetenv("XCDE_INI");
        }
        assert(!setenv("XCDE_INI", "absent.ini", 1)); reset(); settings_load(); assert(g_settings.fps == 30);
    } else if (!strcmp(mode, "clamps")) {
        load("window = 1x2\nvolume = 999\nstick_deadzone = 99\nmouse_look = 88\nmouse_sens = -9\n");
        assert(g_window_w == 320 && g_window_h == 180 && g_settings.volume == 2);
        assert(g_settings.stick_deadzone == 30 && g_mouse_look == 2 && g_mouse_sens == 1);
        load("volume = -2\nstick_deadzone = -1\nmouse_look = -1\nmouse_sens = 101\n");
        assert(g_settings.volume == 0 && !g_settings.stick_deadzone && !g_mouse_look && g_mouse_sens == 100);
    } else if (!strcmp(mode, "bindings-invalid")) {
        load("bind_key_a = Unrecognized Key\nbind_key2_a = Unrecognized Key\nbind_pad_a = InvalidButton\n");
        assert(g_bind_key[BIND_A] == k_def_key[BIND_A]);
        assert(g_bind_key2[BIND_A] == k_def_key2[BIND_A] && g_bind_pad[BIND_A] == k_def_pad[BIND_A]);
        assert(!bind_settings_load("not_a_setting", "ignored"));
    } else if (!strcmp(mode, "linux-fallback")) {
        load("upscaler = dlss\nframe_gen = 1\ncutscenes = 30fg\n");
        assert(g_settings.upscaler == UPSCALE_FSR1 && !g_settings.frame_gen && g_settings.cutscene_mode == 1);
        char *s = read_all("xcde.ini"); assert(strstr(s, "upscaler = dlss")); free(s);
        load("upscaler = fsr3\n"); assert(g_settings.upscaler == UPSCALE_FSR1);
    } else if (!strcmp(mode, "invalid-volume")) {
        load("volume = nan\n");
        printf("observed volume=%g\n", g_settings.volume); fflush(stdout);
        assert(isfinite(g_settings.volume) && g_settings.volume >= 0 && g_settings.volume <= 2);
    } else if (!strcmp(mode, "invalid-fps")) {
        load("fps = -1\n"); printf("observed fps=%d\n", g_settings.fps); fflush(stdout);
        assert(g_settings.fps == 30);
        load("fps = 60garbage\n"); assert(g_settings.fps == 30);
    } else if (!strcmp(mode, "invalid-window")) {
        load("window = 2147483647x2147483647\n");
        printf("observed window=%dx%d\n", g_window_w, g_window_h); fflush(stdout);
        assert(g_window_w <= 16384 && g_window_h <= 16384);
        reset(); load("window = 999xgarbage\n"); assert(g_window_w == 1280 && g_window_h == 720);
    } else if (!strcmp(mode, "unknown-key")) {
        load("future_user_setting = retained value\nfps = 60\n"); settings_save();
        char *s = read_all("xcde.ini");
        bool kept = strstr(s, "future_user_setting = retained value") != NULL;
        printf("unknown key retained=%d\n", kept); fflush(stdout); free(s); assert(kept);
    } else if (!strcmp(mode, "save-io-error")) {
        put("xcde.ini", valid); char *before = read_all("xcde.ini");
        struct rlimit previous, limited; assert(!getrlimit(RLIMIT_FSIZE, &previous));
        limited = previous; limited.rlim_cur = 128;
        assert(signal(SIGXFSZ, SIG_IGN) != SIG_ERR); assert(!setrlimit(RLIMIT_FSIZE, &limited));
        settings_save(); assert(!setrlimit(RLIMIT_FSIZE, &previous));
        char *after = read_all("xcde.ini"); bool preserved = !strcmp(before, after);
        printf("config preserved after write failure=%d (before=%zu after=%zu)\n", preserved, strlen(before), strlen(after));
        fflush(stdout); free(before); free(after); assert(preserved);
    } else { fprintf(stderr, "unknown scenario: %s\n", mode); return 2; }
    printf("PASS %s\n", mode); return 0;
}
