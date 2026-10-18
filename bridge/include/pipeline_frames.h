// pipeline_frames.h — assembling a pipeline egress frame (ck-6kvq.18).
//
// WHY THIS EXISTS.
//
// A pipeline result frame is the bridge's envelope (request_id, clock, ticket,
// flush_id, bridge_record) with the HANDLER's own JSON object spliced into it,
// braces removed. That splice used to be two lines inline in pipeline.cpp:
//
//     frame += ",";
//     frame += result_json.substr(1);     // handler keys AND its closing brace
//
// which left the frame with no `type` key at all. For a client that also sent
// no request_id — and ~20 shipped clients send none — such a frame carried
// neither a type nor a key: literally unroutable, and impossible to skip-list
// the way tick_start / observation / notification are skip-listed. That is the
// aggravating half of ck-6kvq.18; the primary half is that those clients then
// read the ACCEPTANCE ACK as their answer instead.
//
// So the frame is now TYPED. Getting that right needed one correction that the
// host test caught and that is worth stating plainly, because it is the kind
// of thing "obviously fine" reasoning gets wrong:
//
//   A DUPLICATE `type` KEY IS NOT A TIE-BREAK, IT IS A DISAGREEMENT.
//   rapidjson's FindMember returns the FIRST occurrence of a repeated key;
//   python's json.loads keeps the LAST. So a frame carrying both the handler's
//   own top-level "type" and ours would be read as two different frame types
//   by the bridge's own parser and by every python client. Neither "put ours
//   first" nor "put ours last" fixes that — only emitting exactly one does.
//
// Hence: a handler's top-level "type" is renamed to "handler_type" (its value
// is preserved, not dropped) and the frame's own type is appended last. No
// handler emits a top-level "type" today — the pushed event frames in
// event_handler.cpp are unsolicited observations, not handler replies — so
// this costs one depth-0 scan and changes nothing on the current surface. It
// exists so that the day one does, the frame stays unambiguous instead of
// silently re-opening this bead.
//
// Pulled into a header so the splice can be proved on a host. It rewrites EVERY
// pipeline reply; a malformed frame here breaks the whole mode at once, and
// pipeline.cpp itself is NOT pulled in by this header, which is the point of
// splitting the splice out — but it CAN be linked on a host, and the sentence
// that used to stand here said it could not. ck-1var measured it: 21 ordinary
// host TUs, the generated dispatch-handler stubs, and a small engine boundary
// (bridge/test/clock_release_probe.cpp). That false belief is why ck-8o5j's
// clock release went unwitnessed through two beads — nobody tried.
#pragma once

#include <cstring>
#include <string>

#include "ticket_table.h"

namespace bridge {

// Rename a TOP-LEVEL "type" key in a handler body to "handler_type", so the
// frame's own type is the only one. Depth-0 only: a "type" key nested inside
// the handler's payload is data and is left alone. Honours JSON string escaping
// so a `"type"` appearing inside a string value is not mistaken for a key.
inline void rename_top_level_type_key(std::string& body) {
    static const char kFrom[] = "\"type\"";
    static const char kTo[]   = "\"handler_type\"";
    bool in_str = false, esc = false, expect_key = true;
    int depth = 0;
    for (size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (in_str) {
            if (esc)             esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"')   in_str = false;
            continue;
        }
        if (c == '"') {
            if (depth != 0 || !expect_key) { in_str = true; continue; }
            // A key token at depth 0. Find its closing quote.
            size_t j = i + 1;
            bool e2 = false;
            for (; j < body.size(); ++j) {
                if (e2)                 { e2 = false; continue; }
                if (body[j] == '\\')   { e2 = true;  continue; }
                if (body[j] == '"')     break;
            }
            if (j >= body.size()) return;            // truncated: leave as is
            const size_t len = j - i + 1;
            if (len == sizeof(kFrom) - 1 &&
                body.compare(i, len, kFrom) == 0) {
                body.replace(i, len, kTo);
                i += sizeof(kTo) - 2;                // land on the closing quote
            } else {
                i = j;
            }
            continue;
        }
        if (c == '{' || c == '[') { ++depth; continue; }
        if (c == '}' || c == ']') { --depth; continue; }
        if (depth == 0) {
            if (c == ':')      expect_key = false;
            else if (c == ',') expect_key = true;
        }
    }
}

// ck-9vdk: DOES THE SPLICED BODY ALREADY CARRY THIS KEY AT DEPTH 0?
//
// WHY A SCAN AND NOT A SUBSTRING TEST. `body.find("\"action\":")` is a PROXY:
// it is satisfied by a key inside a nested object, by an `"action"` appearing
// in a string VALUE, and by an escaped quote sequence. All three occur in real
// handler replies — query_title_succession_map nests objects, and a refusal
// reason can quote a verb name. This walks the same way
// `rename_top_level_type_key` walks: string-aware, brace/bracket-depth-aware,
// and only looking at tokens in KEY position.
//
// TWO READERS OF ONE TRAVERSAL, AND THE MIRROR IS CHECKED. This is not folded
// into the rename because that mutator rewrites EVERY pipeline reply and a
// behaviour change in it has the widest blast radius in the frame layer. So
// the traversals are kept separate and `test_pipeline_frames` asserts they
// AGREE about `"type"` over the whole corpus — the pattern this repository
// uses for registered_speed.py against registered_speed.h.
//
// `quoted_key` INCLUDES its quotes, e.g. "\"action\"", so the comparison is
// against the whole token and `"actions"` cannot match `"action"`.
inline bool has_top_level_key(const std::string& body, const char* quoted_key) {
    if (!quoted_key || !*quoted_key) return false;
    const size_t klen = std::strlen(quoted_key);
    bool in_str = false, esc = false, expect_key = true;
    int depth = 0;
    for (size_t i = 0; i < body.size(); ++i) {
        const char c = body[i];
        if (in_str) {
            if (esc)            esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"')  in_str = false;
            continue;
        }
        if (c == '"') {
            if (depth != 0 || !expect_key) { in_str = true; continue; }
            size_t j = i + 1;
            bool e2 = false;
            for (; j < body.size(); ++j) {
                if (e2)              { e2 = false; continue; }
                if (body[j] == '\\') { e2 = true;  continue; }
                if (body[j] == '"')   break;
            }
            // TRUNCATED: an unterminated key token. Answer FALSE rather than
            // guess — the caller then names the action, which is the safe
            // direction: a duplicate key is impossible because there is no
            // closing quote for a key that could have matched.
            if (j >= body.size()) return false;
            if (j - i + 1 == klen && body.compare(i, klen, quoted_key) == 0)
                return true;
            i = j;
            continue;
        }
        if (c == '{' || c == '[') { ++depth; continue; }
        if (c == '}' || c == ']') { --depth; continue; }
        if (depth == 0) {
            if (c == ':')      expect_key = false;
            else if (c == ',') expect_key = true;
        }
    }
    return false;
}

// THE ATTRIBUTION SPINE (§2.8) — ONE DECLARATION, THREE READERS (ck-0lph).
//
// The bridge mints this; the agent never reports it. It was written out inline
// in emit_keyed_result and NOWHERE ELSE, which is the whole of ck-0lph: every
// game-thread verb (set_council_task and the rest of is_game_thread_action)
// replies from the drain in bridge_init.cpp, whose frame injects request_id and
// the clock fragment ONLY. pipeline_note_drain_result deliberately emits no
// second frame ("We do NOT emit a second copy"), so a ticketed, drain-settled
// call reached the driver with no spine at all — a bundle with 140
// get_tick_info rows and ZERO set_council_task rows, the action having
// executed correctly the entire time.
//
// A THIRD SPELLING EXISTS and is left alone deliberately: the `check`
// collector emits the same five values FLAT on the reply — and names one of
// them `game_date` where this names it `game_date_executed`. So a client that
// collects a ticket by `check` still hands the driver something
// episode_driver.py cannot read, since it reads `frame["bridge_record"]`.
// Folding `check` in is a wire-shape change to a reply that clients already
// parse, so it is its own bead, not a rider on this one.
//
// `esc` is the caller's own JSON escaper: there are four in bridge/src and
// consolidating them is not this bead's business, so the SHAPE is shared here
// while each TU keeps its escaper. `t == nullptr` means the ticket did not
// resolve, and reproduces byte for byte what emit_keyed_result has always
// emitted in that case — empty strings and zeroes. Callers for whom that
// would read as attribution nobody checked must say so themselves rather than
// attach this; see the drain in bridge_init.cpp.
// ck-324r: a minimal JSON string escaper, HERE because this header already
// owns frame assembly and because bridge/src has FOUR private json_escape
// copies already (pipeline.cpp, board.cpp, event_handler.cpp,
// budget_ledger.cpp). Adding a fifth to ipc_server.cpp to serve one new caller
// is how there came to be four. Consolidating the existing ones is not this
// bead's business; not adding to them is.
//
// Deliberately escapes rather than trusting the inputs. The values passed are
// a bridge-minted session id, a request id and a typed reason token, none of
// which can contain a quote today — and "cannot today" is exactly the
// reasoning that produces an injection the first time a handler's reason
// string is widened.
inline std::string json_escape_min(const std::string& v) {
    std::string o;
    o.reserve(v.size());
    for (char c : v) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", c);
                    o += b;
                } else {
                    o += c;
                }
        }
    }
    return o;
}

// ck-324r: the FIELD-BASED core. A third reader arrived — the inline
// management path, which has no Ticket because its verbs are never ticketed —
// and the choice was a third spelling of the spine or one core with three
// callers. ck-0lph's rule says core.
template <class Esc>
inline void frame_append_attribution_fields(std::string& frame,
                                            const char* ticket_str,
                                            const std::string& session_id,
                                            uint32_t character_id,
                                            uint64_t flush_id,
                                            uint64_t tick_executed,
                                            const std::string& game_date,
                                            const std::string& reason,
                                            Esc esc) {
    // A null ticket_str OMITS the key rather than printing tk_00000000. An
    // inline management verb is not ticketed at all, and a zero ticket id is a
    // plausible value for a thing that does not exist — the shape this repo
    // keeps having to remove.
    if (ticket_str) {
        frame += ",\"ticket\":\"";
        frame += ticket_str;
        frame += "\"";
    }
    frame += ",\"flush_id\":";
    frame += std::to_string(flush_id);
    frame += ",\"bridge_record\":{\"session_id\":\"";
    frame += esc(session_id);
    frame += "\",\"character_id\":";
    frame += std::to_string(character_id);
    frame += ",\"flush_id\":";
    frame += std::to_string(flush_id);
    frame += ",\"tick_executed\":";
    frame += std::to_string(tick_executed);
    frame += ",\"game_date_executed\":\"";
    frame += esc(game_date);
    frame += "\"";
    if (!reason.empty()) {
        frame += ",\"attribution_reason\":\"";
        frame += esc(reason);
        frame += "\"";
    }
    frame += "}";
}

template <class Esc>
inline void frame_append_attribution(std::string& frame, uint64_t ticket_id,
                                     const Ticket* t, Esc esc) {
    const std::string tk = ticket_id_to_string(ticket_id);
    frame_append_attribution_fields(
        frame, tk.c_str(),
        t ? t->session_id : std::string(),
        t ? t->character_id : 0u,
        t ? t->flush_id : 0,
        t ? t->tick_executed : 0,
        t ? t->game_date_executed : std::string(),
        t ? t->attribution_reason : std::string(),
        esc);
}

// Append a handler response's keys to an OPEN frame (one that already begins
// with '{' and has at least one key), then close the frame with `type`.
//
// `result_json`         the handler's reply, normally a complete JSON object.
// `escaped_action`      already-escaped action name. Emitted whenever the
//                       handler's own reply does not carry a top-level
//                       `action` key, so the frame always names the verb it is
//                       a record of (ck-9vdk). It used to be emitted ONLY when
//                       the reply was unusable, which left every reply that
//                       was a valid object but did not name itself anonymous.
// `type`                the frame type, e.g. "result".
//
// Degenerate inputs are survivable by construction: an empty reply, "{}", a
// non-object, and a truncated object with no closing brace all produce VALID
// JSON. The pre-fix splice produced invalid JSON for the truncated case.
inline void frame_append_result_body(std::string& frame,
                                     const std::string& result_json,
                                     const std::string& escaped_action,
                                     const char* type) {
    if (!result_json.empty() && result_json[0] == '{') {
        // Drop the handler's own braces so its keys sit in OUR object.
        std::string body = result_json.substr(1);
        const size_t close = body.find_last_of('}');
        if (close != std::string::npos) body.erase(close);
        const bool spliceable =
            body.find_first_not_of(" \t\r\n") != std::string::npos;

        // ── ck-9vdk: A ROW THAT CANNOT SAY WHAT WAS EXECUTED ──────────────
        //
        // `escaped_action` used to be used ONLY in the else branch below —
        // "used when the handler gave back nothing usable" — so a handler that
        // answered with a perfectly good JSON object that simply did not name
        // itself produced a frame with no `action` key at all. The driver's
        // relay copies `action` off the frame, `split_bridge_rows` calls a row
        // with no name "unnameable", and `decision_capture` declines it
        // outright. ck-9vgu measured the result in a d39 episode: 2,302 of
        // 3,009 ticketed rows carried ticket, request_id, character_id,
        // flush_id, game_date_executed and tick_executed — everything except
        // the verb.
        //
        // IT IS FIXED HERE AND NOT IN THE HANDLERS. The naming is uneven by
        // construction: 119 sites in action_handlers.cpp write an `action`
        // key, 45 in query_handlers.cpp, and the two highest-volume reads
        // (query_character, query_realm) write none. Teaching 160-odd reply
        // builders a convention is a convention that drifts; the frame layer
        // every reply passes through cannot.
        //
        // OURS GOES FIRST AND ONLY WHEN THE BODY HAS NONE, so:
        //   * a reply that names itself is BYTE-IDENTICAL to before, and the
        //     handler's own value stands — no verdict moves;
        //   * no frame ever carries a duplicate `action`, which matters because
        //     rapidjson's FindMember returns the FIRST occurrence while python
        //     json takes the LAST. That asymmetry is what made the `type` key
        //     a rename rather than a shadow (see above), and it would have made
        //     a duplicated `action` read as two different verbs by the bridge's
        //     own parser and by every python client.
        if (!spliceable || !has_top_level_key(body, "\"action\"")) {
            frame += ",\"action\":\"";
            frame += escaped_action;
            frame += "\"";
        }

        if (spliceable) {
#if !defined(CK3_PIPELINE_FRAME_UNTYPED)
            rename_top_level_type_key(body);
#endif
            frame += ",";
            frame += body;
        }
    } else {
        frame += ",\"action\":\"";
        frame += escaped_action;
        frame += "\"";
    }
#if defined(CK3_PIPELINE_FRAME_UNTYPED)
    // NEGATIVE CONTROL ONLY — the pre-fix frame, with no type at all.
    (void)type;
    frame += "}";
#else
    frame += ",\"type\":\"";
    frame += type;
    frame += "\"}";
#endif
}

}  // namespace bridge
