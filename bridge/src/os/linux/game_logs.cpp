// game_logs.cpp (Linux) — os/game_logs.h on Linux. The three functions are the
// Code of action_handlers.cpp (find_game_logs, file_size_of, read_from),
// moved here byte for byte; only their names changed.
#include "os/game_logs.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <string_view>

namespace bridge {
namespace os {

// The game's own log files, found by its open descriptors (the game holds
// debug.log and error.log open for writing under <userdir>/logs/). A -userdir
// game and the default tree are found the same way.
GameLogs find_game_logs() {
    GameLogs g;
    DIR* d = opendir("/proc/self/fd");
    if (!d) return g;
    char path[64], target[4096];
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "/proc/self/fd/%s", e->d_name);
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n <= 0) continue;
        target[n] = 0;
        std::string_view t(target, (size_t)n);
        auto ends = [&](std::string_view s) {
            return t.size() >= s.size() && t.substr(t.size() - s.size()) == s;
        };
        if (g.debug.empty() && ends("/logs/debug.log")) g.debug.assign(t);
        else if (g.error.empty() && ends("/logs/error.log")) g.error.assign(t);
    }
    closedir(d);
    return g;
}

long long game_log_size(const std::string& p) {
    struct stat st;
    if (p.empty() || stat(p.c_str(), &st) != 0) return -1;
    return (long long)st.st_size;
}

// At most `cap` bytes of P from OFFSET on (the tail when more were written).
std::string game_log_read_from(const std::string& p, long long offset, size_t cap) {
    std::string out;
    if (p.empty() || offset < 0) return out;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return out;
    long long size = game_log_size(p);
    if (size > offset + (long long)cap) offset = size - (long long)cap;
    if (size >= offset && fseeko(f, offset, SEEK_SET) == 0) {
        char buf[16384];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && out.size() < cap) out.append(buf, n);
    }
    fclose(f);
    return out;
}

}  // namespace os
}  // namespace bridge
