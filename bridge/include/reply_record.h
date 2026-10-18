// ck-jh76 — one spelling of "what happened to a reply", shared by every send site.
//
// WHY THIS EXISTS. arma7 timed out on get_tick_info with the request RECEIVED,
// 77 further actions served immediately after, and the game thread healthy
// throughout (ck-zgh4). The bridge log said nothing about the reply — and the
// reason is that the reply-path records were gated on `bridge::g_trace`, set
// only from CK3_BRIDGE_TRACE=1, which NOTHING in the repo sets and whose own
// comment says "Default OFF".
//
// So the absence of a reply record was not evidence about the reply. It was a
// disabled instrument, indistinguishable from an instrument that ran and found
// nothing — the same failure the fault-site and in-flight beads were written to
// end, arriving this time as a RUNTIME gate rather than a missing code path.
//
// THE LABEL COLLISION THAT HID IT. ADLOG (action_dispatch_internal.h) is gated
// on nothing but g_logfile, and several of its messages begin "BT_TRACE". So a
// log containing "BT_TRACE do_send_interaction ..." looks like proof that
// BT_TRACE is on while the g_trace-gated reply records are silent. Two
// instruments, one label, different switches. Reading the label instead of the
// switch is what made the silence look like a measurement.
//
// THE RULE APPLIED HERE: a FAILED or EMPTY reply is recorded UNCONDITIONALLY
// (g_logfile only, like the SLOW REQUEST watcher), because a timeout is exactly
// when the record is needed and an instrument you must have predicted you would
// need is not an instrument. Per-action SUCCESS tracing stays behind
// CK3_BRIDGE_TRACE, since that is the high-volume case the gate was added for.
//
// AND SO THAT SILENCE STAYS READABLE: the counters below are always-on and are
// printed by the accept-thread heartbeat. "No failure line" then means
// something, because the heartbeat shows whether replies were being attempted
// at all. Without that, "served and dropped" and "instrument never ran" look
// identical again — which is the whole defect this header exists to close.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "log_stamp.h"

namespace bridge {

extern FILE* g_logfile;
extern bool  g_trace;

// WHICH EXIT of send_to_slot fired. "Not delivered" is not one defect: a slot
// that closed is a lost REPLY, while a failure partway through the write leaves
// the client's STREAM mid-message — and the driver is line-framed NDJSON
// (bridge_client.py accumulates into _buf and splits on '\n'), so a body
// written without its trailing newline is not a dropped message at all. The
// client holds a complete JSON object that its reader will never yield, waits
// out the full deadline, and reports a timeout; the NEXT reply then appends to
// that unterminated line and the two concatenate into one unparseable frame.
//
// One lost reply and a desynchronised stream need different fixes, so the
// record names the stage rather than the verdict.
enum class SendStage : uint8_t {
    kOk = 0,
    kSlotClosed,      // released or gen-bumped since the snapshot — reply lost, stream clean
    kBackpressure,    // TIOCOUTQ over threshold — reply dropped, stream clean
    kPollTimeout,     // no POLLOUT within g_send_timeout_ms, BEFORE any byte — stream clean
    kSendError,       // send() failed partway — STREAM MID-MESSAGE if bytes_written > 0
    kNewlineFailed,   // whole body written, terminator not — STREAM MID-MESSAGE, always
};

inline const char* send_stage_name(SendStage st) {
    switch (st) {
        case SendStage::kOk:            return "ok";
        case SendStage::kSlotClosed:    return "slot_closed";
        case SendStage::kBackpressure:  return "backpressure";
        case SendStage::kPollTimeout:   return "poll_timeout";
        case SendStage::kSendError:     return "send_error";
        case SendStage::kNewlineFailed: return "newline_failed";
    }
    return "unknown";
}

// Filled by send_to_slot. bytes_written is how far the body got, so a reader can
// tell a clean drop from a truncated frame without inferring it from the stage.
struct SendOutcome {
    SendStage stage         = SendStage::kOk;
    size_t    bytes_written = 0;
    size_t    bytes_total   = 0;
    int       err           = 0;
    // kBackpressure only: TIOCOUTQ depth at the drop. arma7's single logged
    // drop was over the threshold by 1024 bytes — about 3%, grazed rather than
    // blown through — which is the difference between a flood and a limit that
    // a modest reply-size increase would cross routinely. The margin is the
    // diagnostic, so it is recorded rather than left to the throttled line.
    int       pending_bytes = -1;   // -1 = not a backpressure outcome
    // ck-rcrr: the threshold ACTUALLY APPLIED, carried rather than restated.
    // This header previously hardcoded 32768 to compute the margin, so raising
    // the limit in ipc_server.cpp would have left the record reporting a margin
    // against a number no longer in force — a second declaration of one value,
    // and the reader would have had no way to tell.
    int       threshold_bytes = -1;
};

// ck-55dx: an UNKNOWN elapsed must not print as 0. A zero there is a claim that
// the reply was instant; absence gets its own spelling (ck-duv5).
inline constexpr uint64_t kElapsedUnknown = ~0ull;

inline std::atomic<uint64_t> g_reply_attempted{0};
inline std::atomic<uint64_t> g_reply_failed{0};
inline std::atomic<uint64_t> g_reply_empty{0};

// ck-0zfc: EVERY backpressure drop is counted here, including the ones the
// verbose line throttles away. arma7 logged exactly one drop — "1 total
// dropped", slot 0, 33792 bytes against a 32768 threshold — and the throttle in
// send_to_slot logs only the 1st and every 100th. That counter is a single
// function-level static, so it is shared across EVERY slot, EVERY session and
// EVERY send path: drop #1 on one agent's socket silences the next 98 drops
// anywhere in the process. Six agents were running. "1 total dropped" therefore
// bounds nothing, and this counter is what makes the real number legible.
inline std::atomic<uint64_t> g_backpressure_drops{0};

// ck-0zfc: KEEPALIVE sends are replies too, for framing purposes. The accept
// loop pings every live slot every ~5s through send_to_slot, which appends its
// own newline — so a ping COMPLETES an orphaned frame left by a failed newline
// send, unprompted, without the client asking for anything. That makes a ping's
// success or failure diagnostically load-bearing: if pings were landing on a
// stuck slot and no parse error followed, the orphan theory is in trouble; if
// the pings were failing too, the socket was unwritable and nothing can be
// concluded from the client's silence.
//
// Pings went through send_to_slot capturing NO outcome and were invisible even
// with CK3_BRIDGE_TRACE on, because the trace lives at the reply site and a
// ping is not a reply. Counted here so the question is answerable at all.
inline std::atomic<uint64_t> g_ping_sent{0};
inline std::atomic<uint64_t> g_ping_failed{0};

inline void record_ping(int slot, bool ok, const SendOutcome& out) {
    g_ping_sent.fetch_add(1, std::memory_order_relaxed);
    if (ok) return;
    g_ping_failed.fetch_add(1, std::memory_order_relaxed);
    if (g_logfile) {
        fprintf(g_logfile,
            "[CK3-Bridge] KEEPALIVE NOT DELIVERED: slot=%d stage=%s wrote=%zu/%zu "
            "— this slot is unwritable, so the client's silence says nothing "
            "about what the bridge sent it (ck-0zfc)\n",
            slot, send_stage_name(out.stage), out.bytes_written, out.bytes_total);
        fflush(g_logfile);
    }
}

// KEEP THE FIRST FAILURE across a broadcast's slots. A later slot succeeding
// must not erase an earlier slot's drop: a broadcast that reached one of six
// clients would otherwise report clean while five were dropped.
//
// THIS LIVES HERE, AS A FUNCTION, ON PURPOSE. Written inline in the loop it was
// a condition an acceptance could only grep for — and a grep for the condition's
// TEXT stays green when the condition is defeated. My own mutant proved it:
// inserting `true ||` ahead of the test left the string intact and the row
// passing. One declaration, one reader, and a probe that CALLS it.
inline void merge_broadcast_outcome(SendOutcome* dst, const SendOutcome& src) {
    if (dst && dst->stage == SendStage::kOk) *dst = src;
}

// A reply was produced and handed to send_observation.
//   target_slot >= 0 -> targeted at the requesting client
//   target_slot <  0 -> BROADCAST; note that send_observation returns `any`
//                       there, so sent_ok=1 does NOT mean the REQUESTING client
//                       received it. Spelled out in the line rather than left
//                       for the reader to remember.
// Renders "elapsed=NNNms" or "elapsed=UNKNOWN(<why>)" — never a bare 0.
inline const char* format_elapsed(char* buf, size_t n, uint64_t elapsed_ms) {
    if (elapsed_ms == kElapsedUnknown)
        snprintf(buf, n, "elapsed=UNKNOWN(no stable in-flight record)");
    else
        snprintf(buf, n, "elapsed=%llums", (unsigned long long)elapsed_ms);
    return buf;
}

inline void record_reply(const char* action, const char* rid, bool sent_ok,
                         size_t bytes, int target_slot, uint32_t target_gen,
                         const SendOutcome* out = nullptr,
                         uint64_t elapsed_ms = kElapsedUnknown) {
    g_reply_attempted.fetch_add(1, std::memory_order_relaxed);
    if (!sent_ok) g_reply_failed.fetch_add(1, std::memory_order_relaxed);

    // UNCONDITIONAL on failure. Rare by construction, so no volume argument.
    if (!sent_ok && g_logfile) {
        if (target_slot >= 0) {
            // The stream verdict is DERIVED from bytes_written, not from the
            // stage alone: a send_error at byte 0 leaves the stream clean, the
            // same stage at byte 1 does not.
            const bool mid_message =
                out && (out->stage == SendStage::kNewlineFailed ||
                        (out->stage == SendStage::kSendError && out->bytes_written > 0));
            char ts[32], el[64];
            fprintf(g_logfile,
                "%s[CK3-Bridge] REPLY NOT DELIVERED: action=%s rid=%s bytes=%zu "
                "%s target=slot %d gen %u stage=%s wrote=%zu/%zu errno=%d "
                "stream=%s (ck-jh76/ck-55dx)\n",
                monotonic_stamp(ts, sizeof ts),
                action ? action : "?", rid ? rid : "?", bytes,
                format_elapsed(el, sizeof el, elapsed_ms), target_slot,
                (unsigned)target_gen,
                out ? send_stage_name(out->stage) : "UNRECORDED",
                out ? out->bytes_written : 0, out ? out->bytes_total : bytes,
                out ? out->err : 0,
                !out ? "UNKNOWN (no outcome captured)"
                     : mid_message ? "MID-MESSAGE — the client holds a partial "
                                     "frame; its line reader will not yield it "
                                     "and the NEXT reply concatenates onto it"
                                   : "clean (nothing written)");
        if (out && out->stage == SendStage::kBackpressure) {
            fprintf(g_logfile,
                "[CK3-Bridge]   backpressure detail: rid=%s pending=%d "
                "threshold=%d over by %d bytes (%.1f%%); process-wide drops so "
                "far=%llu — the verbose drop line is throttled to the 1st and "
                "every 100th ACROSS ALL SLOTS, so it is not a count "
                "(ck-0zfc/ck-rcrr)\n",
                rid ? rid : "?", out->pending_bytes, out->threshold_bytes,
                out->pending_bytes - out->threshold_bytes,
                out->threshold_bytes > 0
                    ? 100.0 * (out->pending_bytes - out->threshold_bytes)
                          / out->threshold_bytes
                    : 0.0,
                (unsigned long long)g_backpressure_drops.load(
                    std::memory_order_relaxed));
        }
        } else {
            fprintf(g_logfile,
                "[CK3-Bridge] REPLY NOT DELIVERED: action=%s rid=%s bytes=%zu "
                "target=BROADCAST (no client accepted it; note a broadcast "
                "sent_ok only ever meant SOME client, never the requester) "
                "(ck-jh76)\n",
                action ? action : "?", rid ? rid : "?", bytes);
        }
        fflush(g_logfile);
    } else if (sent_ok && g_trace && g_logfile) {
        // High-volume success path keeps its existing gate.
        char ts[32], el[64];
        fprintf(g_logfile,
            "%s[CK3-Bridge] BT_TRACE send_observation: action=%s rid=%s sent_ok=1 "
            "bytes=%zu %s target=%s\n",
            monotonic_stamp(ts, sizeof ts),
            action ? action : "?", rid ? rid : "?", bytes,
            format_elapsed(el, sizeof el, elapsed_ms),
            target_slot >= 0 ? "targeted" : "broadcast");
        fflush(g_logfile);
    }
}

// A dispatch produced NO reply at all. Always recorded: an action that answers
// nothing is indistinguishable, from the client's side, from one that hung.
inline void record_empty_reply(const char* action, const char* rid) {
    g_reply_empty.fetch_add(1, std::memory_order_relaxed);
    if (g_logfile) {
        fprintf(g_logfile,
            "[CK3-Bridge] REPLY EMPTY (nothing sent; the client will wait out "
            "its deadline): action=%s rid=%s (ck-jh76)\n",
            action ? action : "?", rid ? rid : "?");
        fflush(g_logfile);
    }
}

// The GAME-THREAD drain reply (ck-n7n0's two-stage delivery): try the recorded
// session id, else fall back to a broadcast. There is no target SLOT here, so
// this does not borrow the slot spelling — printing slot=-1 for a path that
// never had a slot would be a measured-looking value for something that was
// never measured. What matters diagnostically is WHICH leg delivered, because
// a session-id miss followed by a broadcast is a different defect from both
// legs failing.
inline void record_reply_drain(const char* action, const char* rid, bool sent_ok,
                               size_t bytes, bool had_session_id) {
    g_reply_attempted.fetch_add(1, std::memory_order_relaxed);
    if (!sent_ok) g_reply_failed.fetch_add(1, std::memory_order_relaxed);
    if (!sent_ok && g_logfile) {
        fprintf(g_logfile,
            "[CK3-Bridge] REPLY NOT DELIVERED (game-thread drain): action=%s "
            "rid=%s bytes=%zu session_id=%s — both the session-id leg and the "
            "broadcast fallback failed (ck-jh76)\n",
            action ? action : "?", rid ? rid : "?", bytes,
            had_session_id ? "present" : "ABSENT");
        fflush(g_logfile);
    } else if (sent_ok && g_trace && g_logfile) {
        fprintf(g_logfile,
            "[CK3-Bridge] BT_TRACE drain_send_observation: action=%s rid=%s "
            "sent_ok=1 bytes=%zu session_id=%s\n",
            action ? action : "?", rid ? rid : "?", bytes,
            had_session_id ? "present" : "ABSENT");
        fflush(g_logfile);
    }
}

// Rendered into the accept-thread heartbeat so a QUIET log still says whether
// replies were flowing. Absence of failures is only meaningful beside a
// non-zero attempt count.
inline char* format_reply_counters(char* buf, size_t n) {
    snprintf(buf, n, "replies_attempted=%llu reply_failures=%llu reply_empty=%llu "
             "backpressure_drops=%llu ping_sent=%llu ping_failed=%llu",
             (unsigned long long)g_reply_attempted.load(std::memory_order_relaxed),
             (unsigned long long)g_reply_failed.load(std::memory_order_relaxed),
             (unsigned long long)g_reply_empty.load(std::memory_order_relaxed),
             (unsigned long long)g_backpressure_drops.load(std::memory_order_relaxed),
             (unsigned long long)g_ping_sent.load(std::memory_order_relaxed),
             (unsigned long long)g_ping_failed.load(std::memory_order_relaxed));
    return buf;
}

} // namespace bridge
