// test_os_win_process.cpp — os::process_identity / exe_path_is_game on
// Windows, engine-free, under wine. process_identity is checked against
// GetModuleFileNameW directly; the basename rule against a table of paths.

#include "os/os.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace os = bridge::os;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("FAIL: %s\n", what); }
    else std::printf("ok:   %s\n", what);
}

int main() {
    os::ProcessIdentity p;
    std::memset(&p, 0x5a, sizeof(p));
    check(os::succeeded(os::process_identity(&p)), "process_identity: ok");

    wchar_t w[MAX_PATH];
    GetModuleFileNameW(nullptr, w, MAX_PATH);
    char u8[os::kMaxPath];
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, sizeof(u8), nullptr, nullptr);
    check(std::strcmp(p.exe_path, u8) == 0, "process_identity: exe_path == GetModuleFileNameW(NULL)");
    check(!p.is_game, "process_identity: this test exe is not the game");

    struct Case { const char* path; bool game; };
    const Case cases[] = {
        {"C:\\Program Files (x86)\\Steam\\steamapps\\common\\CK3\\binaries\\ck3.exe", true},
        {"C:\\games\\CK3.EXE", true},                 // case-insensitive
        {"ck3.exe", true},                            // no separator
        {"D:/mixed/slashes/ck3.exe", true},           // forward slash
        {"C:\\x\\ck3", false},                        // no .exe
        {"C:\\x\\ck3.exe.bak", false},
        {"C:\\x\\notck3.exe", false},
        {"C:\\x\\dowser.exe", false},                 // the launcher
        {"", false},
    };
    bool all = true;
    for (const Case& c : cases) {
        bool g = !c.game;
        os::Result r = os::exe_path_is_game(c.path, &g);
        all = all && os::succeeded(r) && g == c.game;
    }
    check(all, "exe_path_is_game: the basename rule classifies every path correctly");

    bool g = true;
    check(os::exe_path_is_game(nullptr, &g).status == os::Status::failed && !g,
          "exe_path_is_game(null path): failed, out false");
    check(os::exe_path_is_game("ck3.exe", nullptr).status == os::Status::failed,
          "exe_path_is_game(null out): failed");
    check(os::process_identity(nullptr).status == os::Status::failed, "process_identity(null): failed");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
