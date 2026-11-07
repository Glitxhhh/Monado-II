// [XCDE] LINUX-014: headless tests of the actual launcher configuration path.
// Include the launcher to exercise load/save/reload; never call its UI main,
// initialize SDL, spawn a child, or access game input or a human profile.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>
#define main launcher_main_under_test
#include "launcher.cpp"
#undef main
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line%d: %s\n", __LINE__, #x); abort(); } } while (0)

static std::string contents() {
    FILE *f = fopen(path("xcde.ini").c_str(), "rb"); CHECK(f);
    std::string value; char buffer[1024]; size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, f))) value.append(buffer, n);
    CHECK(!ferror(f)); CHECK(!fclose(f)); return value;
}
static void write_config(const char *value) {
    FILE *f = fopen(path("xcde.ini").c_str(), "wb"); CHECK(f);
    CHECK(fputs(value, f) >= 0); CHECK(!fclose(f));
}
static void reset() {
    g = Cfg{}; g_custom.clear(); g_extra.clear(); g_mods.clear();
}
static void check_saved_key(const std::string &text, const char *value) {
    std::string line = std::string("menu_navigation_speed = ") + value + "\n";
    size_t at = text.find(line); CHECK(at != std::string::npos);
    CHECK(text.find("menu_navigation_speed = ", at + line.size()) == std::string::npos);
}
int main(int argc, char **argv) {
    CHECK(argc == 2); CHECK(mkdir(argv[1], 0700) == 0); g_root = argv[1];
    CHECK(!SDL_WasInit(0)); CHECK(!SDL_GetCurrentVideoDriver());
    CHECK(Cfg{}.menu_navigation_speed == 0);
    g.menu_navigation_speed = 1; load(); CHECK(g.menu_navigation_speed == 0);

    struct Case { const char *text; int expected; };
    const Case cases[] = {
        {"", 0}, {"menu_navigation_speed = normal\n", 0}, {"menu_navigation_speed = fast\n", 1},
        {"menu_navigation_speed = FAST\n", 0}, {"menu_navigation_speed = 1\n", 0},
        {"menu_navigation_speed = -1\n", 0}, {"menu_navigation_speed = slow\n", 0},
        {"menu_navigation_speed = \n", 0}, {"menu_navigation_speed = fast # comment\n", 0},
        {"menu_navigation_speed = fast\nmenu_navigation_speed = normal\n", 0},
        {"menu_navigation_speed = normal\nmenu_navigation_speed = fast\n", 1},
        {"menu_navigation_speed = fast\nmenu_navigation_speed = invalid\n", 0},
        {"menu_navigation_speed = invalid\nmenu_navigation_speed = fast\n", 1},
        {" \tmenu_navigation_speed \t= \tfast \t\r\n", 1},
        {"\xef\xbb\xbfmenu_navigation_speed = fast\n", 1},
    };
    unsigned load_cases = 0;
    for (const Case &test : cases) {
        reset(); g.menu_navigation_speed = 1; write_config(test.text);
        const std::string before = contents(); load();
        CHECK(g.menu_navigation_speed == test.expected);
        CHECK(g_extra.count("menu_navigation_speed") == 0 && g_custom.count("menu_navigation_speed") == 0);
        CHECK(contents() == before); // Merely reading never canonicalizes the file.
        g.icons = 1; CHECK(save()); check_saved_key(contents(), test.expected ? "fast" : "normal");
        g.menu_navigation_speed = 1 - test.expected; load(); CHECK(g.menu_navigation_speed == test.expected);
        load_cases++;
    }

    // Changing this one setting preserves custom runtime choices, unrelated
    // keys, controls and mod choices through the existing atomic save path.
    reset();
    write_config("menu_navigation_speed = normal\nfps = 75\nwindow = 1536x864\nresolution = 25%\n"
                 "language = ko\ncustom_key = first\ncustom_key = kept\nmenu_navigation_speed_future = kept\n"
                 "bind_layout = 2\nbind_key_a = F3\nbutton_icons = switch\nmod_test = 0\n"
                 "volume = 37\nvsync = 0\ncutscenes = 30\n");
    load(); CHECK(g.menu_navigation_speed == 0); g.menu_navigation_speed = 1; CHECK(save());
    std::string saved = contents(); check_saved_key(saved, "fast");
    reset(); load();
    CHECK(g.menu_navigation_speed == 1 && g.volume == 37 && !g.vsync && g.cutscenes == 1);
    CHECK(g_custom.at("fps") == "75" && g_custom.at("window") == "1536x864");
    CHECK(g_custom.at("resolution") == "25%" && g_custom.at("language") == "ko");
    CHECK(g_extra.at("custom_key") == "kept" && g_extra.at("menu_navigation_speed_future") == "kept");
    CHECK(g.key[0] == SDL_SCANCODE_F3 && g.icons == 1);
    CHECK(std::any_of(g_mods.begin(), g_mods.end(), [](const Mod &m) { return m.name == "test" && !m.on; }));

    // Exercise the actual post-child reload function directly, without a child
    // or event loop. A newly absent key must clear a previous Fast selection.
    g_status = "preserved status";
    write_config("volume = 143\nfps = 90\nnew_extra = child\n");
    std::string reload_input = contents(); reload_after_game();
    CHECK(g.menu_navigation_speed == 0 && g.volume == 143 && g_custom.at("fps") == "90");
    CHECK(g_extra.size() == 1 && g_extra.at("new_extra") == "child");
    CHECK(g_status == "preserved status" && contents() == reload_input);
    write_config("menu_navigation_speed = fast\nbutton_icons = switch\n");
    reload_input = contents(); reload_after_game(); CHECK(g.menu_navigation_speed == 1);
    CHECK(contents() == reload_input && g_status == "preserved status");

    // A failed save must preserve the complete old file, including its speed.
    CHECK(save()); saved = contents(); check_saved_key(saved, "fast");
    g.menu_navigation_speed = 0;
    rlimit previous_limit; CHECK(getrlimit(RLIMIT_FSIZE, &previous_limit) == 0);
    rlimit small{128, previous_limit.rlim_max}; auto previous_signal = signal(SIGXFSZ, SIG_IGN);
    CHECK(previous_signal != SIG_ERR); CHECK(setrlimit(RLIMIT_FSIZE, &small) == 0);
    CHECK(!save()); CHECK(setrlimit(RLIMIT_FSIZE, &previous_limit) == 0);
    CHECK(signal(SIGXFSZ, previous_signal) != SIG_ERR); CHECK(contents() == saved);
    DIR *dir = opendir(g_root.c_str()); CHECK(dir);
    while (dirent *entry = readdir(dir)) CHECK(strncmp(entry->d_name, "xcde.ini.tmp.", 13));
    CHECK(!closedir(dir)); CHECK(save()); check_saved_key(contents(), "normal");

    g.ui_lang = 1;
    CHECK(!strcmp(T("Velocidad de navegación en menús"), "Menu navigation speed"));
    CHECK(!strcmp(T("Normal (estilo de 30 FPS)"), "Normal (30 FPS style)"));
    CHECK(!strcmp(T("Rápida (estilo de 60 FPS)"), "Fast (60 FPS style)"));
    CHECK(strstr(T("Controla las direcciones mantenidas en los menús y la selección de artes.\n"
                   "Rápida repite al doble de velocidad. La velocidad del juego y las animaciones de los menús no cambian."),
                 "Fast repeats twice as quickly."));
    g.ui_lang = 0;
    CHECK(!strcmp(T("Velocidad de navegación en menús"), "Velocidad de navegación en menús"));
    CHECK(!SDL_WasInit(0)); CHECK(!SDL_GetCurrentVideoDriver());
    printf("PASS launcher configuration: %u parse/save/reload cases, defaults, preservation, atomic failure, translations; no SDL initialization or UI\n", load_cases);
}
