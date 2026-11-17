// game_logs.cpp — the portable half of os/game_logs.h: which log a path names.
// Standard C++17, no platform header (built into ck3_os on every OS and into the
// Linux bridge).
#include "os/game_logs.h"

namespace bridge {
namespace os {

namespace {
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// path ends with "<sep>logs<sep>NAME"
bool ends_with_log(const std::string& path, const char* name, bool win) {
    const std::string tail = std::string("/logs/") + name;
    if (path.size() < tail.size()) return false;
    const std::size_t at = path.size() - tail.size();
    for (std::size_t i = 0; i < tail.size(); ++i) {
        char p = path[at + i], t = tail[i];
        if (win) {
            if (p == '\\') p = '/';
            p = lower(p);
        }
        if (p != t) return false;
    }
    return true;
}
}  // namespace

int game_log_kind(const std::string& path, bool windows_rules) {
    if (ends_with_log(path, "debug.log", windows_rules)) return 1;
    if (ends_with_log(path, "error.log", windows_rules)) return 2;
    return 0;
}

}  // namespace os
}  // namespace bridge
