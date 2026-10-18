// ck-324r — a management verb's verdict must reach the ledger.
//
// set_controlled_character / release_character are is_bridge_internal_query, so
// classify_polarity returns ANY; an all-ANY envelope is DECLINED by
// pipeline_handle_envelope and served on the legacy ladder; never ticketed, so
// emit_keyed_result never runs and no bridge_record is ever minted. The
// consequence is the event we most want witnessed: diag25 refused 2578 claims
// and actions.jsonl records none of them.
#include "pipeline_frames.h"
#include "management_ledger_frame.h"
#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;
static void row(const char* n, bool ok, const std::string& d) {
    printf("%-56s %s  %s\n", n, ok ? "PASS" : "FAIL", d.c_str());
    if (!ok) failures++;
}
static std::string E(const std::string& v) { return bridge::json_escape_min(v); }

int main() {
    // ── the field core omits a ticket it does not have ──
    std::string f;
    bridge::frame_append_attribution_fields(f, nullptr, "sess_x", 35395,
                                            0, 7300, "", "", E);
    row("no_ticket_key_when_there_is_no_ticket",
        f.find("\"ticket\"") == std::string::npos,
        "tk_00000000 would be a plausible id for a call that was never ticketed");
    row("the_spine_is_still_a_bridge_record",
        f.find("\"bridge_record\":{\"session_id\":\"sess_x\"") != std::string::npos,
        "the relay keys on this and nothing else");

    // ── a ticketed frame is UNCHANGED by the refactor ──
    bridge::Ticket t;
    t.id = 42; t.session_id = "sess_y"; t.character_id = 1; t.flush_id = 2;
    t.tick_executed = 3; t.game_date_executed = "1066.01.01";
    std::string tf;
    bridge::frame_append_attribution(tf, t.id, &t, E);
    row("a_ticketed_frame_still_names_its_ticket",
        tf.find("\"ticket\":\"tk_00000042\"") != std::string::npos, "tk_00000042");

    // ── THE EVENT THIS BEAD EXISTS FOR: a REFUSED claim ──
    //
    // ck-h4pn: this used to hand-assemble the frame, literal by literal, right
    // beside the producer that assembles it for real. Two copies that agreed with
    // each other and with nothing else — and ck-324r's relay row was hand-shaped to
    // records_in in the same way, which is how both stayed green while ep-02's
    // sealed bundle held zero ledger rows. Now the ONE body builds it and the rows
    // below assert the producer's own bytes. CLK is declared before the cases that
    // reuse it; see the contract note there for why it carries no trailing comma.
    // NO TRAILING COMMA, and that is a CONTRACT, not a style choice:
    // frame_append_attribution_fields opens with one. build_envelope_clock_fragment
    // ends with the game_date value for exactly this reason. My first version of
    // this fixture added a comma and the producer bytes came out as
    // ..."game_date":"1066.09.15",,"flush_id"... — invalid JSON. Worth recording: a
    // hand-built dict shaped to records_in could never have shown that, because it
    // was never these bytes. The pair of rows below keeps the contract testable.
    const std::string CLK = "\"tick\":7300,\"game_date\":\"1066.09.15\"";
    const std::string r = bridge::build_management_ledger_frame(
        "set_controlled_character", "abc",
        "{\"success\":false,\"action\":\"set_controlled_character\","
        "\"reason\":\"watchlist_full\",\"character_id\":68445}",
        "sess_mercia", 68445, 7300, CLK);
    row("a_refused_claim_carries_its_reason",
        r.find("\"success\":false") != std::string::npos &&
        r.find("\"reason\":\"watchlist_full\"") != std::string::npos, "watchlist_full");
    row("a_refused_claim_is_named_as_an_action",
        r.find("\"action\":\"session_control\"") != std::string::npos,
        "the relay merges `action` onto the row");
    row("the_frame_is_wellformed",
        !r.empty() && r.back() == '}' && r.find(",,") == std::string::npos,
        r.substr(0, 60) + "...");
    // The rename, asserted in the BYTES rather than in the source that writes them.
    row("the_refusal_frame_carries_no_request_id",
        r.find("\"request_id\"") == std::string::npos &&
        r.find("\"caller_request_id\":\"abc\"") != std::string::npos,
        "the caller's id under its own key, so no reader pops a pending future on it");

    // ── an unattributable row is NAMED, not silently dropped ──
    std::string n;
    bridge::frame_append_attribution_fields(n, nullptr, "", 0, 0, 0, "",
                                            "no_session_on_slot", E);
    row("an_unattributable_row_says_why",
        n.find("\"attribution_reason\":\"no_session_on_slot\"") != std::string::npos,
        "the relay will drop it for an empty session_id; the log says why");

    // ── the escaper actually escapes ──
    row("the_escaper_escapes_a_quote",
        bridge::json_escape_min("a\"b") == "a\\\"b",
        bridge::json_escape_min("a\"b"));
    row("the_escaper_escapes_a_backslash_and_newline",
        bridge::json_escape_min("a\\b\nc") == "a\\\\b\\nc",
        bridge::json_escape_min("a\\b\nc"));
    row("the_escaper_leaves_ordinary_text_alone",
        bridge::json_escape_min("watchlist_full") == "watchlist_full", "watchlist_full");

    printf("\n%s (%d failing row%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");

    // ══ ck-h4pn: PRODUCER BYTES, for the relay to be fed from the producer ══
    //
    // Every row below calls the SAME body the bridge calls — the frame is not
    // re-assembled here. The bytes are printed on PRODUCER_BYTES lines so
    // acceptance_324r / acceptance_h4pn can hand them to ArmActionRelay.records_in
    // instead of a dict written by hand to the relay's own shape. That hand-built
    // fixture is how a green relay row coexisted with zero ledger rows in ep-02:
    // both halves were written to each other and neither to the emitter.
    //
    // The clock fragment is passed in because the real one is built from live tick
    // state. It is the ONE part of these bytes that is not the producer's; it is a
    // valid fragment of the same shape, and the acceptance keys on nothing inside it.

    struct Case { const char* name; const char* verb; const char* rid;
                  const char* response; const char* sid; uint32_t cid; };
    const Case cases[] = {
        // a claim that SUCCEEDED
        {"success", "set_controlled_character", "rq-001",
         "{\"success\":true,\"action\":\"set_controlled_character\","
         "\"controlled_primary\":68445,\"roster_size\":6}", "sess_mercia", 68445},
        // THE DRIVEN REFUSAL — the event the ledger exists to witness
        {"refused_owned_by_other", "set_controlled_character", "rq-002",
         "{\"success\":false,\"action\":\"set_controlled_character\","
         "\"reason\":\"owned_by_other\",\"character_id\":68445}", "sess_mercia", 68445},
        // a release
        {"release", "release_character", "rq-003",
         "{\"success\":true,\"action\":\"release_character\","
         "\"controlled_primary\":0}", "sess_mercia", 68445},
        // UNATTRIBUTABLE: the relay must DROP this, and the frame must say why
        {"no_session", "set_controlled_character", "rq-004",
         "{\"success\":false,\"reason\":\"owned_by_other\"}", "", 0},
        // a session with no claim yet
        {"claimless", "release_character", "rq-005",
         "{\"success\":true}", "sess_mercia", 0},
        // no caller id at all: the key must be ABSENT, not empty
        {"no_request_id", "set_controlled_character", "",
         "{\"success\":true,\"controlled_primary\":68445}", "sess_mercia", 68445},
    };
    for (const Case& c : cases) {
        const std::string f = bridge::build_management_ledger_frame(
            c.verb, c.rid, c.response, c.sid, c.cid, 7300, CLK);
        row(("producer_frame_is_built__" + std::string(c.name)).c_str(),
            !f.empty(), "a management verb always yields a frame");
        printf("PRODUCER_BYTES %s %s\n", c.name, f.c_str());
    }

    // A verb that is NOT ledgered yields NOTHING — an empty return is the decision,
    // so a caller cannot emit a frame that says nothing about a verb it does not record.
    row("a_nonledgered_verb_yields_NO_frame",
        bridge::build_management_ledger_frame(
            "query_character", "rq-006", "{\"success\":true}", "sess_mercia", 68445, 7300, CLK).empty(),
        "query_character is dispatched on the same ladder and must not mint a ledger row");
    row("and_the_verb_filter_is_ONE_predicate",
        bridge::is_management_ledger_verb("set_controlled_character")
        && bridge::is_management_ledger_verb("release_character")
        && !bridge::is_management_ledger_verb("query_character")
        && !bridge::is_management_ledger_verb(nullptr),
        "so a caller asking whether a verb is ledgered cannot answer with a third copy of the list");


    // ── the contract on the clock fragment, both directions ──
    {
        const std::string good = bridge::build_management_ledger_frame(
            "set_controlled_character", "rq-007", "{\"success\":true}",
            "sess_mercia", 68445, 7300, CLK);
        row("a_correctly_shaped_fragment_yields_no_double_comma",
            good.find(",,") == std::string::npos,
            "attribution opens with a comma, so the fragment must not close with one");
        // THE NEGATIVE HALF: prove this row can fail. Without it, the row above is
        // green for a fragment shape nobody ever passes.
        const std::string bad = bridge::build_management_ledger_frame(
            "set_controlled_character", "rq-008", "{\"success\":true}",
            "sess_mercia", 68445, 7300, CLK + ",");
        row("and_a_comma_terminated_fragment_DOES_break_it",
            bad.find(",,") != std::string::npos,
            "the check discriminates: a violated contract produces invalid JSON here");
    }

    return failures ? 1 : 0;
}
