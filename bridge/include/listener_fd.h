// listener_fd.h — the bridge's socket must live only in the game process (L21).
//
// node smoke18 (2026.12.1): CK3 crashed, its crash handler started CrashReporter
// (fork + exec, LD_PRELOAD and all), and the reporter inherited the listening
// socket's descriptor: the socket kept accepting connections with no bridge
// behind it, and the next `ck3agent launch` refused ("a bridge is already
// listening") while no game ran. Two rules close that:
//   1. every descriptor the IPC server opens is close-on-exec, so an exec'd
//      child never holds it (listen_socket_cloexec, accept_cloexec);
//   2. a process the .so loads into whose /proc/self/exe is not the game
//      serves nothing, and closes any listener it inherited on the bridge's
//      socket path (exe_is_game, close_listeners_bound_to).
// PURE (libc only); host-tested by test_listener_fd. Linux only.
#pragma once

namespace bridge {

// True when EXE_PATH (a /proc/self/exe target) is the game: basename exactly "ck3".
bool exe_is_game(const char* exe_path);

// socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC); -1 with errno on failure.
int listen_socket_cloexec();

// accept4(..., SOCK_CLOEXEC).
int accept_cloexec(int fd, void* addr, unsigned* addr_len);

// Close every LISTENING AF_UNIX stream socket of this process bound to PATH.
// Sockets bound elsewhere (a Steam helper's own) are left alone. Returns the
// number closed.
int close_listeners_bound_to(const char* path);

}  // namespace bridge
