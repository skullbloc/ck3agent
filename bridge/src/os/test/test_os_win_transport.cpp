// test_os_win_transport.cpp — the os::transport cell on Windows, engine-free:
// a loopback listener, a client connecting from a second thread, an NDJSON
// line sent each way through transport_read/write, and an orderly close that
// transport_read reports as 0 bytes. Runs under wine.
//
// The listener Handle is a SOCKET; the test reads the OS-assigned port back
// with getsockname (ipc_endpoint reports port 0 before a listen, so the test
// must discover the real one, exactly as endpoint.json will in P4).

#include "os/os.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace os = bridge::os;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("FAIL: %s\n", what); }
    else std::printf("ok:   %s\n", what);
}

int main() {
    os::Endpoint ep{os::TransportKind::none, ""};
    check(os::succeeded(os::ipc_endpoint(&ep)) && ep.kind == os::TransportKind::tcp_loopback &&
              std::strcmp(ep.address, "127.0.0.1:0") == 0,
          "ipc_endpoint: tcp_loopback, OS-assigned port");

    os::Handle listener = os::kInvalidHandle;
    os::Endpoint at{os::TransportKind::tcp_loopback, "127.0.0.1:0"};
    check(os::succeeded(os::transport_listen(at, &listener)) && listener != os::kInvalidHandle,
          "transport_listen: bound and listening on an ephemeral loopback port");

    // The listener Handle is a SOCKET; read the assigned port back.
    sockaddr_in bound;
    int blen = sizeof(bound);
    check(getsockname(static_cast<SOCKET>(listener), reinterpret_cast<sockaddr*>(&bound), &blen) == 0,
          "getsockname on the listener");
    const unsigned short port = ntohs(bound.sin_port);
    check(port != 0, "an ephemeral port was assigned");

    // Reject the wrong transport kind and a bad address.
    os::Handle bad = os::kInvalidHandle;
    os::Endpoint wrong{os::TransportKind::af_unix, "/x"};
    check(os::transport_listen(wrong, &bad).status == os::Status::failed && bad == os::kInvalidHandle,
          "transport_listen: af_unix is refused on Windows");
    os::Endpoint badaddr{os::TransportKind::tcp_loopback, "10.0.0.1:0"};
    check(os::transport_listen(badaddr, &bad).status == os::Status::failed,
          "transport_listen: a non-loopback host is refused");

    // A client connects from another thread and echoes one line back.
    std::thread client([port] {
        SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a;
        std::memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        InetPtonA(AF_INET, "127.0.0.1", &a.sin_addr);
        if (connect(c, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
            char buf[256];
            int n = recv(c, buf, sizeof(buf), 0);
            if (n > 0) send(c, buf, n, 0);  // echo
        }
        closesocket(c);
    });

    os::Handle conn = os::kInvalidHandle;
    check(os::succeeded(os::transport_accept(listener, &conn)) && conn != os::kInvalidHandle,
          "transport_accept: the client connection arrives");

    const std::string line = "{\"cmd\":\"ping\"}\n";
    std::size_t put = 0;
    check(os::succeeded(os::transport_write(conn, line.data(), line.size(), &put)) &&
              put == line.size(),
          "transport_write: the whole NDJSON line is sent");

    std::string got;
    char buf[64];
    std::size_t n = 0;
    bool read_ok = true;
    while (got.size() < line.size()) {
        os::Result r = os::transport_read(conn, buf, sizeof(buf), &n);
        if (!os::succeeded(r) || n == 0) { read_ok = false; break; }
        got.append(buf, n);
    }
    check(read_ok && got == line, "transport_read: the echoed line comes back whole");

    client.join();
    // The peer has closed; the next read is an orderly 0.
    n = 7;
    os::Result r = os::transport_read(conn, buf, sizeof(buf), &n);
    check(os::succeeded(r) && n == 0, "transport_read: orderly peer close reads 0 bytes");

    check(os::succeeded(os::transport_close(conn)), "transport_close: connection");
    check(os::succeeded(os::transport_close(listener)), "transport_close: listener");
    check(os::succeeded(os::transport_close(os::kInvalidHandle)), "transport_close(invalid): ok, no-op");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
