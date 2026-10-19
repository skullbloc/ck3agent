// windows/process.cpp — os::process_identity / os::exe_path_is_game on Windows.
//
// The bridge is loaded into Steam and its helpers too, so everything is gated
// on "am I ck3.exe?". The path is GetModuleFileNameW(NULL) as UTF-8; the rule
// is the basename after the last '\' or '/', ASCII case-insensitive, equal to
// "ck3.exe" (ADR: the Windows analogue of the Linux basename == "ck3" check).

#include "win_internal.h"

#include <cstring>

namespace bridge {
namespace os {

namespace {

char ascii_lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

bool iequals(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b)
        if (ascii_lower(*a) != ascii_lower(*b)) return false;
    return *a == '\0' && *b == '\0';
}

}  // namespace

Result exe_path_is_game(const char* exe_path, bool* is_game) {
    if (!is_game) return win::failed(ERROR_INVALID_PARAMETER, "is_game is null");
    *is_game = false;
    if (!exe_path) return win::failed(ERROR_INVALID_PARAMETER, "exe_path is null");
    const char* bslash = std::strrchr(exe_path, '\\');
    const char* fslash = std::strrchr(exe_path, '/');
    const char* sep = bslash > fslash ? bslash : fslash;  // both null -> null
    const char* base = sep ? sep + 1 : exe_path;
    *is_game = iequals(base, "ck3.exe");
    return win::ok();
}

Result process_identity(ProcessIdentity* out) {
    if (!out) return win::failed(ERROR_INVALID_PARAMETER, "out is null");
    out->exe_path[0] = '\0';
    out->is_game = false;
    wchar_t wpath[kMaxPath];
    if (!win::module_path_w(nullptr, wpath, kMaxPath))
        return win::failed_last("GetModuleFileNameW(NULL) failed or path too long");
    if (!win::utf16_to_utf8(wpath, out->exe_path, kMaxPath)) {
        out->exe_path[0] = '\0';
        return win::failed_last("the executable path is not representable as UTF-8");
    }
    return exe_path_is_game(out->exe_path, &out->is_game);
}

}  // namespace os
}  // namespace bridge
