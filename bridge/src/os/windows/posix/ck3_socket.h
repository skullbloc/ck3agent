#pragma once
// Windows POSIX shim (ADR 0001 P5): the socket subset ipc_server.cpp uses.
// Winsock supplies pollfd / POLL* / socket / bind / listen / accept / send /
// recv / getsockopt; this adds what POSIX has and Winsock lacks:
//   MSG_NOSIGNAL   0: Windows raises no SIGPIPE
//   MSG_DONTWAIT   a flag ipc_server's Windows wrapper turns into a zero-timeout
//                  WSAPoll before recv (Winsock has no per-call non-blocking flag)
//   SO_PEERCRED, struct ucred   type only: the wrapper refuses SO_PEERCRED
//                  (admission on Windows is the endpoint token, not a uid)
//   struct sockaddr_un, TIOCOUTQ   type/constant only (ioctl refuses)
#include "ck3_posix.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#define MSG_NOSIGNAL 0
#define MSG_DONTWAIT 0x40000000
#define SO_PEERCRED  17
#define TIOCOUTQ     0x5411
#ifndef AF_UNIX
#define AF_UNIX 1
#endif
struct ucred { pid_t pid; uid_t uid; gid_t gid; };
struct sockaddr_un { unsigned short sun_family; char sun_path[108]; };
extern "C" int ioctl(int fd, unsigned long req, ...);   // refuses (ENOSYS)
