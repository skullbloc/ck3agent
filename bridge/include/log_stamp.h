// ck-55dx — ONE spelling of the bridge log's wall-clock prefix.
//
// WHY THIS EXISTS. ck-zgh4 tried to measure per-action bridge latency from
// arma7 and could not, then tried from arma8's 204MB of preserved trace and
// still could not. The reason is structural: blog() (bridge_init.cpp) stamps
// CLOCK_MONOTONIC as "[%5ld.%03ld] " and is declared `static`, so ONLY that
// translation unit can call it. Every other log path — the BT_TRACE
// dispatch_done and send_observation records, the Action: lines, the
// backpressure line, the ck-jh76 reply records — uses raw fprintf with no
// timestamp at all.
//
// So the sparse "[37162.342]" stamps in a bridge log are not a sampling
// decision. They are an artefact of which FILE a line happens to be emitted
// from. The trace we arm to diagnose a hang records WHICH action was served and
// WHETHER its reply was written, and nothing whatever about WHEN.
//
// THE FORMAT IS COPIED EXACTLY, and that is the point: a stamp on a different
// clock or in a different shape would be a second timeline nobody could join to
// the first. blog() is refactored to call this, so there is one spelling and a
// change to it moves both.
#pragma once

#include <cstdio>
#include <ctime>

namespace bridge {

// Writes "[%5ld.%03ld] " — CLOCK_MONOTONIC seconds mod 100000, milliseconds.
// Identical to what blog() has always emitted, so new lines land on the same
// timeline as the ones already in every archived bundle.
inline const char* monotonic_stamp(char* buf, size_t n) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    snprintf(buf, n, "[%5ld.%03ld] ",
             (long)(ts.tv_sec % 100000), (long)(ts.tv_nsec / 1000000));
    return buf;
}

} // namespace bridge
