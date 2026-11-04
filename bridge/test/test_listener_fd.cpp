// test_listener_fd.cpp — L21 (node smoke18): the bridge socket lives only in the game.
//
// 1. exe_is_game: the game's binary, not its CrashReporter or a near name.
// 2. A listener made by listen_socket_cloexec is NOT inherited across exec:
//    a child shell asks /proc for the descriptor. The pre-L21 socket() was
//    inherited, and CrashReporter (fork + exec from CK3's crash handler) kept
//    the socket accepting with no bridge behind it.
// 3. close_listeners_bound_to(P) closes this process's listener on P (and so
//    a connect to P is refused) and leaves a listener on another path open.
// 4. accept_cloexec gives the accepted descriptor FD_CLOEXEC.
// The negative control (test_listener_fd_legacy_control, -DCK3_LISTENER_LEGACY:
// the pre-L21 calls) must fail EXACTLY the rows in kLegacyFails and pass every
// other row; it exits 0 only then. A crash, an early exit (mkdtemp) or any
// other set of FAIL rows is a nonzero exit, so ctest runs it as a plain test
// (not WILL_FAIL, which the mkdtemp early return, rc 2, also satisfied).
#include "listener_fd.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <fcntl.h>
#include <set>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail = 0;
static int g_rows = 0;
static std::set<std::string> g_failed;
#define CHECK(c, what) do { ++g_rows; if (c) printf("ok   %s\n", what); \
    else { printf("FAIL %s\n", what); ++g_fail; g_failed.insert(what); } } while (0)

// The rows the pre-L21 calls fail, by name (the legacy control's contract).
static const char* const kLegacyFails[] = {
    "an exec'd child (CrashReporter) does not inherit the listener",
    "listener is FD_CLOEXEC",
    "accept_cloexec: the accepted descriptor is FD_CLOEXEC",
    "close_listeners_bound_to closed exactly one",
    "the inherited descriptor is closed",
    "the bridge path no longer answers",
};
static const int kRows = 13;   // every CHECK below; a row that did not run is a failure

static int listen_at(const std::string& path, bool cloexec) {
    int fd = cloexec ? bridge::listen_socket_cloexec() : socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    unlink(path.c_str());
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&a), sizeof a) != 0 || listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool connects(const std::string& path) {
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    const bool ok = connect(c, reinterpret_cast<struct sockaddr*>(&a), sizeof a) == 0;
    close(c);
    return ok;
}

// Does an exec'd child still hold descriptor FD? (the child's own /proc says)
static bool child_inherits(int fd) {
    const pid_t pid = fork();
    if (pid == 0) {
        char cmd[64];
        snprintf(cmd, sizeof cmd, "[ -e /proc/$$/fd/%d ]", fd);
        execl("/bin/sh", "sh", "-c", cmd, static_cast<char*>(nullptr));
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

int main() {
    CHECK(bridge::exe_is_game("/home/u/.local/share/Steam/steamapps/common/Crusader Kings III/binaries/ck3"),
          "exe_is_game: binaries/ck3");
    CHECK(!bridge::exe_is_game("/home/u/.local/share/Steam/steamapps/common/Crusader Kings III/binaries/CrashReporter"),
          "exe_is_game: CrashReporter is not the game");
    CHECK(!bridge::exe_is_game("/usr/bin/ck3agent") && !bridge::exe_is_game("/x/ck3 (deleted)")
          && !bridge::exe_is_game("") && !bridge::exe_is_game(nullptr), "exe_is_game: near names");

    char dir[] = "/tmp/l21-listener-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }
    const std::string bridge_sock = std::string(dir) + "/agent.sock";
    const std::string other_sock = std::string(dir) + "/other.sock";

    int lfd = listen_at(bridge_sock, true);
    CHECK(lfd >= 0, "listener on the bridge path");
    CHECK(child_inherits(lfd) == false, "an exec'd child (CrashReporter) does not inherit the listener");
    CHECK(fcntl(lfd, F_GETFD) & FD_CLOEXEC, "listener is FD_CLOEXEC");
    {
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un a;
        memset(&a, 0, sizeof a);
        a.sun_family = AF_UNIX;
        strncpy(a.sun_path, bridge_sock.c_str(), sizeof a.sun_path - 1);
        const bool conn = connect(c, reinterpret_cast<struct sockaddr*>(&a), sizeof a) == 0;
        struct sockaddr_un peer;
        unsigned plen = sizeof peer;
        const int acc = conn ? bridge::accept_cloexec(lfd, &peer, &plen) : -1;
        CHECK(acc >= 0 && (fcntl(acc, F_GETFD) & FD_CLOEXEC),
              "accept_cloexec: the accepted descriptor is FD_CLOEXEC");
        if (acc >= 0) close(acc);
        close(c);
    }
    close(lfd);

    // An inherited listener (made the pre-L21 way) on the bridge path, and one elsewhere.
    int inherited = listen_at(bridge_sock, false);
    int other = listen_at(other_sock, false);
    CHECK(inherited >= 0 && other >= 0 && connects(bridge_sock), "inherited listener answers");
    const int n = bridge::close_listeners_bound_to(bridge_sock.c_str());
    CHECK(n == 1, "close_listeners_bound_to closed exactly one");
    CHECK(fcntl(inherited, F_GETFD) == -1 && errno == EBADF, "the inherited descriptor is closed");
    CHECK(!connects(bridge_sock), "the bridge path no longer answers");
    CHECK(fcntl(other, F_GETFD) != -1 && connects(other_sock), "a listener on another path is left open");
    CHECK(bridge::close_listeners_bound_to(nullptr) == 0 && bridge::close_listeners_bound_to("") == 0,
          "no path closes nothing");
    if (fcntl(inherited, F_GETFD) != -1) close(inherited);
    close(other);
    unlink(bridge_sock.c_str());
    unlink(other_sock.c_str());
    rmdir(dir);
#ifdef CK3_LISTENER_LEGACY
    std::set<std::string> want(std::begin(kLegacyFails), std::end(kLegacyFails));
    const bool exact = g_rows == kRows && g_failed == want;
    for (const auto& w : want)
        if (!g_failed.count(w)) printf("CONTROL: expected FAIL did not fail: %s\n", w.c_str());
    for (const auto& f : g_failed)
        if (!want.count(f)) printf("CONTROL: unexpected FAIL: %s\n", f.c_str());
    printf("CONTROL %s (%d rows of %d, %d failed, %zu expected)\n", exact ? "PASS" : "FAIL",
           g_rows, kRows, g_fail, want.size());
    return exact ? 0 : 1;
#else
    const bool all = g_rows == kRows && g_fail == 0;
    printf("%s (%d rows of %d, %d failed)\n", all ? "PASS" : "FAIL", g_rows, kRows, g_fail);
    return all ? 0 : 1;
#endif
}
