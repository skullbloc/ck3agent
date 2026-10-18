// ck-jg2b — a claimless session's action must not be stamped with someone
// else's character.
#include "pipeline_frames.h"
#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;
static void row(const char* n, bool ok, const std::string& d) {
    printf("%-56s %s  %s\n", n, ok ? "PASS" : "FAIL", d.c_str());
    if (!ok) failures++;
}
static std::string esc(const std::string& v) { return v; }

int main() {
    bridge::Ticket attributed;
    attributed.id = 7; attributed.session_id = "sess_a";
    attributed.character_id = 35395; attributed.flush_id = 3;
    attributed.tick_executed = 900; attributed.game_date_executed = "1084.03.01";

    bridge::Ticket claimless = attributed;
    claimless.character_id = 0;
    claimless.attribution_reason = "claimless_session";

    std::string a, c;
    bridge::frame_append_attribution(a, attributed.id, &attributed, esc);
    bridge::frame_append_attribution(c, claimless.id, &claimless, esc);

    // An ATTRIBUTED record must be byte-identical to what it was before this
    // bead: no reader should have to learn a field it will never see.
    row("an_attributed_record_gains_no_new_field",
        a.find("attribution_reason") == std::string::npos, a.substr(0, 78) + "...");
    row("an_attributed_record_still_names_its_character",
        a.find("\"character_id\":35395") != std::string::npos, "35395");

    // THE DEFECT. The old code stamped g_controlled_primary here — a real id,
    // a real character, in the field a reader trusts to say who acted.
    row("a_claimless_record_does_NOT_borrow_a_character",
        c.find("\"character_id\":0") != std::string::npos,
        "0, not somebody else's id");
    row("a_claimless_record_SAYS_WHY",
        c.find("\"attribution_reason\":\"claimless_session\"") != std::string::npos,
        "a zero with a reason is a fact; a borrowed id is a fabrication");

    // The two legs are distinguishable: "holds nothing" and "is gone" are
    // different defects with different fixes.
    bridge::Ticket gone = claimless;
    gone.attribution_reason = "session_gone";
    std::string g;
    bridge::frame_append_attribution(g, gone.id, &gone, esc);
    row("the_two_zero_legs_are_named_differently",
        g.find("\"session_gone\"") != std::string::npos && g != c,
        "session_gone vs claimless_session");

    // Still valid JSON with the extra key, and it composes with the body.
    std::string frame = "{\"request_id\":\"r\",\"tick\":1";
    bridge::frame_append_attribution(frame, claimless.id, &claimless, esc);
    bridge::frame_append_result_body(frame, "{\"success\":true}", "act", "result");
    row("a_claimless_frame_is_wellformed",
        frame.back() == '}' && frame.find(",,") == std::string::npos &&
        frame.find("\"type\":\"result\"") != std::string::npos, frame);

    // An UNRESOLVED ticket is still the historical empty spine — this bead did
    // not change that leg, and conflating it with claimless would be a new
    // wrong answer rather than a fix.
    std::string blank;
    bridge::frame_append_attribution(blank, 0, nullptr, esc);
    row("an_unresolved_ticket_is_not_relabelled_claimless",
        blank.find("attribution_reason") == std::string::npos,
        "nullptr ticket keeps its own spelling");

    printf("\n%s (%d failing row%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
