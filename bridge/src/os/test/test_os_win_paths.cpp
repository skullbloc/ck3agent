// test_os_win_paths.cpp — os::paths on Windows (the cell), engine-free. Runs
// under wine. It points LOCALAPPDATA at a throwaway directory, calls paths()
// once, and checks the resolved members and that the directories were made.
// The rule table itself is test_os_win_paths_rules (portable); this proves the
// cell wires the rules to the real %LOCALAPPDATA% and creates the tree.

#include "os/os.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace os = bridge::os;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("FAIL: %s\n", what); }
    else std::printf("ok:   %s\n", what);
}

static bool is_dir(const char* utf8) {
    wchar_t w[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 1024) == 0) return false;
    const DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int main() {
    wchar_t tmp[MAX_PATH], base[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::swprintf(base, MAX_PATH, L"%lsck3-os-paths-%lu", tmp, GetCurrentProcessId());
    SetEnvironmentVariableW(L"LOCALAPPDATA", base);
    SetEnvironmentVariableW(L"CK3_INSTANCE_LAYOUT", nullptr);
    SetEnvironmentVariableW(L"CK3_INSTANCE_ID", L"wintest");

    os::Paths p;
    const os::Result r = os::paths(&p);
    check(os::succeeded(r), "paths: ok");
    char b8[1024];
    WideCharToMultiByte(CP_UTF8, 0, base, -1, b8, sizeof(b8), nullptr, nullptr);
    const std::string root = std::string(b8) + "\\ck3agent\\inst\\wintest";
    check(std::strcmp(p.instance_id, "wintest") == 0, "paths: instance_id is the slug");
    check(p.socket == root + "\\agent.sock" && p.state_dir == root &&
              p.log_archive_dir == root + "\\log-archive",
          "paths: members under %LOCALAPPDATA%\\ck3agent\\inst\\<id>");
    check(is_dir(p.state_dir) && is_dir(p.telemetry_dir) && is_dir(p.log_archive_dir),
          "paths: the state, telemetry and archive directories were created");

    os::Paths again;
    check(os::succeeded(os::paths(&again)) && std::strcmp(again.socket, p.socket) == 0,
          "paths: a second call returns the same cached strings");
    check(os::paths(nullptr).status == os::Status::failed, "paths(null): failed");

    // Clean up the tree (best effort).
    std::string rd = "cmd /c rmdir /s /q \"" + std::string(b8) + "\"";
    std::system(rd.c_str());
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
