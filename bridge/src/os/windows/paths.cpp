// windows/paths.cpp — os::paths on Windows.
//
// Resolves the instance paths from the environment through the portable rule
// module (os/win_paths.h), caches the strings for the process lifetime, and
// creates the directories the bridge writes into (base, state, telemetry, and
// log-archive when configured). %LOCALAPPDATA% is per-user already, so there
// is no 0700 verification step as on Linux; a refused configuration
// (CK3_INSTANCE_LAYOUT=legacy, a non-slug id, no base dir) leaves the strings
// resolved for a banner but returns `failed`, nothing created.

#include "win_internal.h"

#include "os/win_paths.h"

#include <mutex>
#include <string>

namespace bridge {
namespace os {

namespace {

const char* getenv_utf8(const char* key, void* user) {
    auto& store = *static_cast<std::string*>(user);
    wchar_t wkey[128];
    if (MultiByteToWideChar(CP_UTF8, 0, key, -1, wkey, 128) == 0) return nullptr;
    const DWORD n = GetEnvironmentVariableW(wkey, nullptr, 0);
    if (n == 0) return nullptr;  // unset (or empty: treated as unset, as env_nonempty does)
    std::wstring w(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(wkey, w.data(), n);
    if (got == 0 || got >= n) return nullptr;
    w.resize(got);
    const int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return nullptr;
    store.resize(need);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, store.data(), need, nullptr, nullptr);
    store.resize(need - 1);  // drop the NUL from size()
    return store.c_str();
}

// mkdir -p for a UTF-8 path, creating each ancestor. Ignores "already exists".
bool make_dirs(const std::string& path) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wn <= 0) return false;
    std::wstring w(wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), wn);
    w.resize(wn - 1);
    for (std::size_t i = 1; i <= w.size(); ++i) {
        if (i < w.size() && w[i] != L'\\' && w[i] != L'/') continue;
        const std::wstring part = w.substr(0, i);
        if (part.size() == 2 && part[1] == L':') continue;  // drive letter
        if (!CreateDirectoryW(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
    }
    return true;
}

struct Cached {
    Result  result;
    Paths   paths;
    WinPaths w;  // owns the strings Paths points into
};

// Fills *c IN PLACE: c->paths points into c->w, so c must already live where
// it will stay (SSO strings move with the object, so setting the pointers
// before a copy would dangle — the bug this avoids).
void compute(Cached* c) {
    std::string scratch;
    c->w = resolve_win_paths(&getenv_utf8, &scratch);
    const WinPaths& w = c->w;
    c->paths.instance_id = w.instance_id.c_str();
    c->paths.socket = w.socket.c_str();
    c->paths.bridge_log = w.bridge_log.c_str();
    c->paths.telemetry_dir = w.telemetry_dir.c_str();
    c->paths.log_archive_dir = w.log_archive_dir.c_str();
    c->paths.state_dir = w.state_dir.c_str();

    if (!w.config_error.empty()) {
        c->result = Result{Status::failed, ERROR_INVALID_PARAMETER, c->w.config_error.c_str()};
        return;
    }
    bool ok = make_dirs(w.state_dir) && make_dirs(w.telemetry_dir);
    if (ok && !w.log_archive_dir.empty()) ok = make_dirs(w.log_archive_dir);
    c->result = ok ? win::ok() : win::failed_last("could not create an instance directory");
}

}  // namespace

Result paths(Paths* out) {
    if (!out) return win::failed(ERROR_INVALID_PARAMETER, "out is null");
    static std::once_flag once;
    static Cached cached;
    std::call_once(once, [] { compute(&cached); });
    *out = cached.paths;
    return cached.result;
}

}  // namespace os
}  // namespace bridge
