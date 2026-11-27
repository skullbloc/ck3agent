// frame_send.h — write one NDJSON frame to a client socket, and the rule for
// which frames may wait for a slow reader.
//
// THE STALL. Every send waited up to g_send_timeout_ms (2 s) for POLLOUT, and the
// backpressure drop (TIOCOUTQ over 8 x 66 KB) never fired on a Unix socket, whose
// send buffer fills long before 528 KB is queued (one recorded run: backpressure_drops=0
// for the whole run). So a client that does not read its socket (that run's slot
// 31: a `ck3agent run` control link idle between its polls, 225 keepalives
// `poll_timeout`; 346 failed pings in all) cost the SENDING thread 2 s on every
// frame addressed to it. The keepalive ping and every broadcast are sent from
// the IPC reader thread or the game thread, the two threads every other client
// waits on: handshakes went unanswered for 8-10 s (the seats' `no handshake ack`,
// D2/D3 never bound), reads timed out (`read_timeout`, `R.read: unread`), and the
// meter's polls raised (180 of 326 samples).
//
// THE RULE. A frame nobody is waiting on (a keepalive ping, a broadcast) is
// BEST EFFORT: it is written only when the socket has room for all of it now,
// and is otherwise dropped whole and counted (stage kBackpressure), so no thread
// ever waits on a reader that is not reading and no stream is left mid-frame. A
// reply to the client that asked (unicast) keeps the bounded wait: that client
// is reading, by definition, and a dropped reply is a lost answer.
//
// Host-tested with a real socketpair by bridge/test/test_frame_send.cpp.
//
// WINDOWS. This header spells the socket calls the POSIX way (poll, send,
// getsockopt). ipc_server.cpp maps those spellings onto Winsock file-locally,
// so it includes this header AFTER that mapping block, never before it.
#pragma once

#include "reply_record.h"

#include <cerrno>
#include <cstddef>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>

namespace bridge {

enum class FrameRoom { Fits, NoRoom, Unknown };

// Whether `len` more bytes (+ the newline) fit in fd's send buffer now: the
// bytes still queued (TIOCOUTQ / SIOCOUTQ) against SO_SNDBUF. Unknown when
// either read fails; the caller then falls back to a zero-wait poll.
inline FrameRoom frame_room(int fd, size_t len) {
    int queued = 0, sndbuf = 0;
    socklen_t sl = sizeof(sndbuf);
    if (ioctl(fd, TIOCOUTQ, &queued) != 0) return FrameRoom::Unknown;
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char*>(&sndbuf), &sl) != 0 || sndbuf <= 0)
        return FrameRoom::Unknown;
    // A Unix socket accounts queued skbs with their overhead, so the free space
    // is under-estimated: a frame said to fit does fit.
    const long free_bytes = static_cast<long>(sndbuf) - static_cast<long>(queued);
    return (free_bytes > static_cast<long>(len) + 1) ? FrameRoom::Fits : FrameRoom::NoRoom;
}

// A pollfd asking for POLLOUT on fd. Field-wise, not a braced initialiser: Winsock's
// pollfd.fd is a SOCKET (unsigned 64-bit) and an int there is a narrowing error.
inline struct pollfd pollout_fd(int fd) {
    struct pollfd p{};
    p.fd = static_cast<decltype(p.fd)>(fd);
    p.events = POLLOUT;
    return p;
}

// Write json_data + '\n' to a NON-BLOCKING fd. `wait_ms` bounds each wait for
// POLLOUT. `best_effort`: refuse (kBackpressure, nothing written) unless the
// whole frame has room now, and never wait before the first byte. Returns true
// iff the whole frame was written; `out` (optional) says which exit fired.
inline bool write_frame(int fd, const char* json_data, size_t len, int wait_ms,
                        bool best_effort, SendOutcome* out = nullptr) {
    auto fail = [&](SendStage st, size_t written, int e) {
        if (out) { out->stage = st; out->bytes_written = written; out->bytes_total = len; out->err = e; }
        return false;
    };
    if (out) out->bytes_total = len;
    if (best_effort) {
        const FrameRoom room = frame_room(fd, len);
        if (room == FrameRoom::NoRoom) return fail(SendStage::kBackpressure, 0, 0);
        if (room == FrameRoom::Unknown) {
            struct pollfd p0 = pollout_fd(fd);
            if (poll(&p0, 1, 0) <= 0 || !(p0.revents & POLLOUT))
                return fail(SendStage::kBackpressure, 0, 0);
        }
    }
    ssize_t sent = 0;
    while (sent < static_cast<ssize_t>(len)) {
        struct pollfd pfd = pollout_fd(fd);
        int pr = poll(&pfd, 1, wait_ms);
        if (pr <= 0 || !(pfd.revents & POLLOUT))
            return fail(sent > 0 ? SendStage::kSendError : SendStage::kPollTimeout, (size_t)sent, 0);
        ssize_t n = send(fd, json_data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return fail(SendStage::kSendError, (size_t)sent, errno);
        }
        sent += n;
    }
    struct pollfd pfd = pollout_fd(fd);
    int pr = poll(&pfd, 1, wait_ms);
    if (pr <= 0 || !(pfd.revents & POLLOUT)) return fail(SendStage::kNewlineFailed, (size_t)sent, 0);
    char nl = '\n';
    if (send(fd, &nl, 1, MSG_NOSIGNAL) <= 0) return fail(SendStage::kNewlineFailed, (size_t)sent, errno);
    if (out) { out->stage = SendStage::kOk; out->bytes_written = (size_t)sent; }
    return true;
}

}  // namespace bridge
