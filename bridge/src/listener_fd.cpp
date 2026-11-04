// listener_fd.cpp — see include/listener_fd.h (L21, node smoke18).
#include "listener_fd.h"

#ifndef _WIN32
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <cstdlib>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace bridge {

bool exe_is_game(const char* exe_path) {
    if (!exe_path || !*exe_path) return false;
    const char* base = strrchr(exe_path, '/');
    return strcmp(base ? base + 1 : exe_path, "ck3") == 0;
}

int listen_socket_cloexec() {
#ifdef CK3_LISTENER_LEGACY
    return socket(AF_UNIX, SOCK_STREAM, 0);          // the pre-L21 call (negative control)
#else
    return socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
#endif
}

int accept_cloexec(int fd, void* addr, unsigned* addr_len) {
#ifdef CK3_LISTENER_LEGACY
    return accept(fd, static_cast<struct sockaddr*>(addr), reinterpret_cast<socklen_t*>(addr_len));
#else
    return accept4(fd, static_cast<struct sockaddr*>(addr), reinterpret_cast<socklen_t*>(addr_len),
                   SOCK_CLOEXEC);
#endif
}

int close_listeners_bound_to(const char* path) {
#ifdef CK3_LISTENER_LEGACY
    (void)path;
    return 0;                                        // the pre-L21 behaviour: nothing closed
#else
    if (!path || !*path) return 0;
    DIR* d = opendir("/proc/self/fd");
    if (!d) return 0;
    const int dfd = dirfd(d);
    int fds[256];
    int n = 0;
    while (struct dirent* e = readdir(d)) {
        char* end = nullptr;
        const long fd = strtol(e->d_name, &end, 10);
        if (end == e->d_name || *end || fd == dfd || n >= 256) continue;
        fds[n++] = static_cast<int>(fd);
    }
    closedir(d);
    int closed = 0;
    for (int i = 0; i < n; ++i) {
        struct sockaddr_un a;
        socklen_t len = sizeof a;
        memset(&a, 0, sizeof a);
        if (getsockname(fds[i], reinterpret_cast<struct sockaddr*>(&a), &len) != 0) continue;
        if (a.sun_family != AF_UNIX || len <= sizeof(sa_family_t)) continue;
        int type = 0, acc = 0;
        socklen_t tl = sizeof type, al = sizeof acc;
        if (getsockopt(fds[i], SOL_SOCKET, SO_TYPE, &type, &tl) != 0 || type != SOCK_STREAM) continue;
        if (getsockopt(fds[i], SOL_SOCKET, SO_ACCEPTCONN, &acc, &al) != 0 || !acc) continue;
        if (strncmp(a.sun_path, path, sizeof a.sun_path) != 0) continue;
        if (close(fds[i]) == 0) ++closed;
    }
    return closed;
#endif
}

}  // namespace bridge
#endif  // !_WIN32
