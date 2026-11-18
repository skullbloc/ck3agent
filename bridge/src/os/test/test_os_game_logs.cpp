// test_os_game_logs.cpp — os/game_logs.h, engine-free, ONE row set for both
// platforms: test_os_linux_game_logs here, test_os_win_game_logs
// built for Windows and run under wine by a test driver
//
//
// The test plays the game: it creates <tmp>/<dir>/logs/debug.log and error.log,
// KEEPS BOTH OPEN for writing (as the game does), with a decoy
// <tmp>/<dir>/notlogs/debug.log open too, and requires find_game_logs() to name
// exactly the two held files. Then: the size and the read-from offset follow the
// writer while it still holds the file open (on Windows that is the shared-read
// handle and GetFileSizeEx), a closed file is not found, an empty path reads
// -1 / empty. The portable path rule runs on every OS: both separators and any
// ASCII case on Windows, exact on Linux.

#include "os/game_logs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace os = bridge::os;

static int g_fail = 0;
#define CHECK(c, what) do { if (!(c)) { std::printf("FAIL %s\n", what); ++g_fail; } \
                            else std::printf("ok   %s\n", what); } while (0)

#ifdef _WIN32
static const char kSep = '\\';
struct Writer {
    HANDLE h = INVALID_HANDLE_VALUE;
    bool open(const std::string& p) {
        // The game's own sharing: other readers allowed while it writes.
        h = CreateFileA(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return h != INVALID_HANDLE_VALUE;
    }
    void write(const char* s) { DWORD n = 0; WriteFile(h, s, DWORD(std::strlen(s)), &n, nullptr); }
    void close() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); h = INVALID_HANDLE_VALUE; }
};
static bool make_dir(const std::string& p) { return CreateDirectoryA(p.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS; }
static std::string temp_root() {
    // GetTempPathA can name the directory by its 8.3 short form (GitHub's
    // windows runners: C:\Users\RUNNER~1\...); the cell reports the file
    // system's final, long name. Compare like with like: the long form.
    char s[MAX_PATH]; const DWORD n = GetTempPathA(MAX_PATH, s);
    std::string t(s, n < MAX_PATH ? n : 0);
    char b[MAX_PATH]; const DWORD ln = t.empty() ? 0 : GetLongPathNameA(t.c_str(), b, MAX_PATH);
    if (ln > 0 && ln < MAX_PATH) t.assign(b, ln);
    if (!t.empty() && (t.back() == '\\' || t.back() == '/')) t.pop_back();
    char name[64]; std::snprintf(name, sizeof name, "\\ck3_game_logs_%lu", (unsigned long)GetCurrentProcessId());
    return t + name;
}
// The path the cell reports is the file system's final name: compare it the
// way Windows compares names.
static bool same_path(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x == '/') x = '\\';
        if (y == '/') y = '\\';
        if (x >= 'A' && x <= 'Z') x = char(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = char(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}
#else
static const char kSep = '/';
struct Writer {
    FILE* f = nullptr;
    bool open(const std::string& p) { f = std::fopen(p.c_str(), "wb"); return f != nullptr; }
    void write(const char* s) { std::fputs(s, f); std::fflush(f); }
    void close() { if (f) std::fclose(f); f = nullptr; }
};
static bool make_dir(const std::string& p) { return mkdir(p.c_str(), 0700) == 0; }
static std::string temp_root() {
    char tmpl[] = "/tmp/ck3_game_logs_XXXXXX";
    const char* d = mkdtemp(tmpl);
    return d ? std::string(d) : std::string();
}
static bool same_path(const std::string& a, const std::string& b) {
    char ra[4096], rb[4096];
    if (!realpath(a.c_str(), ra) || !realpath(b.c_str(), rb)) return a == b;
    return std::strcmp(ra, rb) == 0;
}
#endif

static void rules() {
    CHECK(os::game_log_kind("/home/u/.local/share/Paradox Interactive/Crusader Kings III/logs/debug.log", false) == 1,
          "rule: linux debug.log");
    CHECK(os::game_log_kind("/x/logs/error.log", false) == 2, "rule: linux error.log");
    CHECK(os::game_log_kind("/x/logs/Debug.log", false) == 0, "rule: linux is case-exact");
    CHECK(os::game_log_kind("C:\\x\\logs\\debug.log", false) == 0, "rule: linux takes no backslash");
    CHECK(os::game_log_kind("/x/notlogs/debug.log", false) == 0, "rule: the parent must be logs");
    CHECK(os::game_log_kind("/x/logs/debug.log.1", false) == 0, "rule: a rotated name is not the log");
    CHECK(os::game_log_kind("C:\\Users\\u\\Documents\\Paradox Interactive\\Crusader Kings III\\logs\\debug.log", true) == 1,
          "rule: windows backslashes");
    CHECK(os::game_log_kind("D:\\CK3\\User\\LOGS\\Error.Log", true) == 2, "rule: windows ignores case");
    CHECK(os::game_log_kind("C:/x/logs/debug.log", true) == 1, "rule: windows takes forward slashes");
    CHECK(os::game_log_kind("C:\\x\\mylogs\\debug.log", true) == 0, "rule: windows parent must be logs");
    CHECK(os::game_log_kind("", true) == 0 && os::game_log_kind("debug.log", false) == 0, "rule: short paths");
}

int main() {
    rules();

    const std::string root = temp_root();
    CHECK(!root.empty() && (make_dir(root) || true), "temp root");
    const std::string game = root + kSep + "user";
    const std::string logs = game + kSep + "logs";
    const std::string decoy_dir = game + kSep + "notlogs";
    make_dir(game); make_dir(logs); make_dir(decoy_dir);
    const std::string dbg = logs + kSep + "debug.log", err = logs + kSep + "error.log";
    const std::string decoy = decoy_dir + kSep + "debug.log";

    {
        const os::GameLogs none = os::find_game_logs();
        CHECK(none.debug.empty() && none.error.empty(), "nothing held: nothing found");
    }
    Writer wd, we, wx;
    CHECK(wx.open(decoy), "decoy held");
    CHECK(wd.open(dbg) && we.open(err), "both logs held open for writing");
    const char* first = "[00:00:01][game.cpp:1]: start\n";
    wd.write(first);

    const os::GameLogs g = os::find_game_logs();
    CHECK(same_path(g.debug, dbg), "debug.log found by its open handle");
    CHECK(same_path(g.error, err), "error.log found by its open handle");
    std::printf("     debug=%s\n     error=%s\n", g.debug.c_str(), g.error.c_str());
    if (!same_path(g.debug, dbg) || !same_path(g.error, err))
        std::printf("     want debug=%s\n     want error=%s\n", dbg.c_str(), err.c_str());

    const long long s0 = os::game_log_size(g.debug);
    CHECK(s0 == (long long)std::strlen(first), "size while the writer holds it");
    wd.write("CK3AGENT_SA1 ran\n");
    const long long s1 = os::game_log_size(g.debug);
    CHECK(s1 == s0 + 17, "size follows the open writer");
    CHECK(os::game_log_read_from(g.debug, s0, 1024) == "CK3AGENT_SA1 ran\n", "read from the offset, writer still open");
    CHECK(os::game_log_read_from(g.debug, 0, 6) == "1 ran\n", "a capped read returns the tail");
    CHECK(os::game_log_read_from(g.debug, s1, 1024).empty(), "nothing past the end");
    CHECK(os::game_log_size(g.error) == 0, "the empty error.log");

    CHECK(os::game_log_size("") == -1 && os::game_log_read_from("", 0, 16).empty(),
          "empty path: absent, never a guess");
    CHECK(os::game_log_read_from(g.debug, -1, 16).empty(), "negative offset reads nothing");

    wd.close(); we.close(); wx.close();
    const os::GameLogs after = os::find_game_logs();
    CHECK(after.debug.empty() && after.error.empty(), "closed: no longer found");

#ifdef _WIN32
    DeleteFileA(dbg.c_str()); DeleteFileA(err.c_str()); DeleteFileA(decoy.c_str());
    RemoveDirectoryA(logs.c_str()); RemoveDirectoryA(decoy_dir.c_str()); RemoveDirectoryA(game.c_str());
    RemoveDirectoryA(root.c_str());
#else
    unlink(dbg.c_str()); unlink(err.c_str()); unlink(decoy.c_str());
    rmdir(logs.c_str()); rmdir(decoy_dir.c_str()); rmdir(game.c_str()); rmdir(root.c_str());
#endif
    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "passed", g_fail);
    return g_fail ? 1 : 0;
}
