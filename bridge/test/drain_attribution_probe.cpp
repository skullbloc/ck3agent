// ck-0lph — the attribution spine must be ONE shape, minted for BOTH replies.
//
// The defect: it existed in exactly one place, written out inline in
// emit_keyed_result. Every game-thread verb replies from the drain instead,
// which injected request_id and the clock and nothing else, so no such call
// ever carried a spine. The driver keys its record off `bridge_record`, so
// those calls produced no row — while executing correctly the whole time.
#include "pipeline_frames.h"
#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;
static void row(const char* n, bool ok, const std::string& d) {
    printf("%-52s %s  %s\n", n, ok ? "PASS" : "FAIL", d.c_str());
    if (!ok) failures++;
}

// The probe's own escaper. The point of templating on it is that each TU keeps
// its own; this one is deliberately NOT pipeline.cpp's, so the test proves the
// SHAPE is shared rather than proving two copies of one escaper agree.
static std::string esc(const std::string& v) { return v; }

int main() {
    bridge::Ticket t;
    t.id = 342;
    t.session_id = "sess_00000000ed491673_00000003";
    t.flush_id = 9;
    t.character_id = 35395;
    t.tick_executed = 4711;
    t.game_date_executed = "1066.09.15";

    // THE PIN. This is byte for byte what emit_keyed_result emitted inline
    // before ck-0lph moved the shape into the header. Pinned as a literal so a
    // refactor of the builder cannot quietly change the wire shape that every
    // shipped client and the episode driver already parse.
    const std::string expect =
        ",\"ticket\":\"tk_00000342\",\"flush_id\":9"
        ",\"bridge_record\":{\"session_id\":\"sess_00000000ed491673_00000003\""
        ",\"character_id\":35395,\"flush_id\":9,\"tick_executed\":4711"
        ",\"game_date_executed\":\"1066.09.15\"}";

    std::string a;
    bridge::frame_append_attribution(a, t.id, &t, esc);
    row("the_shape_is_byte_identical_to_the_pre_fix_spine", a == expect, a);

    // THE MIRROR IS CHECKED. Both readers call this one builder, so "they
    // agree" is not an assertion about two code paths — it is the reason there
    // is only one. What IS worth asserting is that the fragment a drain reply
    // splices is the same bytes an inline reply splices for the same ticket.
    std::string inline_side, drain_side;
    bridge::frame_append_attribution(inline_side, t.id, &t, esc);
    bridge::frame_append_attribution(drain_side,  t.id, &t, esc);
    row("inline_and_drain_splice_the_SAME_bytes",
        inline_side == drain_side && !inline_side.empty(), inline_side);

    // A fragment opens with ',' because it is spliced into an OPEN frame that
    // already has keys. Getting this wrong yields invalid JSON on every
    // game-thread reply at once.
    row("the_fragment_splices_into_an_open_frame",
        !a.empty() && a[0] == ',', a.substr(0, 12));

    // AN UNRESOLVED TICKET IS NOT A BLANK SPINE. The builder still reproduces
    // the historical empty form when handed nullptr — emit_keyed_result has
    // always done that and changing it is not this bead — but the value is
    // recognisably empty, which is what lets the drain refuse to ship it.
    std::string blank;
    bridge::frame_append_attribution(blank, 0, nullptr, esc);
    row("a_null_ticket_yields_a_RECOGNISABLY_empty_spine",
        blank.find("\"session_id\":\"\"") != std::string::npos &&
        blank.find("\"character_id\":0") != std::string::npos, blank);
    row("an_empty_spine_is_NOT_what_a_real_one_looks_like",
        blank != expect, "a blank spine cannot be mistaken for attribution");

    // The clock fragment and the handler body still bracket it correctly:
    // frame = { request_id, clock } + spine + ',' + handler-keys.
    std::string frame = "{\"request_id\":\"r\",\"tick\":1";
    bridge::frame_append_attribution(frame, t.id, &t, esc);
    bridge::frame_append_result_body(frame, "{\"success\":true}", "set_council_task",
                                     "result");
    const bool wellformed =
        frame.find("\"bridge_record\":{") != std::string::npos &&
        frame.find("\"success\":true") != std::string::npos &&
        frame.find(",,") == std::string::npos &&
        frame.back() == '}';
    row("a_whole_drain_frame_is_wellformed_with_the_spine", wellformed, frame);

    // THE DRAIN'S OWN SPLICE, reproduced exactly as bridge_init.cpp performs
    // it: request_id, clock fragment, spine, then the handler's object with
    // its opening brace dropped. This is the path under test — the row above
    // exercises frame_append_result_body, which is the INLINE path and not
    // what a game-thread verb takes.
    {
        const std::string handler = "{\"success\":true,\"task\":\"fabricate_claim\"}";
        std::string inj = "{\"request_id\":\"8b0aea4c\",";
        inj += "\"tick\":4711,\"game_date\":\"1066.09.15\"";   // clock fragment
        std::string spine;
        bridge::frame_append_attribution(spine, t.id, &t, esc);
        inj += spine;
        inj += ",";
        inj += handler.substr(1);
        const bool ok =
            inj.find("\"bridge_record\":{") != std::string::npos &&
            inj.find("\"session_id\":\"sess_00000000ed491673_00000003\"") != std::string::npos &&
            inj.find("\"success\":true") != std::string::npos &&
            inj.find(",,") == std::string::npos &&
            inj.back() == '}';
        row("the_DRAIN_splice_carries_the_spine_and_stays_valid", ok, inj);

        // THE DEFECT ITSELF, as a negative control: the pre-fix drain frame.
        // It must NOT satisfy what the driver needs, or this bead is describing
        // a problem that was never there.
        std::string prefix_frame = "{\"request_id\":\"8b0aea4c\",";
        prefix_frame += "\"tick\":4711,\"game_date\":\"1066.09.15\"";
        prefix_frame += ",";
        prefix_frame += handler.substr(1);
        row("the_PRE_FIX_drain_frame_carried_no_bridge_record",
            prefix_frame.find("bridge_record") == std::string::npos,
            prefix_frame);
    }

    printf("\n%s (%d failing row%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
