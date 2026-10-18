// ck-jh76 — does a FAILED reply record itself without CK3_BRIDGE_TRACE?
//
// That is the entire bead. Before this, every reply-path record was gated on
// bridge::g_trace (CK3_BRIDGE_TRACE=1, "Default OFF", set by nothing in the
// repo), so arma7's dropped reply left no trace and the silence was mistaken
// for a measurement.
//
// Includes the SHIPPED bridge/include/reply_record.h and calls the shipped
// functions — no local copy of the logic. (ck-bl15's probe claimed that while
// carrying a faithful copy; a copy is true of today's tree and guarantees
// nothing about tomorrow's.)
//
// Exit 0 pass / 1 FAIL / 3 NOT EVALUABLE.
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

namespace bridge { FILE* g_logfile = nullptr; bool g_trace = false; }

#include "reply_record.h"

static int failures = 0;
static void row(const char* name, bool ok, const char* detail) {
    printf("%-44s %s  %s\n", name, ok ? "PASS" : "FAIL", detail);
    if (!ok) failures++;
}

static std::string capture_out(const char* action, const char* rid, size_t bytes,
                               int slot, uint32_t gen, const bridge::SendOutcome* o);

// Capture whatever the shipped functions write, with the trace gate OFF.
template <typename F>
static std::string capture(F fn) {
    char* buf = nullptr; size_t sz = 0;
    FILE* f = open_memstream(&buf, &sz);
    if (!f) return "<memstream failed>";
    bridge::g_logfile = f;
    fn();
    fflush(f); fclose(f);
    bridge::g_logfile = nullptr;
    std::string out(buf ? buf : ""); free(buf);
    return out;
}

static std::string capture_out(const char* action, const char* rid, size_t bytes,
                               int slot, uint32_t gen, const bridge::SendOutcome* o) {
    char* buf = nullptr; size_t sz = 0;
    FILE* f = open_memstream(&buf, &sz);
    if (!f) return "<memstream failed>";
    bridge::g_logfile = f;
    bridge::record_reply(action, rid, /*sent_ok=*/false, bytes, slot, gen, o);
    fflush(f); fclose(f);
    bridge::g_logfile = nullptr;
    std::string out(buf ? buf : ""); free(buf);
    return out;
}

int main() {
    bridge::g_trace = false;            // the SHIPPED default, and arma7's state

    // 1. THE BEAD. A failed targeted reply must record with the gate OFF.
    std::string s = capture([]{
        bridge::record_reply("get_tick_info", "6a8f5cd6", /*sent_ok=*/false,
                             /*bytes=*/128, /*slot=*/3, /*gen=*/7);
    });
    row("failed_reply_records_without_trace",
        s.find("REPLY NOT DELIVERED") != std::string::npos,
        "g_trace=false, sent_ok=false -> line emitted");
    row("failed_reply_names_the_request",
        s.find("get_tick_info") != std::string::npos &&
        s.find("6a8f5cd6") != std::string::npos,
        "action and rid both present");
    row("failed_reply_names_the_target",
        s.find("slot 3") != std::string::npos && s.find("gen 7") != std::string::npos,
        "targeted slot+gen, not a bare -1");

    // 2. An EMPTY reply is the other way a client waits out its deadline.
    s = capture([]{ bridge::record_empty_reply("query_armies", "deadbeef"); });
    row("empty_reply_records_without_trace",
        s.find("REPLY EMPTY") != std::string::npos &&
        s.find("deadbeef") != std::string::npos,
        "g_trace=false -> line emitted");

    // 3. The drain path must NOT borrow the slot spelling for a path that has
    //    no slot, and must say which leg was available.
    s = capture([]{
        bridge::record_reply_drain("send_interaction", "cafe01", false, 64,
                                   /*had_session_id=*/false);
    });
    row("drain_failure_records_without_trace",
        s.find("REPLY NOT DELIVERED (game-thread drain)") != std::string::npos,
        "distinct spelling for the two-stage path");
    row("drain_absent_session_is_named_not_faked",
        s.find("session_id=ABSENT") != std::string::npos &&
        s.find("slot") == std::string::npos,
        "says ABSENT; never prints a slot it never had");

    // 3b. WHICH EXIT FIRED — the difference between a lost reply and a
    //     corrupted stream. The driver is line-framed NDJSON, so a body written
    //     without its newline is NOT a dropped message: the client holds a
    //     complete JSON object its reader will never yield.
    {
        bridge::SendOutcome o;
        o.stage = bridge::SendStage::kNewlineFailed;
        o.bytes_written = 128; o.bytes_total = 128; o.err = 32;
        std::string t = capture_out("get_tick_info", "6a8f5cd6", 128, 3, 7, &o);
        row("newline_failure_is_flagged_mid_message",
            t.find("stage=newline_failed") != std::string::npos &&
            t.find("MID-MESSAGE") != std::string::npos,
            "body written, terminator not -> stream desynchronised");
        row("newline_failure_reports_progress",
            t.find("wrote=128/128") != std::string::npos,
            "how far the body got is stated, not inferred");
    }
    {
        // Same stage, different byte count, OPPOSITE stream verdict. This is
        // the row that proves the verdict is derived from bytes_written rather
        // than read off the stage name.
        bridge::SendOutcome o;
        o.stage = bridge::SendStage::kSendError;
        o.bytes_written = 0; o.bytes_total = 64; o.err = 104;
        std::string clean = capture_out("q", "r1", 64, 1, 1, &o);
        o.bytes_written = 17;
        std::string dirty = capture_out("q", "r2", 64, 1, 1, &o);
        row("send_error_at_byte_zero_is_clean",
            clean.find("stream=clean") != std::string::npos,
            "nothing written -> stream intact");
        row("send_error_midway_is_mid_message",
            dirty.find("MID-MESSAGE") != std::string::npos &&
            dirty.find("wrote=17/64") != std::string::npos,
            "same stage, 17 bytes in -> stream corrupted");
    }
    {
        // ck-duv5: an uncaptured outcome must SAY so, never read as a clean drop.
        std::string t = capture_out("q", "r3", 64, 1, 1, nullptr);
        row("uncaptured_outcome_says_unrecorded",
            t.find("stage=UNRECORDED") != std::string::npos &&
            t.find("UNKNOWN (no outcome captured)") != std::string::npos,
            "absence has its own spelling");
    }

    // 3c. ck-0zfc — BACKPRESSURE, the exit arma7 shows actually fired. The
    //     verbose drop line is throttled to the 1st and every 100th across ALL
    //     slots, so the record must carry the margin and a count that cannot be
    //     silenced.
    {
        const uint64_t before = bridge::g_backpressure_drops.load();
        bridge::g_backpressure_drops.fetch_add(1, std::memory_order_relaxed);
        bridge::SendOutcome o;
        o.stage = bridge::SendStage::kBackpressure;
        o.bytes_written = 0; o.bytes_total = 512; o.pending_bytes = 33792;
        o.threshold_bytes = 32768;   // arma7's threshold, to reproduce its line
        std::string t = capture_out("get_tick_info", "6a8f5cd6", 512, 0, 1, &o);
        row("backpressure_is_named_as_the_stage",
            t.find("stage=backpressure") != std::string::npos,
            "the exit is identified, not just 'not delivered'");
        row("backpressure_drop_is_clean_not_mid_message",
            t.find("stream=clean") != std::string::npos,
            "dropped before any byte -> stream stays framed (matches arma7: "
            "a timeout with ZERO JSONDecodeError)");
        row("backpressure_reports_the_margin",
            t.find("pending=33792") != std::string::npos &&
            t.find("over by 1024 bytes (3.1%)") != std::string::npos &&
            t.find("threshold=32768") != std::string::npos,
            "grazed by 3.1%, not flooded — margin AND the threshold in force");
        row("backpressure_count_cannot_be_silenced",
            bridge::g_backpressure_drops.load() == before + 1 &&
            t.find("process-wide drops so far=") != std::string::npos,
            "counted even when the verbose line is throttled away");
        row("record_names_the_rid_the_old_line_could_not",
            t.find("6a8f5cd6") != std::string::npos,
            "arma7's drop line named only a slot — unattributable to a request");
    }

    // 3d. ck-0zfc — FIRST FAILURE WINS across a broadcast's slots, tested by
    //     CALLING the shipped merge rather than grepping for its condition.
    //     The grep version stayed green under a `true ||` mutant; this does not.
    {
        bridge::SendOutcome acc;                      // starts kOk
        bridge::SendOutcome dropped;
        dropped.stage = bridge::SendStage::kBackpressure;
        dropped.pending_bytes = 33792;
        bridge::SendOutcome ok;                       // a later slot succeeds

        bridge::merge_broadcast_outcome(&acc, dropped);
        bridge::merge_broadcast_outcome(&acc, ok);    // must NOT erase it
        row("broadcast_first_failure_survives_a_later_ok",
            acc.stage == bridge::SendStage::kBackpressure &&
            acc.pending_bytes == 33792,
            "slot 1 dropped, slot 2 succeeded -> the drop is still reported");

        bridge::SendOutcome acc2;
        bridge::SendOutcome first, second;
        first.stage  = bridge::SendStage::kSlotClosed;
        second.stage = bridge::SendStage::kBackpressure;
        bridge::merge_broadcast_outcome(&acc2, first);
        bridge::merge_broadcast_outcome(&acc2, second);
        row("broadcast_keeps_the_FIRST_of_two_failures",
            acc2.stage == bridge::SendStage::kSlotClosed,
            "two drops -> the earlier one is kept, not the last");

        bridge::merge_broadcast_outcome(nullptr, dropped);   // must not crash
        row("broadcast_merge_tolerates_no_subscriber", true,
            "callers that did not opt in pass nullptr");
    }

    // 3e. ck-0zfc — a FAILED KEEPALIVE must be visible. A ping completes an
    //     orphaned frame (see reply_framing_probe.py), so whether pings landed
    //     decides whether a client's silence means anything at all.
    {
        const uint64_t sent0 = bridge::g_ping_sent.load();
        const uint64_t fail0 = bridge::g_ping_failed.load();
        bridge::SendOutcome ok_out;
        std::string quiet = capture([]{
            bridge::SendOutcome o;
            bridge::record_ping(2, true, o);
        });
        row("successful_ping_is_silent", quiet.empty(),
            "one line per ping per slot every 5s would be pure volume");
        std::string loud = capture([]{
            bridge::SendOutcome o;
            o.stage = bridge::SendStage::kBackpressure;
            bridge::record_ping(2, false, o);
        });
        row("failed_ping_records_without_trace",
            loud.find("KEEPALIVE NOT DELIVERED") != std::string::npos &&
            loud.find("stage=backpressure") != std::string::npos,
            "an unwritable slot is named, with the exit");
        row("ping_counters_track_both",
            bridge::g_ping_sent.load() == sent0 + 2 &&
            bridge::g_ping_failed.load() == fail0 + 1,
            "sent counts both, failed counts one");
    }

    // 3f. ck-55dx — the record must say WHEN and HOW LONG, on the same
    //     timeline as the stamps already in every archived bundle.
    {
        bridge::SendOutcome o; o.stage = bridge::SendStage::kSlotClosed;
        std::string t = capture([&]{
            bridge::record_reply("get_tick_info", "6a8f5cd6", false, 128, 3, 7,
                                 nullptr, 1234);
        });
        row("failure_line_carries_a_monotonic_stamp",
            t.size() > 1 && t[0] == '[' && t.find("] [CK3-Bridge]") != std::string::npos,
            "prefixed in blog()'s exact shape, so it joins the existing timeline");
        row("failure_line_carries_the_elapsed",
            t.find("elapsed=1234ms") != std::string::npos,
            "how long the request took, not just that it failed");

        // An UNKNOWN elapsed must not read as an instant reply.
        std::string u = capture([&]{
            bridge::record_reply("get_tick_info", "x", false, 128, 3, 7, nullptr);
        });
        row("unknown_elapsed_is_not_printed_as_zero",
            u.find("elapsed=UNKNOWN") != std::string::npos &&
            u.find("elapsed=0ms") == std::string::npos,
            "absence has its own spelling (ck-duv5)");
    }

    // 4. THE VOLUME CONTRACT. Success must stay behind the gate — otherwise
    //    this lands a line on every action and the gate's original reason
    //    (high-frequency per-query diagnostics) reasserts itself as a
    //    production problem. A fix that logs EVERYTHING is not the fix.
    s = capture([]{
        bridge::record_reply("get_tick_info", "aaa", true, 128, 3, 7);
    });
    row("success_stays_behind_the_gate", s.empty(),
        "g_trace=false, sent_ok=true -> nothing emitted");

    bridge::g_trace = true;
    s = capture([]{ bridge::record_reply("get_tick_info", "bbb", true, 128, 3, 7); });
    row("success_records_when_gate_is_on",
        s.find("BT_TRACE send_observation") != std::string::npos,
        "g_trace=true -> the existing trace still works");
    bridge::g_trace = false;

    // 5. Counters must advance so that SILENCE is readable: "no failure line"
    //    means something only beside a non-zero attempt count.
    const uint64_t att0 = bridge::g_reply_attempted.load();
    const uint64_t fail0 = bridge::g_reply_failed.load();
    capture([]{ bridge::record_reply("x", "y", true, 1, 0, 0); });
    row("counters_count_successes_too",
        bridge::g_reply_attempted.load() == att0 + 1 &&
        bridge::g_reply_failed.load() == fail0,
        "a silent success still advances `attempted`");

    char rc[160];
    bridge::format_reply_counters(rc, sizeof rc);
    row("heartbeat_renders_the_counters",
        strstr(rc, "replies_attempted=") && strstr(rc, "reply_failures=") &&
        strstr(rc, "reply_empty=") && strstr(rc, "backpressure_drops=") && strstr(rc, "ping_failed="),
        rc);

    printf("\n%s (%d failing row%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
