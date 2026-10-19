// win_paths.cpp — the Windows instance-path rules (os/win_paths.h).
// Standard C++17, no platform header.

#include "os/win_paths.h"

#include <cstring>

namespace bridge {
namespace os {

namespace {

constexpr char kDefaultId[] = "default";
constexpr char kSep = '\\';

bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

// instance_paths.cpp env_stripped(): surrounding whitespace removed.
std::string stripped(EnvLookup get, void* u, const char* key) {
    const char* v = get(key, u);
    if (!v) return std::string();
    std::string s(v);
    std::size_t b = 0, e = s.size();
    while (b < e && is_ws(s[b])) ++b;
    while (e > b && is_ws(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// instance_paths.cpp env_nonempty(): set and non-empty, taken as is.
const char* nonempty(EnvLookup get, void* u, const char* key) {
    const char* v = get(key, u);
    return (v && *v) ? v : nullptr;
}

std::string join(const std::string& a, const char* b) {
    if (a.empty()) return std::string(b);
    if (a.back() == '\\' || a.back() == '/') return a + b;
    return a + kSep + b;
}

}  // namespace

bool instance_id_is_slug(const char* s) {
    if (!s || !*s) return false;
    const std::size_t n = std::strlen(s);
    if (n > 48) return false;
    auto alnum = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    if (!alnum(s[0])) return false;
    for (std::size_t i = 1; i < n; ++i)
        if (!alnum(s[i]) && s[i] != '.' && s[i] != '_' && s[i] != '-') return false;
    return true;
}

WinPaths resolve_win_paths(EnvLookup get, void* u) {
    WinPaths p;
    p.id_refused = false;

    const std::string lay = stripped(get, u, "CK3_INSTANCE_LAYOUT");
    if (!lay.empty() && lay != "user") {
        p.config_error = lay == "legacy"
            ? "CK3_INSTANCE_LAYOUT=legacy: the /tmp legacy layout does not exist on Windows"
            : "CK3_INSTANCE_LAYOUT=" + lay + ": want unset or user";
    }

    if (const char* la = nonempty(get, u, "LOCALAPPDATA")) {
        p.base = join(la, "ck3agent");
    } else if (const char* up = nonempty(get, u, "USERPROFILE")) {
        p.base = join(join(join(up, "AppData"), "Local"), "ck3agent");
    } else if (p.config_error.empty()) {
        p.config_error = "neither LOCALAPPDATA nor USERPROFILE is set: no private base directory";
    }

    const std::string id = stripped(get, u, "CK3_INSTANCE_ID");
    const char* raw = id.empty() ? nullptr : id.c_str();
    if (raw && !instance_id_is_slug(raw)) {
        p.id_refused = true;
        if (p.config_error.empty()) p.config_error = "CK3_INSTANCE_ID=" + id + ": not a valid slug";
        raw = nullptr;
    }
    p.instance_id = (raw && std::strcmp(raw, kDefaultId) != 0) ? raw : kDefaultId;
    const bool is_default = p.instance_id == kDefaultId;

    const char* root = nonempty(get, u, "CK3_INSTANCE_ROOT");
    const std::string dir = join(root ? std::string(root) : join(p.base, "inst"), p.instance_id.c_str());

    auto pick = [&](const char* key, const char* member) {
        if (const char* ov = nonempty(get, u, key)) return std::string(ov);
        return join(is_default ? p.base : dir, member);
    };
    p.socket = pick("CK3_BRIDGE_SOCKET", "agent.sock");
    p.telemetry_dir = pick("CK3_TELE", "telemetry");
    p.bridge_log = pick("CK3_BRIDGE_LOG", "bridge.log");
    p.state_dir = is_default ? p.base : dir;
    if (const char* ov = nonempty(get, u, "CK3_BRIDGE_LOG_ARCHIVE_DIR"))
        p.log_archive_dir = ov;
    else
        p.log_archive_dir = is_default ? std::string() : join(dir, "log-archive");
    return p;
}

}  // namespace os
}  // namespace bridge
