// Authored LINUX-027 IPC fixture: no SDL, game, vendor library or network.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/wait.h>
#include "../../Monado_Linux/tools/launcher/linux_update.h"
extern "C" {
#include "../../Monado_Linux/runtime/platform/linux_state_guard.h"
}
using namespace launcher_linux;
static std::string receive(int fd) {
    char bytes[8193]; ssize_t n = recv(fd, bytes, sizeof bytes, 0);
    assert(n > 0 && n <= 8192); return std::string(bytes, (size_t)n);
}
static void provide(int fd, const std::string &text) {
    assert(send(fd, text.data(), text.size(), MSG_NOSIGNAL) == (ssize_t)text.size());
}
int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--exec-probe")) {
        errno = 0; assert(fcntl(atoi(argv[2]), F_GETFD) == -1 && errno == EBADF);
        assert(fcntl(atoi(argv[3]), F_GETFD) >= 0);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "--peer")) {
        if (!update_adopt_fd(atoi(argv[2]))) return 2;
        for (int i = 4; i < argc; i += 2) {
            if (i + 1 >= argc) return 2;
            if (!strcmp(argv[i], "--lock-fd")) { if (fcntl(atoi(argv[i + 1]), F_GETFD) < 0) return 2; }
            else if (strcmp(argv[i], "--update-fd") || atoi(argv[i + 1]) != update_fd) return 2;
        }
        char error[1024];
        if (!rt_linux_state_guard(argv[3], error, sizeof error)) { fprintf(stderr, "%s\n", error); return 77; }
        for (unsigned i = 0; i < 2000; ++i) {
            update_poll(false);
            if (update_snapshot.offered && !update_apply()) return 3;
            if (update_snapshot.ready) return 75;
            if (update_snapshot.failed) return 4;
            usleep(1000);
        }
        return 5;
    }
    assert(argc == 1);
    int fds[2]; assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) == 0);
    assert(!update_adopt_fd(STDIN_FILENO));
    assert(update_adopt_fd(fds[1]));
    assert(receive(fds[0]) == "1\thello\t" + std::to_string(getpid()));
    assert(fcntl(fds[1], F_GETFD) & FD_CLOEXEC);
    int lease = open("/dev/null", O_RDONLY); assert(lease >= 3);
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        std::string channel = std::to_string(fds[1]), lock = std::to_string(lease);
        execl("/proc/self/exe", argv[0], "--exec-probe", channel.c_str(), lock.c_str(), nullptr); _exit(127);
    }
    int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status)); close(lease);
    update_poll(false);
    assert(receive(fds[0]) == "1\tactivity\tidle"); assert(receive(fds[0]) == "1\tcheck");
    assert(!update_receive("2\toffer\tid\tv2\t1\twrong version"));
    assert(!update_receive("1\toffer\t../path\tv2\t1\tbad identifier"));
    assert(!update_receive("1\terror\tbad\ntext"));
    assert(!update_receive(std::string(8193, 'a')));
    provide(fds[0], "1\toffer\tstable-2\tv0.2.0\t1\tA stable update is available.");
    update_poll(true);
    assert(receive(fds[0]) == "1\tactivity\tbusy");
    assert(update_snapshot.offered && !update_apply());
    update_poll(false); assert(receive(fds[0]) == "1\tactivity\tidle");
    assert(update_apply() && update_applying() && !update_apply());
    assert(receive(fds[0]) == "1\tapply\tstable-2");
    provide(fds[0], "1\tprogress\tbuilding\t0.25\tBuilding authored components");
    update_poll(false); assert(update_snapshot.fraction == .25f);
    assert(!update_receive("1\tprogress\tbuilding\tnan\tbad"));
    update_cancel(); assert(receive(fds[0]) == "1\tcancel");
    provide(fds[0], "1\tcancelled\tCurrent version retained"); update_poll(false);
    assert(!update_applying() && !update_snapshot.offered);
    provide(fds[0], "1\toffer\tstable-2\tv0.2.0\t0\tLauncher update"); update_poll(false);
    update_defer(); assert(receive(fds[0]) == "1\tdefer" && !update_snapshot.offered);
    provide(fds[0], "1\toffer\tstable-2\tv0.2.0\t0\tLauncher update"); update_poll(false);
    assert(update_apply()); assert(receive(fds[0]) == "1\tapply\tstable-2");
    provide(fds[0], "1\tready\tReady for a safe restart"); update_poll(false);
    assert(update_snapshot.ready && update_applying());
    close(fds[0]); update_poll(false); assert(!update_snapshot.enabled && !update_applying());
    puts("PASS bounded IPC, offer/busy/apply/cancel/defer/ready, malformed controls, and exec FD isolation");
}
