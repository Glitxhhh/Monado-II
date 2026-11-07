// [XCDE] LINUX-017: authored process/namespace fixture; no game, SDK or UI.
#include <SDL3/SDL.h>
#include "../../tools/launcher/linux_host.h"
#include <ctime>

static double monotonic() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--leaf")) {
        puts("LEAF_READY authored non-game process"); fflush(stdout);
        usleep(400000);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "--fd-probe")) {
        int fd = atoi(argv[2]), expected = atoi(argv[3]);
        bool inherited = fcntl(fd, F_GETFD) >= 0;
        struct stat st{}; char path[PATH_MAX] = {};
        if (inherited) {
            if (fstat(fd, &st)) return 3;
            std::string link = "/proc/self/fd/" + std::to_string(fd);
            ssize_t n = readlink(link.c_str(), path, sizeof path - 1);
            if (n > 0) path[n] = 0;
        }
        printf("FD_PROBE fd=%d inherited=%d expected=%d dev=%llu ino=%llu path=%s\n", fd, inherited, expected,
               (unsigned long long)st.st_dev, (unsigned long long)st.st_ino, path); fflush(stdout);
        usleep(400000);
        return inherited == (expected != 0) ? 0 : 3;
    }
    if (argc != 3 || (strcmp(argv[1], "--direct") && strcmp(argv[1], "--mismatch") && strcmp(argv[1], "--namespace"))) {
        fprintf(stderr, "usage: fixture --direct|--mismatch|--namespace PRIVATE_STATE\n");
        return 2;
    }
    std::string state;
    if (!launcher_linux::canonical(argv[2], state, true)) return 2;
    char self[PATH_MAX];
    if (!realpath(argv[0], self)) return 2;
    char error[1024];
    if (!rt_linux_state_guard(state.c_str(), error, sizeof error)) {
        fprintf(stderr, "state guard: %s\n", error); return 2;
    }
    bool mismatch = !strcmp(argv[1], "--mismatch");
    bool ns = !strcmp(argv[1], "--namespace");
    launcher_linux::game = mismatch ? "/not-the-selected-fixture" : self;
    std::vector<std::string> command;
    if (ns) {
        command = {"/usr/bin/bwrap", "--unshare-user", "--unshare-pid", "--unshare-net",
                   "--ro-bind", "/", "/", "--dev-bind", "/dev", "/dev", "--proc", "/proc",
                   "--bind", state, state, "--bind", state, "/tmp"};
        if (access("/usr/bin/nvidia-ngx-updater", F_OK) == 0)
            command.insert(command.end(), {"--ro-bind", "/dev/null", "/usr/bin/nvidia-ngx-updater"});
        command.push_back("--");
    }
    command.insert(command.end(), {"/usr/bin/python3", "-B", "-c",
        "import subprocess,sys,time; time.sleep(.1); p=subprocess.Popen(sys.argv[1:]); raise SystemExit(p.wait())",
        self, "--leaf"});
    launcher_linux::game_pid = launcher_linux::spawn(command, state + "/process.log");
    launcher_linux::game_seen = false;
    launcher_linux::last_result = 1;
    if (!launcher_linux::game_pid) { fprintf(stderr, "%s\n", launcher_linux::error.c_str()); return 2; }
    pid_t owned = launcher_linux::game_pid;
    double deadline = monotonic() + 5;
    bool seen = false;
    std::string final;
    while (launcher_linux::game_pid && monotonic() < deadline) {
        std::string message = launcher_linux::poll_game();
        if (launcher_linux::game_seen) seen = true;
        if (!message.empty()) { puts(message.c_str()); final = message; }
        usleep(1000);
    }
    if (launcher_linux::game_pid) {
        kill(-owned, SIGKILL);
        while (waitpid(owned, nullptr, 0) < 0 && errno == EINTR) {}
        fprintf(stderr, "owned fixture process exceeded five seconds\n");
        return 2;
    }
    if (mismatch) {
        if (seen || !launcher_linux::last_result) return 3;
    } else if (!seen || launcher_linux::last_result || final != "Game closed. Settings are available.") {
        fprintf(stderr, "fixture not observed/cleanly reaped: seen=%d result=%d\n", seen, launcher_linux::last_result);
        return 3;
    }
    printf("PROCESS_FIXTURE namespace=%d mismatch=%d observed=%d result=%d\n", ns, mismatch, seen, launcher_linux::last_result);
    return 0;
}
