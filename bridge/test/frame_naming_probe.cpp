// ck-9vdk — PRODUCER BYTES for the question "does this row say what was
// executed?".
//
// `frame_append_result_body` splices the handler's reply into the bridge's
// envelope. It used to emit `escaped_action` ONLY when the reply was unusable,
// so a reply that was a perfectly good JSON object and did not name itself
// produced a frame with no `action` key. ck-9vgu measured it: 2,302 of 3,009
// ticketed rows in a d39 episode carried ticket, request_id, character_id,
// flush_id, tick_executed and game_date_executed — everything but the verb.
//
// The naming is uneven by construction and that is why the fix is at this
// layer: 119 sites in action_handlers.cpp write an `action` key, 45 in
// query_handlers.cpp, and the two highest-volume reads (query_character,
// query_realm) write none.
//
// This prints the emitter's OWN bytes so scripts/acceptance_9vdk_row_names.py
// can feed them to the REAL ArmActionRelay, split_bridge_rows and
// decision_capture. A fixture written to the consumer's shape cannot falsify
// the producer's shape (ck-h4pn).
#include "pipeline_frames.h"
#include <cstdio>
#include <string>

static std::string envelope(const char* rid, const char* action) {
    std::string f = "{";
    if (rid && rid[0]) {
        f += "\"request_id\":\"";
        f += rid;
        f += "\",";
    }
    f += "\"tick\":7300,\"game_date\":\"1066.09.15\"";
    bridge::Ticket t;
    t.id = 91;
    t.session_id = "sess_00000000beab8e62_00000004";
    t.character_id = 30853;
    t.flush_id = 12;
    t.tick_executed = 7300;
    t.game_date_executed = "1066.09.15";
    bridge::frame_append_attribution(f, t.id, &t, [](const std::string& v) {
        return bridge::json_escape_min(v);
    });
    (void)action;
    return f;
}

static void emit(const char* name, const char* action, const char* reply) {
    std::string f = envelope("rid-9vdk", action);
    bridge::frame_append_result_body(f, reply, bridge::json_escape_min(action),
                                     "result");
    printf("PRODUCER_BYTES %s %s\n", name, f.c_str());
}

int main() {
    // THE POPULATION. query_character's real reply shape: valid, useful, and
    // it names nothing.
    emit("anonymous_read", "query_character",
         "{\"success\":true,\"character_id\":30853,\"gold\":123.45}");
    emit("anonymous_read_2", "query_realm",
         "{\"success\":true,\"counties\":7}");

    // A write that DOES name itself — must be unchanged, with its own value.
    emit("named_write", "construct_building",
         "{\"success\":true,\"action\":\"construct_building\",\"started\":true}");

    // A handler name that DISAGREES with the ticket's: today the handler's
    // stands, and this bead does not change that.
    emit("handler_disagrees", "ticket_said_that",
         "{\"success\":true,\"action\":\"handler_said_this\"}");

    // Shapes a substring test would have called "already named".
    emit("nested_only", "query_title_succession_map",
         "{\"success\":true,\"detail\":{\"action\":\"nested\"}}");
    emit("in_a_string_value", "query_opinion",
         "{\"success\":false,\"reason\":\"no action available\"}");
    emit("longer_key", "query_intents", "{\"success\":true,\"actions\":[1,2]}");

    // Degenerate replies, which were already named before this bead — kept so
    // the acceptance can show the fix did not disturb them.
    emit("empty_object", "unpause_game", "{}");
    emit("empty_reply", "pause_game", "");
    emit("non_object", "get_tick_info", "[1,2,3]");
    emit("truncated", "query_character", "{\"success\":true");
    return 0;
}
