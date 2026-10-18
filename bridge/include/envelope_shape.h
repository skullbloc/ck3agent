#pragma once
// ck-dzxr: IS THIS LINE A REQUEST AT ALL?
//
// Every handler in ipc_server's ladder begins by requiring a top-level "actions"
// ARRAY. An envelope without one — a bare {"action":"..."}, an {"action_type":...},
// a typo in the key — failed all of them and fell through to g_msg_queue.
//
// WHY THAT WAS WORSE THAN AN ERROR. The queue's replies are BROADCAST: the game
// thread has no send target, so clients match answers by request_id. A caller
// whose envelope was never understood therefore does not get silence — it gets
// the NEXT broadcast, which is some other request's answer, well-formed and
// complete and about something else entirely.
//
// ck-zgh4 hit this probing a live world: two malformed envelopes each came back
// with a full query_character payload for Harold Godwinson. They read a field
// off it, wrote the OPPOSITE of the truth into a report, and caught it only
// because they recognised the character name. That is the plausible-wrong-answer
// family at the transport layer, and it is the worst place for it: every
// instrument in this repo talks through this socket.
//
// The predicate lives in a header so the acceptance can DRIVE it. Deliberately
// permissive about everything else — inner actions, request_ids and params have
// their own typed refusals (unknown_action, game_paused) further down. This
// closes only the case where the line was never a request.
#include "rapidjson/document.h"

#include <cstddef>

namespace bridge {

// rc2 B9: limits checked on every socket line BEFORE any parse. RapidJSON's
// Accept() (used to re-serialize params) recurses even when Parse() is
// iterative, so depth is capped here, at the one choke point.
constexpr size_t kMaxLineBytes = 1u << 20;   // 1 MiB; longer drops the client
constexpr int    kMaxJsonDepth = 128;

// True when [s, s+n) nests objects/arrays deeper than max_depth. Brackets
// inside strings do not count. Malformed input is left to the parser.
inline bool json_depth_exceeds(const char* s, size_t n, int max_depth) {
    int depth = 0;
    bool in_str = false, esc = false;
    for (size_t i = 0; i < n; ++i) {
        const char c = s[i];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '{' || c == '[') { if (++depth > max_depth) return true; }
        else if (c == '}' || c == ']') { if (depth > 0) --depth; }
    }
    return false;
}

inline bool envelope_has_actions_array(const char* line) {
    if (!line) return false;
    rapidjson::Document d;
    d.Parse<rapidjson::kParseIterativeFlag>(line);
    if (d.HasParseError() || !d.IsObject()) return false;
    return d.HasMember("actions") && d["actions"].IsArray();
}

}  // namespace bridge
