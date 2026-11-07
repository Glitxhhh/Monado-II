// [XCDE] LINUX-009: synthetic checks of actual Linux launcher helpers.
// No GUI, game files, game execution or policy changes.
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>
#include "linux_host.h"
using namespace launcher_linux;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s (%s)\n", __LINE__, #x, strerror(errno)); abort(); } } while (0)
static std::string contents(const std::string &name) {
    FILE *f = fopen(name.c_str(), "r"); CHECK(f); std::string out; char buffer[256];
    size_t n; while ((n = fread(buffer, 1, sizeof buffer, f))) out.append(buffer, n);
    CHECK(!ferror(f)); CHECK(!fclose(f)); return out;
}
int main(int argc, char **argv) {
    CHECK(argc == 2); std::string root = argv[1]; CHECK(mkdir(root.c_str(), 0700) == 0);
    std::string ini = root + "/xcde.ini", temporary;
    FILE *f = atomic_open(ini, temporary); CHECK(f); CHECK(fputs("original\n", f) >= 0); CHECK(atomic_finish(f, temporary, ini));
    CHECK(contents(ini) == "original\n");
    rlimit old; CHECK(getrlimit(RLIMIT_FSIZE, &old) == 0); rlimit small{128, old.rlim_max};
    auto previous = signal(SIGXFSZ, SIG_IGN); CHECK(previous != SIG_ERR); CHECK(setrlimit(RLIMIT_FSIZE, &small) == 0);
    f = atomic_open(ini, temporary); CHECK(f);
    for (unsigned i = 0; i < 1024; ++i) fputc('X', f);
    CHECK(!atomic_finish(f, temporary, ini));
    CHECK(setrlimit(RLIMIT_FSIZE, &old) == 0); signal(SIGXFSZ, previous);
    CHECK(contents(ini) == "original\n"); CHECK(access(temporary.c_str(), F_OK) < 0 && errno == ENOENT);
    f = atomic_open(ini, temporary); CHECK(f); CHECK(fputs("replacement\n", f) >= 0); CHECK(atomic_finish(f, temporary, ini));
    CHECK(contents(ini) == "replacement\n");
    CHECK(copy_file(ini, root + "/report.ini")); CHECK(contents(root + "/report.ini") == "replacement\n");
    CHECK(!copy_file(ini, root + "/report.ini")); CHECK(contents(root + "/report.ini") == "replacement\n");
    CHECK(!spawn({"/definitely-missing-monado-test-executable"}, root + "/exec-error.log"));
    CHECK(waitpid(-1, nullptr, WNOHANG) < 0 && errno == ECHILD);
    pid_t p = spawn({"/usr/bin/python3", "-B", "-c", "import sys; print(repr(sys.argv[1:]))", "a 'quoted' $HOME; $(false)", "two words"}, root + "/argv.log");
    CHECK(p > 0); int status; CHECK(waitpid(p, &status, 0) == p && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(contents(root + "/argv.log") == "[\"a 'quoted' $HOME; $(false)\", 'two words']\n");
    p = spawn({"/usr/bin/false"}, root + "/nonzero.log"); CHECK(p > 0);
    CHECK(waitpid(p, &status, 0) == p && WIFEXITED(status) && WEXITSTATUS(status) == 1);
    CHECK(waitpid(-1, nullptr, WNOHANG) < 0 && errno == ECHILD);
    puts("launcher helpers: atomic replacement/failure preservation, unique report copy, argv quoting, exec errors and child reaping OK");
}
