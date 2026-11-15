// [XCDE] LINUX-009: Linux launcher filesystem/process helpers (no shell).
#pragma once
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <vector>
#include <string>
#include "../../runtime/paths.h"
#include "linux_update.h"
extern "C" {
#include "../../runtime/platform/linux_state_guard.h"
}
namespace launcher_linux {
static std::string project, game_project, input, game;
static bool smooth_motion_layer_available; // [XCDE] LINUX-097: host component probe, not a GPU compatibility claim
static bool smooth_motion_hint(const char *value) {
    // Production argument-value parser; no SDL/driver initialization.
    if (!value || (strcmp(value, "0") && strcmp(value, "1"))) return false;
    smooth_motion_layer_available = !strcmp(value, "1");
    return true;
}
static std::vector<std::string> test_environment;
static pid_t game_pid;
static pid_t precompile_pid;  // [XCDE] LINUX-018: one owned activity at a time
static bool fsr4_active, fsr4_models_ready;
static unsigned fsr4_request;
static std::string fsr4_message,fsr4_phase_value;
static bool fsr4_preparing(){return fsr4_active;}
static const char*fsr4_status(){return fsr4_message.c_str();}
static const char*fsr4_phase(){return fsr4_phase_value.c_str();}
static bool activity_busy() { return game_pid != 0 || precompile_pid != 0 || fsr4_active || update_applying(); }
static bool game_seen;
static int last_result;
static std::string error;
static bool canonical(const char *value, std::string &out, bool directory) {
    char resolved[PATH_MAX]; struct stat st;
    if (!value || value[0] != '/' || !realpath(value, resolved) || strcmp(value, resolved) ||
        stat(resolved, &st) || (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode))) return false;
    out = resolved; return true;
}
static bool beneath(const std::string &p, const std::string &root) {
    return p.size() > root.size() && p.compare(0, root.size(), root) == 0 && p[root.size()] == '/';
}
static bool output_path(const char *value, const std::string &root) {
    if (!value || *value != '/') return false;
    std::string p(value); auto slash = p.rfind('/');
    std::string parent, name = p.substr(slash + 1);
    return !name.empty() && name != "." && name != ".." &&
           canonical(p.substr(0, slash).c_str(), parent, true) &&
           (parent == root || beneath(parent, root));
}
static FILE *atomic_open(const std::string &destination, std::string &temporary) {
    temporary = destination + ".tmp.XXXXXX";
    int fd = mkostemp(temporary.data(), O_CLOEXEC);
    if (fd < 0) { error = strerror(errno); return nullptr; }
    FILE *f = fdopen(fd, "w");
    if (!f) { int e = errno; close(fd); unlink(temporary.c_str()); error = strerror(e); }
    return f;
}
static bool atomic_finish(FILE *f, const std::string &temporary, const std::string &destination) {
    bool ok = !ferror(f) && fflush(f) == 0 && fsync(fileno(f)) == 0;
    int e = errno;
    if (fclose(f) != 0) { ok = false; e = errno; }
    if (ok && rename(temporary.c_str(), destination.c_str()) != 0) { ok = false; e = errno; }
    if (ok) {
        std::string parent = destination.substr(0, destination.rfind('/'));
        int fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd < 0) { ok = false; e = errno; }
        else { if (fsync(fd)) { ok = false; e = errno; } close(fd); }
    }
    if (!ok) { unlink(temporary.c_str()); error = strerror(e ? e : EIO); }
    return ok;
}
static pid_t spawn(const std::vector<std::string> &args, const std::string &log_path, bool cancel_with_parent = false) {
    std::vector<char *> argv;
    for (const auto &s : args) argv.push_back(const_cast<char *>(s.c_str()));
    argv.push_back(nullptr);
    int errors[2];
    if (pipe2(errors, O_CLOEXEC)) { error = strerror(errno); return 0; }
    int log = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    int nullin = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (log < 0 || nullin < 0) {
        error = strerror(errno); if (log >= 0) close(log); if (nullin >= 0) close(nullin);
        close(errors[0]); close(errors[1]); return 0;
    }
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        // SDL may own threads: only async-signal-safe operations before exec.
        close(errors[0]);
        int e = 0;
        // [XCDE] LINUX-018: GUI death cancels only its compiler supervisor.
        // Gameplay retains the original independent lifetime.
        if (cancel_with_parent && (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() != parent)) e = ECANCELED;
        if (setpgid(0, 0) || dup2(nullin, STDIN_FILENO) < 0 || dup2(log, STDOUT_FILENO) < 0 || dup2(log, STDERR_FILENO) < 0) e = errno;
        close(log); close(nullin);
        if (!e) { execv(argv[0], argv.data()); e = errno; }
        (void)!write(errors[1], &e, sizeof e); _exit(127);
    }
    close(errors[1]); close(log); close(nullin);
    if (pid < 0) { error = strerror(errno); close(errors[0]); return 0; }
    // A CLOEXEC pipe distinguishes interpreter exec success from fork success.
    pollfd pfd{errors[0], POLLIN | POLLHUP, 0};
    int ready; do { ready = poll(&pfd, 1, 3000); } while (ready < 0 && errno == EINTR);
    int e = 0; ssize_t n = ready > 0 ? read(errors[0], &e, sizeof e) : -1;
    close(errors[0]);
    if (n != 0) {
        if (n < 0) { kill(pid, SIGKILL); error = "child exec handshake failed"; }
        else error = strerror(e);
        while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
        return 0;
    }
    return pid;
}
// [XCDE] LINUX-099: a host-owned, fixed-selection Play capability. A positive
// legacy game_pid is an owned child; -1 is a host-owned pending/running game.
static int game_fd = -1, game_last_activity = -1;
static bool game_broker_configured, game_broker_ready;
static unsigned game_request;
static std::string game_message;
static void game_disconnect(const char *reason) {
    if (game_fd >= 0) close(game_fd);
    game_fd = -1; game_broker_ready = false;
    game_message = reason;
    // An uncertain active game remains busy. Never launch a replacement blindly.
}
static bool game_send(const std::string &text) {
    if (game_fd < 0 || text.empty() || text.size() > 2048) return false;
    ssize_t n; do { n = send(game_fd, text.data(), text.size(), MSG_DONTWAIT | MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
    if (n == (ssize_t)text.size()) return true;
    game_disconnect("Host game launch connection unavailable. Reopen the launcher after closing the game.");
    return false;
}
static bool game_adopt_fd(int fd) {
    int type=0; socklen_t length=sizeof type; sockaddr_storage address{};socklen_t address_length=sizeof address;
    int flags=fcntl(fd,F_GETFD),status=fcntl(fd,F_GETFL);
    if(fd<3||flags<0||status<0||game_broker_configured||getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&length)||
        type!=SOCK_SEQPACKET||getsockname(fd,(sockaddr*)&address,&address_length)||address.ss_family!=AF_UNIX||
        fcntl(fd,F_SETFD,flags|FD_CLOEXEC)||fcntl(fd,F_SETFL,status|O_NONBLOCK))return false;
    game_fd=fd;game_broker_configured=true;
    return game_send("1\thello\t"+std::to_string(getpid()));
}
static bool game_number(const std::string &text,unsigned &out) {
    if(text.empty()||text.size()>10||text[0]=='0')return false;
    for(unsigned char c:text)if(c<'0'||c>'9')return false;
    char *end=nullptr;errno=0;unsigned long value=strtoul(text.c_str(),&end,10);
    if(errno||!end||*end||value>INT_MAX)return false;out=(unsigned)value;return true;
}
static bool game_receive(const std::string &text) {
    if(text.empty()||text.size()>2048)return false;
    for(unsigned char c:text)if((c<32&&c!='\t')||c==127)return false;
    std::vector<std::string> fields;
    for(size_t at=0;;){size_t end=text.find('\t',at);fields.push_back(text.substr(at,end==std::string::npos?end:end-at));if(fields.size()>5)return false;if(end==std::string::npos)break;at=end+1;}
    if(fields.size()<3||fields[0]!="1"||fields.back().size()>1024)return false;
    if(fields[1]=="ready"&&fields.size()==3&&!game_broker_ready){game_broker_ready=true;return true;}
    unsigned request=0;
    if(fields[1]=="fsr4"){
        if(fields.size()!=5||!game_number(fields[2],request)||request!=fsr4_request||fields[4].size()>512)return false;
        const auto &phase=fields[3];
        if(phase!="pending"&&phase!="running"&&phase!="ready"&&phase!="error"&&phase!="cancelled")return false;
        fsr4_active=phase=="pending"||phase=="running";
        if(phase=="ready")fsr4_models_ready=true;
        fsr4_phase_value=phase;fsr4_message=fields[4];return true;
    }
    if(!game_number(fields[2],request)||request!=game_request||game_pid!=-1)return false;
    if((fields[1]=="pending"||fields[1]=="running"||fields[1]=="error")&&fields.size()==4){
        if(fields[1]=="running")game_seen=true;
        if(fields[1]=="error"){game_pid=0;last_result=1;}
        game_message=fields[3];return true;
    }
    if(fields[1]=="exit"&&fields.size()==5){
        char *end=nullptr;errno=0;long code=strtol(fields[3].c_str(),&end,10);
        if(errno||!end||end==fields[3].c_str()||*end||code<0||code>255)return false;
        last_result=(int)code;if(!last_result&&!game_seen)last_result=1;
        game_pid=0;game_message=fields[4];return true;
    }
    return false;
}
static void game_broker_poll(void) {
    if(game_fd<0)return;
    int activity=precompile_pid!=0||fsr4_active||update_applying();
    if(game_last_activity!=activity){if(!game_send(std::string("1\tactivity\t")+(activity?"busy":"idle")))return;game_last_activity=activity;}
    for(unsigned i=0;i<16;i++){
        char bytes[2049];ssize_t n=recv(game_fd,bytes,sizeof bytes,MSG_DONTWAIT);
        if(n<0&&errno==EINTR){--i;continue;}
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;
        if(n<=0||n>2048||!game_receive(std::string(bytes,(size_t)n))){game_disconnect("Host game status connection closed. A running game is not stopped; close it before reopening the launcher.");return;}
    }
}
static bool request_fsr4_prepare(){
    if(activity_busy()){error="Finish the current game, update or preparation first.";return false;}
    if(!game_broker_ready||game_fd<0){error="Reopen the current launcher to prepare FSR4 Lite.";return false;}
    if(game_request>=INT_MAX){error="Launcher request limit reached. Reopen the launcher.";return false;}
    fsr4_request=++game_request;fsr4_active=true;fsr4_phase_value="pending";
    fsr4_message="Checking installed Proton and FSR4 preparation requirements...";
    if(!game_send("1\tprepare-fsr4\t"+std::to_string(fsr4_request))){fsr4_active=false;error=game_message;return false;}
    return true;
}
static bool cancel_fsr4_prepare(){
    if(!fsr4_active||game_fd<0){error="No FSR4 preparation is running.";return false;}
    if(!game_send("1\tcancel-fsr4\t"+std::to_string(fsr4_request))){error=game_message;return false;}
    fsr4_message="Cancelling FSR4 preparation and closing its private Proton process...";
    return true;
}
static bool start_game(const std::string &state, bool smooth_required=false) {
    if(activity_busy()){error="A game, shader precompiler or update is already running";return false;}
    if(game_broker_configured){
        if(game_fd<0||!game_broker_ready){error="Host game launch is unavailable. Reopen the AppImage or Play script.";return false;}
        if(game_request>=INT_MAX){error="Host game request limit reached. Reopen the launcher.";return false;}
        ++game_request;game_pid=-1;game_seen=false;last_result=1;
        if(!game_send("1\tplay\t"+std::to_string(game_request))){game_pid=0;error=game_message;return false;}
        return true;
    }
    if(smooth_required){error="NVIDIA Smooth Motion requires the host game launcher. Reopen the AppImage or Play script.";return false;}
    std::vector<std::string> args = {"/usr/bin/python3", "-B", (game_project.empty() ? project : game_project) + "/scripts/run_linux.py", "--executable", game,
        "--state-root", state, "--env", "XCDE_STRICT=1", "--env", "XCDE_NO_ERROR_DIALOG=1", "--env", "XCDE_INI=" + state + "/xcde.ini"};
    for(const auto &entry:test_environment){args.push_back("--env");args.push_back(entry);}
    game_pid=spawn(args,state+"/logs/launcher-game.log");game_seen=false;last_result=1;
    return game_pid!=0;
}

// [XCDE] LINUX-017: bwrap retains monitor/init processes while its game runs.
// Observe only descendants of our unreaped child, with pinned proc directory FDs
// and an exact ELF path match. Fork/exec success alone is never "game running".
static bool proc_parent_is(int process, pid_t expected) {
    if (!expected) return true;  // owned direct child is established by waitpid
    int fd = openat(process, "status", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    FILE *file = fdopen(fd, "r");
    if (!file) { close(fd); return false; }
    char line[256]; bool match = false;
    for (unsigned i = 0; i < 128 && fgets(line, sizeof line, file); i++) {
        long parent;
        if (sscanf(line, "PPid:%ld", &parent) == 1) { match = parent == expected; break; }
    }
    fclose(file);
    return match;
}
struct ProcessProbe { pid_t pid, parent; unsigned depth; };
static bool process_tree_has_game(pid_t root, const std::string &expected) {
    constexpr size_t process_limit = 64;
    constexpr unsigned depth_limit = 8, thread_limit = 64;
    if (root <= 0 || expected.empty()) return false;
    std::vector<ProcessProbe> pending = {{root, 0, 0}};
    for (size_t next = 0; next < pending.size() && next < process_limit; next++) {
        ProcessProbe probe = pending[next];
        std::string path = "/proc/" + std::to_string(probe.pid);
        int process = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (process < 0) continue;
        if (!proc_parent_is(process, probe.parent)) { close(process); continue; }
        char executable[PATH_MAX];
        ssize_t n = readlinkat(process, "exe", executable, sizeof executable - 1);
        if (n > 0 && n < (ssize_t)sizeof executable - 1) {
            executable[n] = 0;
            if (expected == executable) { close(process); return true; }
        }
        if (probe.depth >= depth_limit || pending.size() >= process_limit) { close(process); continue; }
        int tasks_fd = openat(process, "task", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        DIR *tasks = tasks_fd < 0 ? nullptr : fdopendir(tasks_fd);
        if (!tasks) { if (tasks_fd >= 0) close(tasks_fd); close(process); continue; }
        unsigned threads = 0;
        while (dirent *entry = readdir(tasks)) {
            char *end;
            long tid = strtol(entry->d_name, &end, 10);
            if (*end || tid <= 0 || tid > INT_MAX) continue;
            if (++threads > thread_limit || pending.size() >= process_limit) break;
            std::string children = std::string(entry->d_name) + "/children";
            int fd = openat(dirfd(tasks), children.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            FILE *file = fdopen(fd, "r");
            if (!file) { close(fd); continue; }
            long child;
            while (pending.size() < process_limit && fscanf(file, "%ld", &child) == 1) {
                if (child <= 0 || child > INT_MAX) continue;
                bool duplicate = false;
                for (const auto &known : pending) duplicate |= known.pid == child;
                if (!duplicate) pending.push_back({(pid_t)child, probe.pid, probe.depth + 1});
            }
            fclose(file);
        }
        closedir(tasks);
        close(process);
    }
    return false;
}
static std::string poll_game() {
    if(game_broker_configured){game_broker_poll();std::string result=game_message;game_message.clear();return result;}
    if (!game_pid) return {};
    int status = 0; pid_t done = waitpid(game_pid, &status, WNOHANG);
    if (done < 0 && errno == EINTR) return {};
    if (done == 0) {
        if (!game_seen && process_tree_has_game(game_pid, game)) {
            game_seen = true;
            return "Game running. Close its window to return to settings.";
        }
        return {};
    }
    bool seen = game_seen; game_pid = 0;
    if (done < 0) return "Could not reap game process: " + std::string(strerror(errno));
    last_result = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
    if (last_result == 0 && seen) return "Game closed. Settings are available.";
    if (!last_result) last_result = 1; // zero without an observed game exec is not a launch pass
    std::string reason = WIFEXITED(status) ? "exit " + std::to_string(WEXITSTATUS(status)) :
                         WIFSIGNALED(status) ? "signal " + std::to_string(WTERMSIG(status)) : "unknown status";
    return "Game stopped (" + reason + "). See logs/launcher-game.log and logs/monado.log.";
}
static bool copy_file(const std::string &from, const std::string &to) {
    int in = open(from.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW); if (in < 0) return false;
    struct stat st; if (fstat(in, &st) || !S_ISREG(st.st_mode)) { close(in); errno = EINVAL; return false; }
    int out = open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out < 0) { close(in); return false; }
    char buffer[16384]; bool ok = true;
    for (;;) {
        ssize_t n = read(in, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = n == 0; break; }
        for (ssize_t at = 0; at < n;) { ssize_t wrote = write(out, buffer + at, n - at); if (wrote < 0 && errno == EINTR) continue;
            if (wrote <= 0) { ok = false; break; } at += wrote; }
        if (!ok) break;
    }
    if (ok && fsync(out)) ok = false;
    if (close(out)) ok = false; close(in); return ok;
}
} // namespace launcher_linux

namespace launcher_linux {
// Bounded tests use the same SDL events and ImGui handlers as real input.
struct UiEvent { unsigned frame; std::string action; int a, b; };
static std::vector<UiEvent> events;
static bool load_events(const char *filename) {
    FILE *f = fopen(filename, "r"); if (!f) { error = strerror(errno); return false; }
    char line[256]; unsigned previous = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        unsigned frame = 0; char action[24] = {}, extra; int a = 0, b = 0;
        int n = sscanf(line, "%u %23s %d %d %c", &frame, action, &a, &b, &extra);
        bool valid = frame >= previous && frame <= 10000 && events.size() < 4096;
        if (!strcmp(action, "motion")) valid &= n == 4 && a >= 0 && b >= 0 && a < 8192 && b < 8192;
        else if (!strcmp(action, "down") || !strcmp(action, "up")) valid &= n == 3 && a >= 1 && a <= 5;
        else if (!strcmp(action, "key-down") || !strcmp(action, "key-up")) valid &= n == 3 && a > 0 && a < SDL_SCANCODE_COUNT;
        else if (!strcmp(action, "quit")) valid &= n == 2;
        else valid = false;
        if (!valid) { fclose(f); error = "Invalid UI script (frame motion x y | down/up button | key-down/key-up scancode | quit)"; return false; }
        events.push_back({frame, action, a, b}); previous = frame;
    }
    bool ok = !ferror(f); fclose(f); if (!ok) error = "Cannot read UI script"; return ok;
}
static bool push_events(unsigned frame, SDL_Window *window) {
    static size_t next; static float x, y;
    while (next < events.size() && events[next].frame <= frame) {
        const UiEvent &in = events[next++]; SDL_Event e{};
        if (in.action == "motion") {
            x = in.a; y = in.b; SDL_WarpMouseInWindow(window, x, y);
            e.type = SDL_EVENT_MOUSE_MOTION; e.motion.windowID = SDL_GetWindowID(window); e.motion.x = x; e.motion.y = y;
        } else if (in.action == "down" || in.action == "up") {
            e.type = in.action == "down" ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.windowID = SDL_GetWindowID(window); e.button.button = in.a; e.button.down = in.action == "down";
            e.button.x = x; e.button.y = y;
        } else if (in.action == "key-down" || in.action == "key-up") {
            e.type = in.action == "key-down" ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            e.key.windowID = SDL_GetWindowID(window); e.key.scancode = (SDL_Scancode)in.a; e.key.down = in.action == "key-down";
            e.key.key = SDL_GetKeyFromScancode(e.key.scancode, SDL_KMOD_NONE, false);
        } else e.type = SDL_EVENT_QUIT;
        if (!SDL_PushEvent(&e)) { error = SDL_GetError(); return false; }
    }
    return true;
}
} // namespace launcher_linux
