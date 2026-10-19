// windows/transport.cpp — the os::transport cell on Windows: a TCP loopback
// listener (ADR 0001 "IPC and the Python side": Windows has no AF_UNIX in
// CPython yet, so the bridge listens on 127.0.0.1 and the port + token go in
// endpoint.json). The NDJSON framing and the handshake live above this layer
// and are unchanged; this is only the byte transport.
//
// The listener binds 127.0.0.1 only, never a routable address, so it is
// reachable only from the same machine. A port of 0 in the endpoint means
// "OS-assigned"; the chosen port is read back with getsockname and written
// into endpoint.address so the caller (and endpoint.json, in P4) can publish
// it. A Handle is the SOCKET value; kInvalidHandle (-1) is INVALID_SOCKET.
//
// Winsock is started once per process and never cleaned up: the bridge lives
// for the process's lifetime, and WSACleanup on a shared stack is the kind of
// teardown race the bridge avoids elsewhere.

#include "win_internal.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>
#include <mutex>

namespace bridge {
namespace os {

namespace {

constexpr const char kLoopback[] = "127.0.0.1";
// Storage for the one endpoint address string ipc_endpoint / transport_listen
// hand back (never null; valid for the process lifetime).
char g_endpoint_addr[32] = "127.0.0.1:0";

void ensure_winsock() {
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
    });
}

Result wsa_failed(const char* d) { return Result{Status::failed, WSAGetLastError(), d}; }

// "127.0.0.1:NNNN" -> host, port. Only the loopback host is accepted.
bool parse_loopback(const char* addr, unsigned short* port) {
    const char* colon = std::strrchr(addr, ':');
    if (!colon) return false;
    const std::size_t hostlen = std::size_t(colon - addr);
    if (hostlen != std::strlen(kLoopback) || std::strncmp(addr, kLoopback, hostlen) != 0)
        return false;
    unsigned long p = std::strtoul(colon + 1, nullptr, 10);
    if (p > 65535) return false;
    *port = static_cast<unsigned short>(p);
    return true;
}

SOCKET to_sock(Handle h) { return static_cast<SOCKET>(h); }
Handle from_sock(SOCKET s) { return static_cast<Handle>(s); }

}  // namespace

Result ipc_endpoint(Endpoint* out) {
    if (!out) return win::failed(ERROR_INVALID_PARAMETER, "out is null");
    // Port 0 = OS-assigned; the real port is known only after transport_listen
    // (endpoint.json, written in P4, carries it to the client).
    out->kind = TransportKind::tcp_loopback;
    out->address = "127.0.0.1:0";
    return win::ok();
}

Result transport_listen(const Endpoint& at, Handle* listener) {
    if (listener) *listener = kInvalidHandle;
    if (!listener) return win::failed(ERROR_INVALID_PARAMETER, "listener is null");
    if (at.kind != TransportKind::tcp_loopback)
        return win::failed(ERROR_INVALID_PARAMETER, "windows transport is tcp_loopback only");
    unsigned short port = 0;
    if (!at.address || !parse_loopback(at.address, &port))
        return win::failed(ERROR_INVALID_PARAMETER, "address must be 127.0.0.1:<port>");
    ensure_winsock();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return wsa_failed("socket() failed");
    sockaddr_in a;
    std::memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    InetPtonA(AF_INET, kLoopback, &a.sin_addr);
    if (bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == SOCKET_ERROR) {
        Result r = wsa_failed("bind(127.0.0.1) failed");
        closesocket(s);
        return r;
    }
    if (listen(s, SOMAXCONN) == SOCKET_ERROR) {
        Result r = wsa_failed("listen() failed");
        closesocket(s);
        return r;
    }
    sockaddr_in bound;
    int blen = sizeof(bound);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &blen) == 0)
        std::snprintf(g_endpoint_addr, sizeof(g_endpoint_addr), "127.0.0.1:%u",
                      unsigned(ntohs(bound.sin_port)));
    *listener = from_sock(s);
    return win::ok();
}

Result transport_accept(Handle listener, Handle* conn) {
    if (conn) *conn = kInvalidHandle;
    if (!conn) return win::failed(ERROR_INVALID_PARAMETER, "conn is null");
    if (listener == kInvalidHandle) return win::failed(ERROR_INVALID_HANDLE, "listener is invalid");
    SOCKET c = accept(to_sock(listener), nullptr, nullptr);
    if (c == INVALID_SOCKET) return wsa_failed("accept() failed");
    *conn = from_sock(c);
    return win::ok();
}

Result transport_read(Handle conn, void* buf, std::size_t cap, std::size_t* got) {
    if (got) *got = 0;
    if (!got) return win::failed(ERROR_INVALID_PARAMETER, "got is null");
    if (!buf || cap == 0) return win::failed(ERROR_INVALID_PARAMETER, "buf/cap invalid");
    if (conn == kInvalidHandle) return win::failed(ERROR_INVALID_HANDLE, "conn is invalid");
    const int n = recv(to_sock(conn), static_cast<char*>(buf),
                       int(cap > INT_MAX ? INT_MAX : cap), 0);
    if (n == SOCKET_ERROR) return wsa_failed("recv() failed");
    *got = std::size_t(n);  // 0 = orderly shutdown by the peer
    return win::ok();
}

Result transport_write(Handle conn, const void* buf, std::size_t len, std::size_t* put) {
    if (put) *put = 0;
    if (!buf) return win::failed(ERROR_INVALID_PARAMETER, "buf is null");
    if (conn == kInvalidHandle) return win::failed(ERROR_INVALID_HANDLE, "conn is invalid");
    std::size_t sent = 0;
    const char* p = static_cast<const char*>(buf);
    while (sent < len) {
        const std::size_t chunk = len - sent;
        const int n = send(to_sock(conn), p + sent, int(chunk > INT_MAX ? INT_MAX : chunk), 0);
        if (n == SOCKET_ERROR) {
            if (put) *put = sent;
            return wsa_failed("send() failed");
        }
        sent += std::size_t(n);
    }
    if (put) *put = sent;
    return win::ok();
}

Result transport_close(Handle h) {
    if (h == kInvalidHandle) return win::ok();
    if (closesocket(to_sock(h)) == SOCKET_ERROR) return wsa_failed("closesocket() failed");
    return win::ok();
}

}  // namespace os
}  // namespace bridge
