// windows/game_logs.cpp — os/game_logs.h on Windows.
//
// The Windows counterpart of the Linux /proc/self/fd walk: the process's own open
// handles. NtQuerySystemInformation(SystemExtendedHandleInformation) lists every
// handle in the system; the rows of this process whose object type is the one a
// file handle of our own has are named with GetFinalPathNameByHandleW, after
// GetFileType says FILE_TYPE_DISK (it never blocks; naming a pipe can). The game
// opens debug.log / error.log once and keeps them for its lifetime, so whatever
// user directory it resolved (Documents, a redirected Documents, userdir.txt) is
// the one found. Reads go through a handle of the bridge's own, shared
// read+write+delete, sized by GetFileSizeEx.

#include "win_internal.h"

#include "os/game_logs.h"

#include <cstddef>
#include <cstring>
#include <cwchar>
#include <vector>

namespace bridge {
namespace os {

namespace {

struct HandleEntryEx {            // SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX
    void*      object;
    ULONG_PTR  pid;
    ULONG_PTR  handle;
    ULONG      access;
    USHORT     backtrace;
    USHORT     type_index;
    ULONG      attributes;
    ULONG      reserved;
};
struct HandleInfoEx {             // SYSTEM_HANDLE_INFORMATION_EX
    ULONG_PTR     count;
    ULONG_PTR     reserved;
    HandleEntryEx rows[1];
};
constexpr ULONG kSystemExtendedHandleInformation = 64;
constexpr LONG  kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004L);
using NtQSI = LONG(WINAPI*)(ULONG, void*, ULONG, ULONG*);

std::wstring widen(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), &w[0], n);
    return w;
}

std::string narrow(const wchar_t* w, int len) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, &s[0], n, nullptr, nullptr);
    return s;
}

// GetFinalPathNameByHandleW's "\\?\C:\x" -> "C:\x", "\\?\UNC\srv\x" -> "\\srv\x".
std::string final_path(HANDLE h) {
    wchar_t buf[2048];
    const DWORD n = GetFinalPathNameByHandleW(h, buf, DWORD(sizeof buf / sizeof buf[0]), FILE_NAME_NORMALIZED);
    if (n == 0 || n >= sizeof buf / sizeof buf[0]) return std::string();
    const wchar_t* p = buf;
    int len = int(n);
    if (len >= 8 && std::wcsncmp(p, L"\\\\?\\UNC\\", 8) == 0) {
        std::string s = "\\\\" + narrow(p + 8, len - 8);
        return s;
    }
    if (len >= 4 && std::wcsncmp(p, L"\\\\?\\", 4) == 0) { p += 4; len -= 4; }
    return narrow(p, len);
}

// The system handle list, or empty when the call is refused.
std::vector<unsigned char> handle_list() {
    std::vector<unsigned char> buf;
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    if (!nt) return buf;
    const auto q = reinterpret_cast<NtQSI>(reinterpret_cast<void*>(GetProcAddress(nt, "NtQuerySystemInformation")));
    if (!q) return buf;
    ULONG size = 1u << 20;
    for (int tries = 0; tries < 8; ++tries) {
        buf.assign(size, 0);
        ULONG need = 0;
        const LONG st = q(kSystemExtendedHandleInformation, buf.data(), size, &need);
        if (st >= 0) return buf;
        if (st != kStatusInfoLengthMismatch) break;
        size = (need > size ? need : size) * 2;
        if (size > (256u << 20)) break;
    }
    buf.clear();
    return buf;
}

HANDLE open_shared(const std::string& p) {
    const std::wstring w = widen(p);
    if (w.empty()) return INVALID_HANDLE_VALUE;
    return CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

}  // namespace

GameLogs find_game_logs() {
    GameLogs g;
    // A file handle of our own, to learn the File object type's index.
    wchar_t self[MAX_PATH * 2];
    const DWORD sn = GetModuleFileNameW(nullptr, self, DWORD(sizeof self / sizeof self[0]));
    HANDLE probe = INVALID_HANDLE_VALUE;
    if (sn > 0 && sn < sizeof self / sizeof self[0])
        probe = CreateFileW(self, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const std::vector<unsigned char> buf = handle_list();
    if (!buf.empty()) {
        const auto* info = reinterpret_cast<const HandleInfoEx*>(buf.data());
        const ULONG_PTR me = GetCurrentProcessId();
        const std::size_t cap = (buf.size() - offsetof(HandleInfoEx, rows)) / sizeof(HandleEntryEx);
        const std::size_t n = info->count < cap ? std::size_t(info->count) : cap;
        int file_type = -1;   // unknown: every row of ours is tried (GetFileType filters)
        for (std::size_t i = 0; i < n && probe != INVALID_HANDLE_VALUE; ++i) {
            const HandleEntryEx& r = info->rows[i];
            if (r.pid == me && reinterpret_cast<HANDLE>(r.handle) == probe) { file_type = r.type_index; break; }
        }
        for (std::size_t i = 0; i < n && (g.debug.empty() || g.error.empty()); ++i) {
            const HandleEntryEx& r = info->rows[i];
            if (r.pid != me) continue;
            if (file_type >= 0 && r.type_index != file_type) continue;
            HANDLE h = reinterpret_cast<HANDLE>(r.handle);
            if (h == probe) continue;
            if (GetFileType(h) != FILE_TYPE_DISK) continue;
            const std::string p = final_path(h);
            const int kind = game_log_kind(p, true);
            if (kind == 1 && g.debug.empty()) g.debug = p;
            else if (kind == 2 && g.error.empty()) g.error = p;
        }
    }
    if (probe != INVALID_HANDLE_VALUE) CloseHandle(probe);
    return g;
}

long long game_log_size(const std::string& p) {
    if (p.empty()) return -1;
    HANDLE h = open_shared(p);
    if (h == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER sz;
    const BOOL ok = GetFileSizeEx(h, &sz);
    CloseHandle(h);
    return ok ? static_cast<long long>(sz.QuadPart) : -1;
}

std::string game_log_read_from(const std::string& p, long long offset, std::size_t cap) {
    std::string out;
    if (p.empty() || offset < 0) return out;
    HANDLE h = open_shared(p);
    if (h == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(h, &sz)) {
        const long long size = static_cast<long long>(sz.QuadPart);
        if (size > offset + static_cast<long long>(cap)) offset = size - static_cast<long long>(cap);
        LARGE_INTEGER at; at.QuadPart = offset;
        if (size >= offset && SetFilePointerEx(h, at, nullptr, FILE_BEGIN)) {
            char b[16384];
            DWORD got = 0;
            while (out.size() < cap && ReadFile(h, b, DWORD(sizeof b), &got, nullptr) && got > 0)
                out.append(b, got);
        }
    }
    CloseHandle(h);
    return out;
}

}  // namespace os
}  // namespace bridge
