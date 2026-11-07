// [XCDE] LINUX-009: exercise the real launcher UI loop's post-child path.
// Only process creation is injected: a tiny Python child edits synthetic INI/mods.
// No game ELF is executed and no production test behavior is added.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <cstdio>
#include <cstdlib>
static bool fixture_present(SDL_Renderer *renderer);
#define SDL_RenderPresent fixture_present
#define main launcher_main_under_test
#include "launcher.cpp"
#undef main
#undef SDL_RenderPresent
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line%d: %s\n", __LINE__, #x); abort(); } } while (0)
static const char *new_config = "volume = 143\nfps = 90\nwindow = 1600x900\nupscaler = dlss\nframe_gen = 1\ncutscenes = 30fg\nlanguage = ko\nnew_extra = child\nmod_existing = 1\nmod_new = 0\n";
static int child_exit;
static bool checked, spawned;
static unsigned frames;
static bool fixture_present(SDL_Renderer *renderer) {
    if (frames++ == 5) {
        CHECK(g.volume == 27 && g_custom.at("fps") == "75" && g_extra.at("old_extra") == "parent");
        CHECK(std::any_of(g_mods.begin(), g_mods.end(), [](const Mod &m) { return m.name == "existing" && m.present && !m.on; }));
        std::vector<std::string> args = {"/usr/bin/python3", "-B", "-c",
            "import pathlib,sys,time; time.sleep(.03); pathlib.Path(sys.argv[1]).write_text(sys.argv[2]); pathlib.Path(sys.argv[3]).mkdir(parents=True); sys.exit(int(sys.argv[4]))",
            path("xcde.ini"), new_config, path("mods/new/romfs"), std::to_string(child_exit)};
        launcher_linux::game_pid = launcher_linux::spawn(args, path("logs/synthetic-child.log"));
        CHECK(launcher_linux::game_pid > 0);
        // This synthetic child stands in only for an already-started game.
        launcher_linux::game_seen = true; spawned = true;
    } else if (spawned && !launcher_linux::game_pid && !checked) {
        checked = true;
        CHECK(g.volume == 143 && g.win == 1 && g_custom.at("fps") == "90" && g_custom.at("language") == "ko");
        CHECK(g_extra.size() == 1 && g_extra.at("new_extra") == "child");
        CHECK(g.upscaler == 1 && !g.frame_gen && !g.cut_fg);
        CHECK(child_exit ? g_status.find("exit 7") != std::string::npos : g_status == "Game closed. Settings are available.");
        FILE *f = fopen(path("xcde.ini").c_str(), "r"); CHECK(f); char text[1024]{};
        size_t n = fread(text, 1, sizeof text - 1, f); CHECK(!ferror(f)); CHECK(!fclose(f));
        CHECK(n == strlen(new_config) && !strcmp(text, new_config)); // reload must not rewrite the file
        fprintf(stderr, "Config reloaded, defaults/custom/extra reset, Linux capabilities reapplied, child status retained, INI unchanged\n");
        bool existing = false, added = false;
        for (const Mod &m : g_mods) { fprintf(stderr, "mod %s present=%d on=%d\n", m.name.c_str(), m.present, m.on);
            existing |= m.name == "existing" && m.present && m.on;
            added |= m.name == "new" && m.present && !m.on;
        }
        CHECK(existing && added);
        SDL_Event event{}; event.type = SDL_EVENT_QUIT; CHECK(SDL_PushEvent(&event));
    }
    CHECK(frames < 3000); SDL_Delay(1);
    return SDL_RenderPresent(renderer);
}
int main(int argc, char **argv) {
    const char *mode = getenv("FIXTURE_CHILD_EXIT"); child_exit = mode ? atoi(mode) : 0;
    int result = launcher_main_under_test(argc, argv);
    CHECK(checked && !launcher_linux::game_pid);
    puts("launcher actual UI loop: synthetic child exit reloads settings and mods without writing INI OK");
    return result;
}
