// [XCDE] LINUX-018: actual native precompile controller, authored fake children.
// Headers only: no SDL init/link, window, real game input, gameplay or profile.
#include <SDL3/SDL.h>
#include "../../tools/launcher/linux_host.h"
#include "../../tools/launcher/linux_precompile.h"
#include <ctime>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s errno=%d (%s)\n", __LINE__, #x, errno, launcher_linux::error.c_str()); exit(1); } } while (0)
using namespace launcher_linux;

static double now() {
    timespec time{}; CHECK(!clock_gettime(CLOCK_MONOTONIC, &time));
    return time.tv_sec + time.tv_nsec / 1e9;
}
static bool exists(const std::string &path) { return access(path.c_str(), F_OK) == 0; }
static void touch(const std::string &path, const std::string &text = "authored fixture\n") {
    FILE *f = fopen(path.c_str(), "w"); CHECK(f);
    CHECK(fwrite(text.data(), 1, text.size(), f) == text.size() && !fclose(f));
}
static void no_children() { CHECK(waitpid(-1, nullptr, WNOHANG) == -1 && errno == ECHILD); }

int main(int argc, char **argv) {
    CHECK(argc == 4);
    std::string mode = argv[1], state;
    CHECK(canonical(argv[2], project, true) && canonical(argv[3], state, true));
    game = project + "/authored-game-never-executed";
    char guard_error[512]; CHECK(rt_linux_state_guard(state.c_str(), guard_error, sizeof guard_error));
    CHECK(!activity_busy() && !precompile_pid && !game_pid);

    if (mode == "game-busy") {
        pid_t child = fork(); CHECK(child >= 0);
        if (!child) { while (true) pause(); }
        game_pid = child;
        CHECK(!start_precompile(state) && !precompile_pid && game_pid == child);
        CHECK(error.find("already running") != std::string::npos);
        CHECK(!kill(child, SIGTERM)); int status;
        CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
        game_pid = 0; CHECK(!activity_busy()); no_children();
        puts("PASS game-busy rejects compiler; authored owned child reaped"); return 0;
    }
    if (mode == "parent-death") {
        CHECK(!prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0));
        pid_t holder = fork(); CHECK(holder >= 0);
        if (!holder) {
            CHECK(start_precompile(state));
            touch(state + "/holder-ready", std::to_string(precompile_pid));
            while (true) pause();
        }
        double deadline = now() + 4;
        while (!exists(state + "/holder-ready") || !exists(state + "/fake-ready")) { CHECK(now() < deadline); usleep(1000); }
        FILE *f = fopen((state + "/holder-ready").c_str(), "r"); CHECK(f);
        int supervisor = 0; CHECK(fscanf(f, "%d", &supervisor) == 1 && supervisor > 0 && !fclose(f));
        CHECK(!kill(holder, SIGKILL)); int status;
        CHECK(waitpid(holder, &status, 0) == holder && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        pid_t done = 0;
        while ((done = waitpid(supervisor, &status, WNOHANG)) == 0) { CHECK(now() < deadline); usleep(1000); }
        CHECK(done == supervisor && WIFEXITED(status) && WEXITSTATUS(status) == 130);
        CHECK(exists(state + "/parent-cancelled")); no_children();
        puts("PASS parent-death: actual spawn cancel_with_parent delivered SIGTERM to authored supervisor"); return 0;
    }

    CHECK(start_precompile(state) && activity_busy() && precompile_pid > 0);
    pid_t owned = precompile_pid;
    CHECK(hex_string(precompile_session, 32));
    const std::string session = precompile_session;
    CHECK(!start_precompile(state) && precompile_pid == owned && precompile_session == session);
    CHECK(!start_game(state) && !game_pid && precompile_pid == owned);
    CHECK(!exists(state + "/PLAY_WAS_LAUNCHED"));

    const bool cancel = mode == "cancel" || mode == "cancel-forced";
    const bool no_final = mode == "no-final-record";
    bool observed_before_exit = false, requested = false;
    double deadline = now() + 4;
    PrecompileSnapshot final;
    while (precompile_pid) {
        CHECK(now() < deadline);
        auto snapshot = poll_precompile();
        if (snapshot.valid && !snapshot.exited) {
            observed_before_exit = true;
            if (cancel && snapshot.state == "running" && !requested) {
                CHECK(cancel_precompile() && cancel_precompile()); requested = true;
            }
            if (no_final && !requested) { touch(state + "/release"); requested = true; }
        }
        if (snapshot.exited) final = snapshot;
        usleep(1000);
    }
    CHECK(final.exited && final.session == session && !activity_busy());
    CHECK(!cancel_precompile() && !game_pid && !exists(state + "/PLAY_WAS_LAUNCHED"));
    no_children();
    if (mode == "complete") {
        CHECK(final.valid && final.state == "complete" && final.code == 0 && !final.forced);
        CHECK(final.counts["built"] == 6 && final.counts["unsupported"] == 2);
        CHECK(final.message.find("6 built") != std::string::npos && final.message.find("2 not precompiled") != std::string::npos);
    } else if (mode == "partial" || mode == "partial-errors") {
        CHECK(final.valid && final.state == "partial" && final.code == 2);
        CHECK(final.counts["built"] == 4 && final.counts["failed"] == (mode == "partial" ? 1u : 0u) && final.counts["unsupported"] == 3);
        CHECK(final.message.find(mode == "partial" ? "1 failed" : "0 failed, 2 errors") != std::string::npos &&
              final.message.find("3 not precompiled") != std::string::npos && final.message.find("Incomplete") != std::string::npos);
    } else if (cancel) {
        CHECK(requested && observed_before_exit && final.valid && final.state == "cancelled" && final.code == 130);
        CHECK(final.forced == (mode == "cancel-forced"));
        CHECK(final.message.find("cancelled") != std::string::npos);
        if (final.forced) CHECK(final.message.find("forced termination") != std::string::npos);
    } else {
        CHECK(final.state == "failed" && final.message.find("failed") != std::string::npos);
        if (mode == "fatal") CHECK(final.valid && final.code == 7);
        else CHECK(final.code == 0);
        if (no_final) CHECK(observed_before_exit && !final.valid);
    }
    printf("PASS %s state=%s code=%d valid=%d forced=%d observed_live=%d built=%llu unsupported=%llu\n",
        mode.c_str(), final.state.c_str(), final.code, final.valid, final.forced, observed_before_exit,
        (unsigned long long)final.counts["built"], (unsigned long long)final.counts["unsupported"]);
    return 0;
}
