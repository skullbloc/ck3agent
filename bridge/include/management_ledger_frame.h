#pragma once
//: ck-h4pn / ck-324r: ONE composition, TWO readers.
//:
//: The bytes the management ledger record puts on the wire used to be assembled
//: inline in emit_management_ledger_record, and the only thing that tested them
//: (bridge/test/mgmt_ledger_probe.cpp) re-assembled its OWN copy around one real
//: helper. So the composition — the caller_request_id head, the action/verb/
//: success tail, the refusal annotations — was exercised by nothing that a change
//: to it could break, and acceptance_324r fed the relay's records_in a HAND-BUILT
//: dict shaped to what records_in wanted.
//:
//: A fixture written to the CONSUMER's shape cannot falsify the PRODUCER's shape.
//: That is not hypothetical here: ck-324r's relay row was green while ep-02's
//: sealed bundle contained zero ledger rows, because both halves agreed with each
//: other and neither was ever compared to the emitter.
//:
//: So the composition lives here and has exactly one body. The emitter calls it
//: and sends what it returns; the probe calls it and PRINTS what it returns; the
//: acceptance feeds those printed bytes to records_in. There is no mirror to keep
//: in step, which is the only version of this that cannot drift.
//:
//: Deliberately free of the socket layer and of rapidjson, so a probe can obtain
//: producer bytes on a host with no game, no bridge and no CK3. The two inputs the
//: emitter cannot supply from here — the envelope clock fragment, which is built
//: from live tick state, and the session fields, which are read under the
//: session's own mutex — are PARAMETERS. Passing them in rather than reaching for
//: them is what makes the function callable off-process.
#include <cstdint>
#include <cstring>
#include <string>

#include "pipeline_frames.h"
#include "refusal_code.h"

namespace bridge {

//: Whether this verb is recorded at all. The two strcmps live here so a caller
//: asking "is this verb ledgered?" cannot answer it with a third copy of the
//: list that then falls out of step with the emitter.
inline bool is_management_ledger_verb(const char* verb) {
    if (!verb) return false;
    return strcmp(verb, "set_controlled_character") == 0
        || strcmp(verb, "release_character") == 0;
}

//: The frame, or an EMPTY STRING for a verb that is not ledgered — so the caller
//: emits nothing rather than emitting a frame that says nothing. An empty return
//: is a decision, not a failure: `is_management_ledger_verb` answers the same
//: question without building anything.
//:
//: `response` is the HANDLER'S OWN REPLY and the verdict is read out of it. A
//: refused claim and a successful one are both a completed dispatch, and telling
//: them apart is the whole point of the record; so `success` is never passed in.
inline std::string build_management_ledger_frame(const char* verb,
                                                const char* request_id,
                                                const std::string& response,
                                                const std::string& session_id,
                                                uint32_t character_id,
                                                uint64_t tick,
                                                const std::string& clock_fragment) {
    if (!is_management_ledger_verb(verb)) return std::string();
    const bool is_control = (strcmp(verb, "set_controlled_character") == 0);

    const bool ok = response.find("\"success\":true") != std::string::npos;

    const bridge::RefusalCode rc = bridge::normalise_refusal_code(response);
    std::string reason;
    if (rc.has_raw) reason = response.substr(rc.raw_off, rc.raw_len);

    //: An unattributable row is DROPPED by the relay (records_in requires a
    //: non-empty session_id), so say WHY it is empty rather than emit a row that
    //: silently never arrives. Absence with a named reason and absence by default
    //: are different defects.
    const std::string attr_reason = session_id.empty() ? "no_session_on_slot"
                                  : (character_id == 0 ? "claimless_session" : "");

    const auto esc = [](const std::string& v) { return bridge::json_escape_min(v); };

    std::string frame = "{";
    //: `caller_request_id`, NEVER `request_id` — see the emitter's own note. The
    //: key that makes a frame a REPLY must not appear on a record whose audience
    //: is whoever is listening.
    if (request_id && request_id[0]) {
        frame += "\"caller_request_id\":\"";
        frame += bridge::json_escape_min(request_id);
        frame += "\",";
    }
    frame += clock_fragment;
    bridge::frame_append_attribution_fields(
        frame, nullptr, session_id, character_id,
        0,
        tick,
        std::string(), attr_reason, esc);
    frame += ",\"action\":\"";
    frame += is_control ? "session_control" : "session_release";
    frame += "\",\"verb\":\"";
    frame += verb;
    frame += "\",\"success\":";
    frame += ok ? "true" : "false";
    if (!reason.empty()) {
        frame += ",\"reason\":\"";
        frame += bridge::json_escape_min(reason);
        frame += "\"";
        if (!rc.known) {
            frame += ",\"reason_code_unknown\":true";
        }
        if (rc.disagree) {
            frame += ",\"reason_error_field\":\"";
            frame += bridge::json_escape_min(rc.error_field);
            frame += "\",\"reason_reason_field\":\"";
            frame += bridge::json_escape_min(rc.reason_field);
            frame += "\",\"reason_fields_disagree\":true";
        }
    }
    frame += ",\"ledger_source\":\"inline_management\",\"type\":\"result\"}";
    return frame;
}

}  // namespace bridge
