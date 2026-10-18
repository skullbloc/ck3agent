// ck-td8m — a drain-settled call's record must reach the OBSERVER, not only
// the caller.
//
// ck-n7n0 (2026-09-21 16:39) made the drained reply unicast to the asking
// session; ck-0lph (2026-09-22 10:07) then put the attribution spine on that
// reply, calling it "the only reply such a call ever gets". The second change
// was built on a premise the first had removed seventeen hours earlier, so the
// record reached the AGENT and the recorder never saw it. ck-9vgu priced it:
// construct_building in bridge.log 168 times, execute=1 167 times, and rows in
// actions.jsonl ZERO.
//
// THIS PROBE PRINTS THE PRODUCER'S OWN BYTES. It calls the same
// build_drain_action_record_frame body the bridge calls, on a host, with no
// game and no socket, and scripts/acceptance_td8m_drain_record.py feeds those
// bytes to the REAL ArmActionRelay.records_in. ck-h4pn's finding is why: a
// fixture written to the consumer's shape cannot falsify the producer's shape,
// and in this repository that is not hypothetical — ck-324r's relay row was
// green while a sealed bundle held zero ledger rows, because both halves agreed
// with each other and neither was ever compared to the emitter.
#include "drain_action_record_frame.h"
#include "pipeline_frames.h"
#include <cstdio>
#include <string>

static int failures = 0;
static void row(const char* n, bool ok, const std::string& d) {
    printf("%-58s %s  %s\n", n, ok ? "PASS" : "FAIL", d.c_str());
    if (!ok) failures++;
}
static std::string E(const std::string& v) { return bridge::json_escape_min(v); }

// NO TRAILING COMMA, and that is the contract build_envelope_clock_fragment
// honours: frame_append_attribution_fields opens with one. ck-h4pn's probe
// recorded what happens otherwise — `"game_date":"1066.09.15",,"flush_id"`,
// invalid JSON that no hand-built dict could ever have exposed.
static const char* CLK = "\"tick\":7300,\"game_date\":\"1066.09.15\"";

static std::string spine(uint64_t id, const char* sess, uint32_t cid,
                         uint64_t flush, uint64_t tick, const char* date,
                         const char* reason) {
    bridge::Ticket t;
    t.id = id;
    t.session_id = sess ? sess : "";
    t.character_id = cid;
    t.flush_id = flush;
    t.tick_executed = tick;
    t.game_date_executed = date ? date : "";
    t.attribution_reason = reason ? reason : "";
    std::string f;
    bridge::frame_append_attribution(f, id, &t, E);
    return f;
}

static void emit(const char* name, const std::string& frame) {
    printf("PRODUCER_BYTES %s %s\n", name, frame.c_str());
}

int main() {
    const std::string S = spine(49, "sess_00000000beab8e62_00000004", 30853,
                                12, 7300, "1066.09.15", nullptr);

    // ── THE CASE THE BEAD EXISTS FOR ──────────────────────────────────────
    const std::string built = bridge::build_drain_action_record_frame(
        "construct_building", "rid-td8m-0001",
        R"({"success":true,"action":"construct_building","started":true})",
        S, CLK);
    emit("construct_building_ok", built);
    row("the_record_is_a_bridge_record_the_relay_can_key_on",
        built.find("\"bridge_record\":{\"session_id\":\"sess_") != std::string::npos,
        "records_in admits on bridge_record + a non-empty session_id");
    row("the_record_NAMES_THE_VERB",
        built.find("\"action\":\"construct_building\"") != std::string::npos,
        "a row with no action name is not an action (ck-9rto)");

    // THE RULE THAT MAKES A BROADCAST RECORD SAFE. Both shipped readers pop the
    // caller's pending future on the FIRST frame bearing request_id, before any
    // type dispatch. This frame goes to EVERY client, so that key on it would
    // resolve some other agent's call.
    row("NO_request_id_key_on_a_broadcast_record",
        built.find("\"request_id\"") == std::string::npos,
        "caller_request_id carries the value instead");
    row("the_callers_id_SURVIVES_as_caller_request_id",
        built.find("\"caller_request_id\":\"rid-td8m-0001\"") != std::string::npos,
        "renamed rather than dropped (ck-h4pn's ruling)");
    row("the_path_is_named_IN_the_row",
        built.find("\"record_path\":\"drain_settled\"") != std::string::npos,
        "distinguishable from inline_management and from emit_keyed_result");
    // `record_path`, NOT `ledger_source`: ck-ey8p's terminal else warns on any
    // unmatched frame carrying ledger_source, calling it "a management record
    // that reached the client and went nowhere". This record's audience IS the
    // observer and it is broadcast on purpose, so borrowing that key would put
    // a false warning in six agent seats 168 times an episode.
    row("it_does_NOT_borrow_ck_324r_s_ledger_source_key",
        built.find("ledger_source") == std::string::npos,
        "ck-ey8p's warning is written about management rows, not about this");
    row("the_frame_is_typed",
        built.find("\"type\":\"result\"") != std::string::npos,
        "an untyped frame cannot be skip-listed (ck-6kvq.18)");

    // ── a REFUSED write is still a record ─────────────────────────────────
    emit("construct_building_refused", bridge::build_drain_action_record_frame(
        "construct_building", "rid-td8m-0002",
        R"({"success":false,"action":"construct_building","reason":"not_owner_or_realm"})",
        S, CLK));

    // ── AN UNRESOLVED TICKET EMITS NOTHING ────────────────────────────────
    // Not a spine of empty strings: the relay drops a row with no session_id,
    // so that would be an absence by default where the reply below already
    // carries an absence with a reason.
    row("an_empty_spine_yields_NO_FRAME",
        bridge::build_drain_action_record_frame(
            "construct_building", "rid", R"({"success":true})", "", CLK).empty(),
        "the drain site writes bridge_record_absent on the reply instead");
    row("an_unnamed_verb_yields_NO_FRAME",
        bridge::build_drain_action_record_frame(
            "", "rid", R"({"success":true})", S, CLK).empty(),
        "and a null one too");
    row("a_null_action_yields_NO_FRAME",
        bridge::build_drain_action_record_frame(
            nullptr, "rid", R"({"success":true})", S, CLK).empty(), "");

    // ── degenerate handler replies must still be VALID JSON ───────────────
    emit("empty_reply", bridge::build_drain_action_record_frame(
        "set_council_task", "rid-td8m-0003", "", S, CLK));
    emit("non_object_reply", bridge::build_drain_action_record_frame(
        "set_council_task", "rid-td8m-0004", "not json at all", S, CLK));
    emit("truncated_reply", bridge::build_drain_action_record_frame(
        "set_council_task", "rid-td8m-0005", R"({"success":true)", S, CLK));
    emit("empty_object_reply", bridge::build_drain_action_record_frame(
        "set_council_task", "rid-td8m-0006", "{}", S, CLK));

    // ── no caller id at all (a request_id-less client) ────────────────────
    const std::string anon = bridge::build_drain_action_record_frame(
        "maa_recruit", nullptr, R"({"success":true,"action":"maa_recruit"})",
        S, CLK);
    emit("no_caller_id", anon);
    row("no_caller_id_omits_the_key_rather_than_writing_empty",
        anon.find("caller_request_id") == std::string::npos,
        "an empty string would read as a caller that sent one");

    // ── a handler that names its own `type` must not shadow the frame's ───
    emit("reply_carrying_its_own_type", bridge::build_drain_action_record_frame(
        "move_capital", "rid-td8m-0007",
        R"({"success":true,"action":"move_capital","type":"handler_type"})",
        S, CLK));

    // ── ck-9vdk's POPULATION, MEASURED AND NOT REPAIRED HERE ──────────────
    // A handler whose reply does not name itself produces a record with no
    // action name. Inventing one in this path only would leave every other
    // path unfixed while making the gap look closed, so the case is EMITTED
    // for the acceptance to measure and the bead to own.
    emit("reply_that_does_not_name_itself",
         bridge::build_drain_action_record_frame(
             "set_lifestyle_focus", "rid-td8m-0008",
             R"({"success":true,"focus":"intrigue"})", S, CLK));

    // ── a claimless session carries its reason, never a borrowed id ───────
    emit("claimless_session", bridge::build_drain_action_record_frame(
        "appoint_councillor", "rid-td8m-0009",
        R"({"success":true,"action":"appoint_councillor"})",
        spine(50, "sess_claimless", 0, 12, 7300, "1066.09.15",
              "claimless_session"),
        CLK));

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
