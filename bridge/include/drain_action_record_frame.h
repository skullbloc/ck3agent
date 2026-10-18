#pragma once
//: ck-td8m: A DRAIN-SETTLED CALL'S RECORD MUST REACH THE OBSERVER.
//:
//: THE DEFECT, IN THE ORDER IT WAS BUILT. Three correct changes, and the third
//: one landed on a premise the second had already removed:
//:
//:   ck-6kvq.9   the pipeline records every action as `bridge_record`, minted
//:               by the bridge, and the driver's ArmActionRelay keys its rows
//:               off exactly that. For an INLINE-serviced call the record IS
//:               the reply, emitted by emit_keyed_result via send_observation
//:               from the conductor thread -> BROADCAST -> the observer sees it.
//:
//:   ck-n7n0     2026-09-21 16:39. A drained reply is sent to the client that
//:               ASKED for it: `send_to_session_id(...) || send_observation(...)`
//:               at the drain site in bridge_init.cpp. Correct, and narrowing
//:               delivery is exactly what it says it does. The `||` short-
//:               circuits, so when the asker is known and connected the
//:               broadcast leg NEVER RUNS.
//:
//:   ck-0lph     2026-09-22 10:07, seventeen hours later. Put the attribution
//:               spine on the drain's own reply, "the only reply such a call
//:               ever gets", because a bundle had "140 get_tick_info and 7
//:               set_ai_personality rows and ZERO set_council_task rows while
//:               the council task was being applied correctly every time".
//:               It fixed the CONTENT of a frame the observer had stopped
//:               receiving the day before, so the record reached the AGENT and
//:               the recorder still had nothing to key off.
//:
//: ck-9vgu then measured the consequence in a d39 episode: `construct_building`
//: in bridge.log 168 times, `execute=1` 167 times, and `action=construct_building`
//: rows in actions.jsonl ZERO. Not truncation — a whole action class absent end
//: to end, and every game-thread verb is in that class
//: (`is_game_thread_action`: change_realm_law, set_lifestyle_focus,
//: set_council_task, add_construction, construct_building, move_capital,
//: appoint_councillor, maa_recruit, set_character_faith, ...).
//:
//: THE FIX IS ck-h4pn'S RULE, APPLIED ONE PATH OVER: **a record is not a reply,
//: whoever emits it.** The unicast reply stays exactly as it is — that is what
//: the caller blocks on — and the RECORD is broadcast separately, keyed
//: `caller_request_id` and never `request_id`, because request_id is what makes
//: a frame a reply and both shipped readers pop the caller's pending future on
//: the first frame bearing it, before any type dispatch.
//:
//: Deliberately free of the socket layer and of rapidjson, so
//: bridge/test/drain_record_probe.cpp can obtain these exact bytes on a host
//: with no game, no bridge and no CK3, and scripts/acceptance_td8m_drain_record.py
//: can feed the PRODUCER's bytes to the relay that has to read them. A fixture
//: written to the consumer's shape cannot falsify the producer's shape — that is
//: not hypothetical in this repository, it is ck-h4pn's own finding.
//:
//: The two inputs this cannot reach from a header are PARAMETERS: the envelope
//: clock fragment (built from live tick state) and the attribution fragment
//: (`pipeline_attribution_fragment`, which reads the ticket the drain has just
//: stamped). Passing them in rather than reaching for them is what makes the
//: function callable off-process.
#include <cstddef>
#include <string>

#include "pipeline_frames.h"

namespace bridge {

//: The record for a drain-settled call, or an EMPTY STRING when there is
//: nothing honest to say.
//:
//: `attribution_fragment` is what `pipeline_attribution_fragment` returned: it
//: OPENS WITH A COMMA and already contains the ticket, flush_id and the
//: `bridge_record` object. An EMPTY one means the ticket did not resolve, and
//: this returns "" rather than a spine of empty strings — the relay admits a
//: row on `bridge_record` being present with a non-empty session_id, so a
//: hollow spine would be a row that silently never arrives, and an absence by
//: default is a different defect from an absence with a reason. The drain site
//: has somewhere honest to put that case: it already writes
//: `bridge_record_absent: "ticket_did_not_resolve"` onto the reply.
//:
//: `clock_fragment` carries NO TRAILING COMMA and that is a contract, not a
//: style choice: `frame_append_attribution_fields` opens with one, and
//: `build_envelope_clock_fragment` ends with the game_date value for exactly
//: this reason. ck-h4pn's probe recorded the failure — a comma added here
//: produced `..."game_date":"1066.09.15",,"flush_id"...`, invalid JSON, and a
//: hand-built dict shaped to the consumer could never have shown it.
//:
//: `response` is the HANDLER'S OWN REPLY and every key in it is spliced in by
//: `frame_append_result_body`, the same body every inline reply is built with.
//: So `action`, `success` and `reason` come from the handler rather than being
//: restated here, and a verb whose reply does not name itself produces a record
//: with no action name. THAT IS NOT REPAIRED HERE (ck-9vdk): this bead is about
//: the record REACHING the observer, and inventing a name in the one path that
//: happens to have the parameter to hand would leave the other paths unfixed
//: while making the gap look closed.
inline std::string build_drain_action_record_frame(
        const char* action,
        const char* caller_request_id,
        const std::string& response,
        const std::string& attribution_fragment,
        const std::string& clock_fragment) {
    if (!action || !action[0]) return std::string();
    if (attribution_fragment.empty()) return std::string();

    std::string frame = "{";
    //: `caller_request_id`, NEVER `request_id`. See the header note; ck-h4pn's
    //: acceptance asserts the same rule for the management ledger by
    //: measurement, and this frame is broadcast to EVERY client, so a
    //: `request_id` on it would resolve some other agent's future.
    if (caller_request_id && caller_request_id[0]) {
        frame += "\"caller_request_id\":\"";
        frame += json_escape_min(caller_request_id);
        frame += "\",";
    }
    frame += clock_fragment;
    frame += attribution_fragment;
    //: Which path minted this row, IN the row, so the three are distinguishable
    //: without consulting anything outside the bundle: `record_path:
    //: drain_settled` here, `ledger_source: inline_management` on ck-324r's
    //: management rows, and neither on an `emit_keyed_result` record.
    //:
    //: `record_path` AND NOT `ledger_source`, AND THAT IS NOT A STYLE CHOICE.
    //: ck-ey8p's terminal else in BOTH clients (mcp_server.py's
    //: `_bridge_listener`, agent.py's `_message_loop`) logs a WARNING for any
    //: unmatched frame carrying `ledger_source`: "This is a management record
    //: that reached the client and went nowhere." That sentence is TRUE of a
    //: management row and FALSE of this one — this record's audience IS the
    //: observer, it is broadcast on purpose, and it did arrive where it was
    //: meant to. Reusing the key would make every agent seat log a false
    //: warning once per game-thread verb: ck-9vgu counted 168
    //: construct_building executions in one episode, in six seats, against
    //: ck-324r's 27 management rows for the whole episode. A guard's message
    //: names the case it was written for, not the one that fired it, so this
    //: frame stays out of that population instead of quietly redefining it.
    frame += ",\"record_path\":\"drain_settled\"";
    frame_append_result_body(frame, response, json_escape_min(action), "result");
    return frame;
}

}  // namespace bridge
