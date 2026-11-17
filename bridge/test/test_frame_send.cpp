// test_frame_send.cpp — fix10: a frame nobody waits on never waits on a client
// that is not reading (frame_send.h). A real AF_UNIX socketpair, the kind of
// socket the bridge serves, filled until it is not writable.
//
// Rows:
//  1. an empty socket: a best-effort frame is written whole (kOk).
//  2. a full socket: a best-effort frame is refused at once (< 50 ms), stage
//     kBackpressure, NOTHING written (the stream stays clean).
//  3. CONTROL (the pre-fix behaviour, kept for replies): the same full socket
//     with best_effort=false waits its full bound and fails kPollTimeout. On
//     5e506f28 every frame took this path, pings and broadcasts included
//     (council-a: 346 pings, 2 s each, on the IPC reader thread).
//  4. after the reader drains, a best-effort frame goes through again.
#include "frame_send.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static long ms_since(std::chrono::steady_clock::time_point t0) {
    return (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
}

int main() {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { std::perror("socketpair"); return 2; }
    const int fd = sv[0];
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    const std::string frame = "{\"type\":\"ping\"}";

    // 1. empty socket
    {
        bridge::SendOutcome o;
        CHECK(bridge::write_frame(fd, frame.data(), frame.size(), 100, true, &o));
        CHECK(o.stage == bridge::SendStage::kOk);
        char buf[64]; ssize_t n = read(sv[1], buf, sizeof buf);
        CHECK(n == (ssize_t)frame.size() + 1);
    }
    // fill: nobody reads sv[1]
    {
        std::string blob(4096, 'x');
        long total = 0;
        for (;;) {
            ssize_t n = send(fd, blob.data(), blob.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n <= 0) break;
            total += n;
        }
        CHECK(total > 0);
        CHECK(bridge::frame_room(fd, frame.size()) == bridge::FrameRoom::NoRoom);
    }
    // 2. best effort on a full socket: refused at once, nothing written
    {
        bridge::SendOutcome o;
        auto t0 = std::chrono::steady_clock::now();
        CHECK(!bridge::write_frame(fd, frame.data(), frame.size(), 2000, true, &o));
        const long ms = ms_since(t0);
        CHECK(ms < 50);
        CHECK(o.stage == bridge::SendStage::kBackpressure);
        CHECK(o.bytes_written == 0);
        std::printf("row 2: best effort on a full socket: %ld ms, stage %s\n", ms, bridge::send_stage_name(o.stage));
    }
    // 3. CONTROL: a reply (not best effort) waits its bound, then poll_timeout
    {
        bridge::SendOutcome o;
        auto t0 = std::chrono::steady_clock::now();
        CHECK(!bridge::write_frame(fd, frame.data(), frame.size(), 300, false, &o));
        const long ms = ms_since(t0);
        CHECK(ms >= 250);
        CHECK(o.stage == bridge::SendStage::kPollTimeout);
        std::printf("row 3 (control): a reply waits %ld ms, stage %s\n", ms, bridge::send_stage_name(o.stage));
    }
    // 4. the reader drains: best effort goes through
    {
        fcntl(sv[1], F_SETFL, fcntl(sv[1], F_GETFL, 0) | O_NONBLOCK);
        char buf[65536];
        while (read(sv[1], buf, sizeof buf) > 0) {}
        bridge::SendOutcome o;
        CHECK(bridge::write_frame(fd, frame.data(), frame.size(), 100, true, &o));
        CHECK(o.stage == bridge::SendStage::kOk);
    }
    close(sv[0]); close(sv[1]);
    if (g_fail) { std::fprintf(stderr, "%d check(s) failed\n", g_fail); return 1; }
    std::printf("test_frame_send: all rows passed\n");
    return 0;
}
