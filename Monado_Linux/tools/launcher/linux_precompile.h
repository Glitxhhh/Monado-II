// [XCDE] LINUX-018: native launcher ownership and strict supervisor handshake.
// Included after linux_host.h. No shell, automatic Play or automatic retry.
#pragma once
#include <map>
#include <sys/random.h>

namespace launcher_linux {
struct PrecompileSnapshot {
    bool valid = false, exited = false, forced = false;
    int code = 1;
    std::string session, state = "starting", phase = "validating", message;
    std::map<std::string, uint64_t> counts;
};
static PrecompileSnapshot precompile_snapshot;
static std::string precompile_state_root, precompile_session;
static bool precompile_cancelling;
static bool hex_string(const std::string &text, size_t size) {
    if (text.size() != size) return false;
    for (char c : text) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
static bool start_precompile(const std::string &state) {
    if (activity_busy()) { error = "A game or shader precompiler is already running"; return false; }
    unsigned char bytes[16]; size_t done = 0;
    while (done < sizeof bytes) {
        ssize_t n = getrandom(bytes + done, sizeof bytes - done, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { error = "Cannot create a fresh precompile session"; return false; }
        done += (size_t)n;
    }
    const char hex[] = "0123456789abcdef";
    precompile_session.clear();
    for (unsigned char b : bytes) { precompile_session += hex[b >> 4]; precompile_session += hex[b & 15]; }
    std::vector<std::string> args = {"/usr/bin/python3", "-B", project + "/scripts/precompile_linux.py",
        "--executable", game, "--state-root", state, "--session", precompile_session};
    precompile_pid = spawn(args, state + "/logs/launcher-precompile.log", true);
    if (!precompile_pid) return false;
    precompile_state_root = state;
    precompile_cancelling = false;
    precompile_snapshot = {};
    precompile_snapshot.session = precompile_session;
    precompile_snapshot.message = "Checking game files and build...";
    return true;
}
static bool precompile_read_status(PrecompileSnapshot &out, pid_t owned_pid) {
    const auto path = precompile_state_root + "/cache/precompile_status.txt";
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size <= 0 || st.st_size > 8192) { close(fd); return false; }
    std::string text((size_t)st.st_size, '\0'); size_t done = 0;
    while (done < text.size()) {
        ssize_t n = read(fd, &text[done], text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return false; }
        done += (size_t)n;
    }
    close(fd);
    if (text.back() != '\n' || text.find('\0') != std::string::npos) return false;
    const std::string header = "monado_precompile_status 1\n";
    if (text.compare(0, header.size(), header)) return false;
    std::map<std::string, std::string> fields;
    for (size_t pos = header.size(); pos < text.size();) {
        size_t end = text.find('\n', pos), space = text.find(' ', pos);
        if (end == std::string::npos || space == std::string::npos || space >= end || space == pos) return false;
        if (!fields.emplace(text.substr(pos, space - pos), text.substr(space + 1, end - space - 1)).second) return false;
        pos = end + 1;
    }
    if (fields["session"] != precompile_session || !hex_string(fields["session"], 32) ||
        !hex_string(fields["build"], 64) || !hex_string(fields["identity"], 64)) return false;
    auto number = [&](const char *key, uint64_t &value) {
        auto it = fields.find(key);
        if (it == fields.end() || it->second.empty() || it->second.size() > 20) return false;
        for (char c : it->second) if (c < '0' || c > '9') return false;
        errno = 0; char *end = nullptr;
        value = strtoull(it->second.c_str(), &end, 10);
        return !errno && end && !*end;
    };
    uint64_t process, forced;
    if (!number("supervisor_pid", process) || process != (uint64_t)owned_pid || !number("forced", forced) || forced > 1) return false;
    const char *states[] = {"starting", "running", "cancelling", "complete", "partial", "cancelled", "failed"};
    const char *phases[] = {"validating", "scan", "build", "seed", "pipelines", "done"};
    bool state_ok = false, phase_ok = false;
    for (auto s : states) state_ok |= fields["state"] == s;
    for (auto p : phases) phase_ok |= fields["phase"] == p;
    if (!state_ok || !phase_ok || fields["message"].size() > 512) return false;
    for (unsigned char c : fields["message"]) if (c < 32 || c == 127) return false;
    const char *counters[] = {"files_total", "files_done", "shaders", "programs", "programs_done", "seed_total", "seed_done",
        "variants_total", "variants_done", "attempted", "built", "failed", "skipped", "unsupported", "errors",
        "gpu_vendor", "gpu_device", "gpu_driver"};
    PrecompileSnapshot snapshot;
    for (auto counter : counters) { uint64_t n; if (!number(counter, n)) return false; snapshot.counts[counter] = n; }
    const char *done_keys[] = {"files_done", "programs_done", "seed_done", "variants_done"};
    const char *total_keys[] = {"files_total", "programs", "seed_total", "variants_total"};
    for (unsigned i = 0; i < 4; i++) if (snapshot.counts[done_keys[i]] > snapshot.counts[total_keys[i]]) return false;
    if (!hex_string(fields["gpu_cache_uuid"], 32)) return false;
    snapshot.valid = true; snapshot.session = precompile_session; snapshot.state = fields["state"];
    snapshot.phase = fields["phase"]; snapshot.message = fields["message"]; snapshot.forced = forced != 0;
    out = snapshot;
    return true;
}
static bool cancel_precompile() {
    if (!precompile_pid) return false;
    if (!precompile_cancelling && kill(precompile_pid, SIGTERM) && errno != ESRCH) { error = strerror(errno); return false; }
    precompile_cancelling = true;
    return true;
}
static PrecompileSnapshot poll_precompile() {
    if (!precompile_pid) return precompile_snapshot;
    pid_t pid = precompile_pid;
    precompile_read_status(precompile_snapshot, pid);
    int status = 0; pid_t done = waitpid(pid, &status, WNOHANG);
    if (!done || (done < 0 && errno == EINTR)) return precompile_snapshot;
    // Read after the observed exit too: the atomic final status precedes exit.
    bool final_valid = precompile_read_status(precompile_snapshot, pid);
    if (!final_valid) precompile_snapshot.valid = false;
    precompile_pid = 0;
    auto &snapshot = precompile_snapshot;
    snapshot.exited = true;
    snapshot.code = done < 0 ? 1 : WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
    bool complete = snapshot.valid && snapshot.state == "complete" && snapshot.phase == "done" && snapshot.code == 0 &&
                    !snapshot.forced && !snapshot.counts["failed"] && !snapshot.counts["skipped"] && !snapshot.counts["errors"] &&
                    snapshot.counts["files_done"] == snapshot.counts["files_total"] &&
                    snapshot.counts["programs_done"] == snapshot.counts["programs"] &&
                    snapshot.counts["seed_done"] == snapshot.counts["seed_total"] &&
                    snapshot.counts["variants_done"] == snapshot.counts["variants_total"];
    bool partial = snapshot.valid && snapshot.state == "partial" && snapshot.phase == "done" && snapshot.code == 2;
    bool cancelled = (snapshot.valid && snapshot.state == "cancelled" && snapshot.phase == "done" && snapshot.code == 130) ||
                     (precompile_cancelling && !snapshot.valid);
    if (complete) snapshot.message = "Shader preparation finished: " + std::to_string(snapshot.counts["built"]) +
        " built, " + std::to_string(snapshot.counts["unsupported"]) + " not precompiled. Remaining variants compile during play.";
    else if (partial) snapshot.message = "Incomplete: " + std::to_string(snapshot.counts["built"]) + " built; " +
        std::to_string(snapshot.counts["failed"]) + " failed, " + std::to_string(snapshot.counts["errors"]) + " errors, " +
        std::to_string(snapshot.counts["skipped"]) + " skipped, " + std::to_string(snapshot.counts["unsupported"]) +
        " not precompiled. Caches kept; see logs/launcher-precompile.log.";
    else if (cancelled) { snapshot.state = "cancelled"; snapshot.message = snapshot.forced ?
        "Shader preparation cancelled; compiler required forced termination. Valid caches are kept." :
        "Shader preparation cancelled. Valid caches are kept."; }
    else { snapshot.state = "failed"; snapshot.message = "Shader preparation failed. See logs/launcher-precompile.log."; }
    return snapshot;
}
}  // namespace launcher_linux
