// envelope_shape_probe.cpp — WHICH LINES ARE REQUESTS? (ck-dzxr)
//
// The bridge used to QUEUE an envelope it could not parse, and the queue's
// replies are broadcast — so the sender read another request's answer as its
// own. The two shapes below marked "ck-zgh4 sent this" are the literal lines
// that came back as a query_character payload for Harold Godwinson on a live
// world. They are in the table because a fixture I invented could not have
// found this; a real probe against a real socket did.
#include "../include/envelope_shape.h"

#include <cstdio>
#include <string>

namespace {
int g_failed = 0;
void row(const char* name, bool ok) {
    if (!ok) ++g_failed;
    std::printf("%s\t%d\n", name, ok ? 1 : 0);
}
using bridge::envelope_has_actions_array;
}  // namespace

int main() {
    std::printf("case\tvalue\n");

    // ── ACCEPTED: the real protocol shape ───────────────────────────────────
    row("a_well_formed_envelope_is_accepted",
        envelope_has_actions_array(
            R"({"actions":[{"action":"get_tick_info","params":{},"request_id":"r1"}]})"));
    row("an_empty_actions_array_is_still_a_request",
        envelope_has_actions_array(R"({"actions":[]})"));
    row("extra_top_level_keys_do_not_disqualify_it",
        envelope_has_actions_array(
            R"({"type":"action","tick_id":0,"actions":[{"action":"x"}]})"));

    // ── REFUSED: the shapes that used to be queued ──────────────────────────
    row("a_bare_action_envelope_is_refused",              // ck-zgh4 sent this
        !envelope_has_actions_array(R"({"action":"get_bridge_state"})"));
    row("an_action_type_envelope_is_refused",             // ck-zgh4 sent this
        !envelope_has_actions_array(R"({"action_type":"get_bridge_state"})"));
    row("actions_as_an_OBJECT_is_refused",
        !envelope_has_actions_array(R"({"actions":{"action":"x"}})"));
    row("actions_as_a_STRING_is_refused",
        !envelope_has_actions_array(R"({"actions":"get_tick_info"})"));
    row("a_misspelled_key_is_refused",
        !envelope_has_actions_array(R"({"action s":[{"action":"x"}]})"));

    // ── REFUSED: not even JSON, or not an object ────────────────────────────
    row("malformed_json_is_refused",   !envelope_has_actions_array(R"({"actions":[)"));
    row("a_json_ARRAY_is_refused",     !envelope_has_actions_array(R"([{"action":"x"}])"));
    row("a_bare_string_is_refused",    !envelope_has_actions_array(R"("get_tick_info")"));
    row("an_empty_line_is_refused",    !envelope_has_actions_array(""));
    row("a_null_pointer_is_refused",   !envelope_has_actions_array(nullptr));

    // THE ROW THAT STOPS THIS BEING A CHECK THAT CANNOT FAIL. If the predicate
    // returned true for everything the accept rows above would all pass, and if
    // it returned false for everything the refuse rows would. It must do BOTH,
    // so assert the two populations actually differ.
    const bool accepts_a_good_one =
        envelope_has_actions_array(R"({"actions":[{"action":"a"}]})");
    const bool refuses_a_bad_one =
        !envelope_has_actions_array(R"({"action":"a"})");
    row("the_predicate_DISCRIMINATES_rather_than_answering_one_way",
        accepts_a_good_one && refuses_a_bad_one);

    std::printf("\n%s: %d row(s) failed\n", g_failed ? "FAIL" : "PASS", g_failed);
    return g_failed ? 1 : 0;
}
