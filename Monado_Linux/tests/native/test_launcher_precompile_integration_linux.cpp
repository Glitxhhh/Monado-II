// [XCDE] LINUX-018: production controller -> supervisor -> hidden native compiler.
// SDL headers only: this controller initializes no SDL/window/guest execution.
#include <SDL3/SDL.h>
#include "../../tools/launcher/linux_host.h"
#include "../../tools/launcher/linux_precompile.h"
#include <ctime>

using namespace launcher_linux;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s errno=%d controller=%s\n", __LINE__, #x, errno, error.c_str()); exit(1); } } while (0)
static double now() {
    timespec t{}; CHECK(!clock_gettime(CLOCK_MONOTONIC, &t));
    return t.tv_sec + t.tv_nsec / 1e9;
}
int main(int argc, char **argv) {
    CHECK(argc == 7);
    char *end = nullptr;
    long cancel_after = strtol(argv[5], &end, 10);
    CHECK(end && !*end && cancel_after >= 1 && cancel_after <= 30);
    long native_timeout = strtol(argv[6], &end, 10);
    CHECK(end && !*end && native_timeout >= cancel_after + 5 && native_timeout <= 50);
    std::string state;
    CHECK(canonical(argv[1], project, true) && canonical(argv[2], input, true) &&
          canonical(argv[3], game, false) && canonical(argv[4], state, true));
    struct stat a{}, b{};
    CHECK(!stat(state.c_str(), &a) && !stat("/tmp", &b) && a.st_dev == b.st_dev && a.st_ino == b.st_ino);
    char message[1024]{};
    CHECK(rt_paths_init_linux(input.c_str(), state.c_str(), message, sizeof message));
    CHECK(rt_linux_state_guard(state.c_str(), message, sizeof message));
    CHECK(!prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0));
    CHECK(!activity_busy() && start_precompile(state) && precompile_pid > 0);
    const pid_t owned = precompile_pid;
    const std::string session = precompile_session;
    CHECK(!start_precompile(state) && precompile_pid == owned && precompile_session == session);
    // The production busy gate returns before it can spawn Play.
    CHECK(!start_game(state) && !game_pid && precompile_pid == owned);
    const double began = now();
    bool requested = false, live = false, gpu = false;
    std::string phase;
    PrecompileSnapshot final;
    while (precompile_pid) {
        CHECK(now() - began < native_timeout);
        auto snap = poll_precompile();
        if (snap.valid && !snap.exited) {
            live = true;
            gpu |= snap.counts["gpu_vendor"] != 0;
            if (phase != snap.phase) {
                phase = snap.phase;
                printf("PHASE %s seconds=%.3f files=%llu/%llu programs=%llu/%llu gpu=%llu\n", phase.c_str(), now()-began,
                    (unsigned long long)snap.counts["files_done"], (unsigned long long)snap.counts["files_total"],
                    (unsigned long long)snap.counts["programs_done"], (unsigned long long)snap.counts["programs"],
                    (unsigned long long)snap.counts["gpu_vendor"]); fflush(stdout);
            }
        }
        if (precompile_pid && !requested &&
            ((snap.valid && snap.phase == "build" && snap.counts["programs_done"] >= 8) || now()-began >= cancel_after)) {
            CHECK(cancel_precompile()); requested = true;
            printf("CANCEL seconds=%.3f phase=%s programs_done=%llu\n", now()-began, snap.phase.c_str(),
                   (unsigned long long)snap.counts["programs_done"]); fflush(stdout);
        }
        if (snap.exited) final = snap;
        usleep(10000);
    }
    CHECK(requested && live && final.valid && final.exited && final.session == session);
    // A validation-only cancellation does not qualify the native GPU chain.
    CHECK(gpu || final.counts["gpu_vendor"] != 0);
    CHECK(final.state == "cancelled" && final.phase == "done" && final.code == 130 && !final.forced);
    CHECK(!activity_busy() && !game_pid && !cancel_precompile());
    CHECK(waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD);
    CHECK(access((state + "/logs/launcher-game.log").c_str(), F_OK) == -1 && errno == ENOENT);
    printf("PASS production-controller real-supervisor hidden-native cancellation code=%d valid=%d forced=%d live_gpu=%d seconds=%.3f built=%llu failed=%llu unsupported=%llu\n",
        final.code, final.valid, final.forced, gpu, now()-began,
        (unsigned long long)final.counts["built"], (unsigned long long)final.counts["failed"],
        (unsigned long long)final.counts["unsupported"]);
    return 0;
}
