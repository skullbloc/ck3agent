// ck-dup0 — WHICH PATH FED THE DRAIN.
//
// THE AMBIGUITY THIS ENDS. Two paths reach g_msg_queue and the game-thread
// drain, and they emit byte-identical trace lines:
//
//   READER RAW PUSH   ipc_server.cpp:1707 — the client's own envelope, pushed
//                     when the ladder declines it. NO conductor ticket.
//   FLUSH HAND-OFF    pipeline.cpp:681 enqueue_for_drain — a call already
//                     BUFFERED AND TICKETED at submit, which the flush routes
//                     here because action_routes_to_drain() is true for
//                     is_game_thread_action. enqueue_action_envelope's own
//                     comment: "the flush loop does NOT open a second execution
//                     path into the game thread, IT JUST DECIDES WHEN THE
//                     EXISTING ONE IS FED."
//
// Three things made the existing trace unable to tell them apart, and all three
// were absences rather than errors:
//   * drain_dispatch_done prints action, rid, resp_empty and resp_size — not the
//     ticket, not the session;
//   * emit_accepted sends its frame through send_observation and DOES NOT LOG,
//     so "no accepted frame in the log" is not evidence that none was sent;
//   * the `Action: <verb> params:` arrival line is emitted BEFORE the ladder
//     branches, so it is common to both.
// A four-line trace of a game-thread verb is therefore emitted identically
// whichever path it took, and ck-0lph was argued for a day from it.
//
// WHY BOTH A TRACE FIELD AND A COUNTER. The per-action trace is gated on
// CK3_BRIDGE_TRACE because it is per-action volume — that gate is correct and
// stays. But "did ANY game-thread action take the raw push?" is a question whose
// answer is often NO, and a diagnostic behind a flag cannot support a negative
// claim: in an untraced run its silence means nothing. So the COUNTS are
// unconditional and ride the heartbeat, exactly as ck-jh76's reply failures,
// ck-0zfc's drops and ck-475j's refusals do, and a clean run states them as
// zero rather than omitting them.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace bridge {

extern FILE* g_logfile;

inline std::atomic<uint64_t> g_drain_from_flush{0};      // ticketed hand-off
inline std::atomic<uint64_t> g_drain_from_raw_push{0};   // no ticket on the envelope

inline void note_drain_provenance(uint64_t pipeline_ticket) {
    if (pipeline_ticket) g_drain_from_flush.fetch_add(1, std::memory_order_relaxed);
    else                 g_drain_from_raw_push.fetch_add(1, std::memory_order_relaxed);
}

inline const char* format_drain_provenance(char* buf, size_t n) {
    snprintf(buf, n, "drain_from_flush=%llu drain_from_raw_push=%llu",
             (unsigned long long)g_drain_from_flush.load(std::memory_order_relaxed),
             (unsigned long long)g_drain_from_raw_push.load(std::memory_order_relaxed));
    return buf;
}

// Renders the two fields the trace was missing. A zero ticket is spelled "0"
// and NOT omitted — an absent field is indistinguishable from an older binary
// that never had one — and an empty session says so in words rather than
// leaving a blank that reads as attribution nobody checked.
inline const char* format_drain_origin(char* buf, size_t n,
                                       uint64_t pipeline_ticket,
                                       const char* session_id) {
    const bool have_session = session_id && session_id[0];
    if (pipeline_ticket) {
        snprintf(buf, n, "ticket=tk_%08llu session=%s",
                 (unsigned long long)pipeline_ticket,
                 have_session ? session_id
                              : "UNATTRIBUTED (no session_id on the queued envelope)");
    } else {
        snprintf(buf, n, "ticket=0 (reader raw push, no conductor ticket) session=%s",
                 have_session ? session_id
                              : "UNATTRIBUTED (no session_id on the queued envelope)");
    }
    return buf;
}

} // namespace bridge
