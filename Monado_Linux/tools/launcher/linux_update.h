// [XCDE] LINUX-027: bounded capability IPC to the host update supervisor.
// No networking, filesystem update, shell, or game lifetime control lives here.
#pragma once
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

namespace launcher_linux {
struct UpdateSnapshot {
    bool enabled = false, offered = false, applying = false, ready = false, rebuild = false;
    bool failed = false, cancelling = false;
    float fraction = -1;
    unsigned revision = 0;
    std::string id, version, phase, message;
};
static UpdateSnapshot update_snapshot;
static int update_fd = -1, update_last_activity = -1;
static bool update_checked;
static bool update_applying() { return update_snapshot.applying; }
static void update_disconnect(const char *message) {
    if (update_fd >= 0) close(update_fd);
    update_fd = -1;
    update_snapshot.enabled = update_snapshot.offered = update_snapshot.applying = update_snapshot.ready = false;
    update_snapshot.failed = true;
    update_snapshot.message = message;
    ++update_snapshot.revision;
}
static bool update_send(const std::string &text) {
    if (update_fd < 0 || text.empty() || text.size() > 8192) return false;
    ssize_t n;
    do { n = send(update_fd, text.data(), text.size(), MSG_DONTWAIT | MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
    if (n == (ssize_t)text.size()) return true;
    update_disconnect("Update connection unavailable. The current version is still usable.");
    return false;
}
static bool update_adopt_fd(int fd) {
    int type = 0; socklen_t length = sizeof type;
    sockaddr_storage address{}; socklen_t address_length = sizeof address;
    int flags = fcntl(fd, F_GETFD), status = fcntl(fd, F_GETFL);
    if (fd < 3 || flags < 0 || status < 0 || getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &length) ||
        type != SOCK_SEQPACKET || getsockname(fd, (sockaddr *)&address, &address_length) || address.ss_family != AF_UNIX ||
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC) || fcntl(fd, F_SETFL, status | O_NONBLOCK)) return false;
    update_fd = fd;
    update_snapshot.enabled = true;
    return update_send("1\thello\t" + std::to_string(getpid()));
}
static bool update_identifier(const std::string &text) {
    if (text.empty() || text.size() > 128) return false;
    for (unsigned char c : text) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_')) return false;
    return true;
}
static bool update_receive(const std::string &text) {
    if (text.empty() || text.size() > 8192) return false;
    for (unsigned char c : text) if ((c < 32 && c != '\t') || c == 127) return false;
    std::vector<std::string> fields;
    for (size_t at = 0;;) {
        size_t end = text.find('\t', at);
        fields.push_back(text.substr(at, end == std::string::npos ? end : end - at));
        if (fields.size() > 8) return false;
        if (end == std::string::npos) break;
        at = end + 1;
    }
    if (fields.size() < 3 || fields[0] != "1" || fields.back().size() > 1024) return false;
    auto &s = update_snapshot;
    if (fields[1] == "offer" && fields.size() == 6) {
        if (!update_identifier(fields[2]) || !update_identifier(fields[3]) || (fields[4] != "0" && fields[4] != "1") || s.applying) return false;
        s.id = fields[2]; s.version = fields[3]; s.rebuild = fields[4] == "1";
        s.offered = true; s.failed = false; s.message = fields[5];
    } else if (fields[1] == "state" && fields.size() == 4) {
        if (!update_identifier(fields[2])) return false;
        s.phase = fields[2]; s.message = fields[3];
    } else if (fields[1] == "progress" && fields.size() == 5) {
        if (!s.applying || !update_identifier(fields[2])) return false;
        char *end = nullptr; errno = 0;
        float value = strtof(fields[3].c_str(), &end);
        if (errno || !end || end == fields[3].c_str() || *end || !std::isfinite(value) || (value != -1 && (value < 0 || value > 1))) return false;
        s.phase = fields[2]; s.fraction = value; s.message = fields[4];
    } else if ((fields[1] == "error" || fields[1] == "cancelled" || fields[1] == "uptodate") && fields.size() == 3) {
        s.failed = fields[1] == "error"; s.applying = s.cancelling = s.offered = s.ready = false;
        s.message = fields[2]; s.phase = fields[1];
    } else if (fields[1] == "ready" && fields.size() == 3 && s.applying && !s.cancelling) {
        s.ready = true; s.message = fields[2];
    } else return false;
    ++s.revision;
    return true;
}
// Call with actual game/precompile activity, excluding the update's own busy flag.
static void update_poll(bool activity) {
    if (update_fd < 0) return;
    if (update_last_activity != (int)activity) {
        if (!update_send(std::string("1\tactivity\t") + (activity ? "busy" : "idle"))) return;
        update_last_activity = (int)activity;
    }
    for (unsigned i = 0; i < 16; ++i) {
        char bytes[8193]; ssize_t n = recv(update_fd, bytes, sizeof bytes, MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) { --i; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (n <= 0 || n > 8192 || !update_receive(std::string(bytes, (size_t)n))) {
            update_disconnect("Update connection closed. The current version is still usable."); return;
        }
    }
    if (!activity && !update_checked && update_fd >= 0) {
        update_checked = true;
        update_send("1\tcheck");
    }
}
static bool update_apply() {
    auto &s = update_snapshot;
    if (!s.enabled || !s.offered || s.applying || update_last_activity != 0) return false;
    s.applying = true; s.offered = false; s.failed = s.ready = s.cancelling = false;
    s.fraction = -1; s.phase = "starting"; s.message = "Preparing the update..."; ++s.revision;
    return update_send("1\tapply\t" + s.id);
}
static void update_defer() {
    if (!update_snapshot.applying) { update_snapshot.offered = false; update_send("1\tdefer"); }
}
static void update_cancel() {
    if (update_snapshot.applying && !update_snapshot.ready && !update_snapshot.cancelling) {
        update_snapshot.cancelling = true;
        update_snapshot.message = "Stopping the update; waiting for its current work...";
        update_send("1\tcancel");
    }
}
} // namespace launcher_linux
