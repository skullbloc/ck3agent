// observation_route.h — WHERE DOES AN OBSERVATION FRAME GO? (ck-h4pn)
//
// THE DEFECT. send_observation routes by a THREAD-LOCAL set by the reader thread:
//
//     ipc_server.cpp:167    static thread_local int g_send_target_slot = -1;
//     ipc_server.cpp:1580   g_send_target_slot = idx;        <- window opens
//     ipc_server.cpp:1793   } else if (try_inline_safe_query(line_buf, ...)) {
//     ipc_server.cpp:1842   g_send_target_slot = -1;         <- window closes
//
// so a reply routes back to the client that asked. The game thread leaves it unset
// and therefore BROADCASTS. Both halves are correct about themselves, and both are
// documented at their own site.
//
// emit_management_ledger_record is called from INSIDE try_inline_safe_query — so it
// runs on the reader thread, inside that window, and its frame unicast to the
// CALLING AGENT. The ARM relay drains the OBSERVER socket and never saw it: zero of
// roughly 386 management calls in ep-02, under any spelling, with nothing anywhere
// reporting a failure. The frame was never malformed; ck-9rto drove the relay with
// the exact shape and it was accepted. Only the destination was wrong.
//
// WHY A PREDICATE AND NOT AN `if`. The routing choice was an implicit consequence of
// which thread happened to be running, invisible at every call site. Written inline
// it is a condition a test can only grep for. `must_broadcast` is the input the old
// code did not have, and the truth table below is what proves it WINS over a set
// target slot — a signature that could not express the wrong rule could not be
// tested against it (the ck-48z8/ck-7yrp pattern).
//
// AND WHY IT IS NOT "JUST CLEAR THE THREAD-LOCAL AROUND THE SEND". A save/restore is
// three lines whose correctness depends on the next reader noticing why they are
// there, in a function whose whole behaviour is a thread-local invisible at the call
// site — which is the property that caused the bug. A named route says what it does.
#pragma once

#include <cstdint>

namespace bridge {

enum class ObsRoute : uint8_t {
    Unicast   = 0,   //: back to the requesting client, via the reader's target slot
    Broadcast = 1,   //: to every live client, which then filter by request_id
};

//: `must_broadcast` is the CALLER's statement that this frame is an OBSERVATION for
//: whoever is listening, not a reply to whoever asked. It wins over target_slot,
//: because the whole defect was a frame that was semantically a broadcast being
//: routed as a reply by a thread-local it never consulted.
inline ObsRoute observation_route(int target_slot, bool must_broadcast) {
    if (must_broadcast) return ObsRoute::Broadcast;
    return target_slot >= 0 ? ObsRoute::Unicast : ObsRoute::Broadcast;
}

}  // namespace bridge
