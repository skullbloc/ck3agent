// refusal_code.h — the ONE normalised refusal code for a management reply.
//
// ck-7ndj. emit_management_ledger_record read "reason" and nothing else, while
// reject_player_actor (dispatch_router.cpp:238) puts the CODE under "error" and
// a PROSE SENTENCE under "reason". So a cannot_act_as_player refusal produced a
// ledger row carrying the sentence, and the code was discarded AT THE EMITTER —
// which is why telling the classifier to "read both fields" could not work: the
// ledger frame has no error key and never did.
//
// EXTRACTED SO A FIXTURE CAN DRIVE IT. emit_management_ledger_record is static
// inside ipc_server.cpp and cannot be linked by a probe; written inline there,
// this decision is a condition a test can only GREP for, and a grep cannot tell
// a live condition from a defeated one (ck-7rk6's reason for extracting
// handoff_subject, and ck-1v1t's two lost lanes before it).
#pragma once

#include <cstddef>
#include <string>

namespace bridge {

//: THE NINE. Every reason a management verb's claim can be refused with —
//: five from the registry (session.cpp reason_out) and four from the handler's
//: own gates, including ownership_divergence which is a TERNARY at
//: dispatch_router.cpp:826 emitted at :843 and which no literal grep finds.
inline constexpr const char* kRefusalCodes[] = {
    "observe_mode", "character_not_found", "character_dead",
    "cannot_act_as_player", "invalid_character", "no_session",
    "owned_by_other", "watchlist_full", "ownership_divergence",
};
inline constexpr size_t kRefusalCodeCount =
    sizeof(kRefusalCodes) / sizeof(kRefusalCodes[0]);

//: First string value for `key` in a flat JSON object, or empty.
//: Deliberately the same parse for both keys — one computation, two callers,
//: so "error" and "reason" cannot be read by two subtly different readers.
//: WHERE the first string value for `key` lives, or false. Split out from
//: json_first_string so a caller can perform the EXTRACTION ITSELF from the
//: reply (ck-324r's evidence rule: the ledger's reason is pulled out of the
//: handler's own JSON, never inferred or substituted). One parse, two shapes:
//: the span for a caller that must be seen extracting, the string for one that
//: only needs the value.
inline bool json_first_string_span(const std::string& obj, const char* key,
                                   size_t* off, size_t* len) {
    std::string pat = "\"";
    pat += key;
    pat += "\":\"";
    const size_t k = obj.find(pat);
    if (k == std::string::npos) return false;
    const size_t a = k + pat.size();
    const size_t b = obj.find('"', a);
    if (b == std::string::npos) return false;
    if (off) *off = a;
    if (len) *len = b - a;
    return true;
}

inline std::string json_first_string(const std::string& obj, const char* key) {
    std::string pat = "\"";
    pat += key;
    pat += "\":\"";
    const size_t k = obj.find(pat);
    if (k == std::string::npos) return std::string();
    const size_t a = k + pat.size();
    const size_t b = obj.find('"', a);
    if (b == std::string::npos) return std::string();
    return obj.substr(a, b - a);
}

inline bool is_known_refusal_code(const std::string& v) {
    for (size_t i = 0; i < kRefusalCodeCount; ++i) {
        if (v == kRefusalCodes[i]) return true;
    }
    return false;
}

struct RefusalCode {
    std::string code;          //: the CANONICAL code the classifier keys on
    bool        known = true;  //: false -> the row must say so, never launder it

    //: WHERE the reply's own field sits, so the emitter can extract it FROM THE
    //: REPLY rather than be handed a copy. ck-324r asserts the extraction, and
    //: a value arriving pre-extracted cannot be told from an inferred one.
    size_t      raw_off = 0;
    size_t      raw_len = 0;
    bool        has_raw = false;

    //: BOTH keys present and DIFFERENT. Neither is preferred silently: the row
    //: records both raw fields and carries a named flag. Ruled by bt-bbd on the
    //: ck-7ndj re-cut — a canonical mapping may choose, but a LEDGER may not
    //: discard the half it did not choose, because the disagreement is itself
    //: the finding.
    bool        disagree = false;
    std::string error_field;
    std::string reason_field;
};

//: PREFER error, FALL BACK to reason, emit ONE code.
//:
//: The handler is deliberately NOT changed to emit "reason" twice: the emitter's
//: find() takes the FIRST occurrence, so key order inside a ResponseBuilder
//: would silently decide whether a row got a code or a sentence.
//:
//: SCOPE. "Prefer error" is not universally safe here — of 38 writers of an
//: error key in this tree, 24 carry a bare code, 2 carry prose and 12 carry a
//: RUNTIME value of unknown shape. None of the 14 is reachable from
//: session_control or session_release, so the preference is right over the
//: population this emitter sees, and it is right BECAUSE of that scope. Hence
//: `known`: a static enumerator over string literals cannot see the twelve, so
//: the check has to happen at runtime, on the value that actually arrived.
//:
//: An ABSENT reason is not an unknown code — absence is its own state.
inline RefusalCode normalise_refusal_code(const std::string& response) {
    RefusalCode out;
    size_t eo = 0, el = 0, ro = 0, rl = 0;
    const bool has_error  = json_first_string_span(response, "error",  &eo, &el);
    const bool has_reason = json_first_string_span(response, "reason", &ro, &rl);
    out.error_field  = has_error  ? response.substr(eo, el) : std::string();
    out.reason_field = has_reason ? response.substr(ro, rl) : std::string();

    //: NOT "error wins". `error` is where a handler puts a CODE and `reason`
    //: where it puts a sentence, so preferring `error` is a canonicalisation.
    //: But when both are present and disagree, choosing is a claim about which
    //: the handler meant, and this header is not entitled to make it — hence
    //: `disagree`, which obliges the caller to record both.
    out.disagree = has_error && has_reason && out.error_field != out.reason_field;
    if (has_error) {
        out.has_raw = true; out.raw_off = eo; out.raw_len = el;
        out.code = out.error_field;
    } else if (has_reason) {
        out.has_raw = true; out.raw_off = ro; out.raw_len = rl;
        out.code = out.reason_field;
    }
    out.known = out.code.empty() || is_known_refusal_code(out.code);
    return out;
}

}  // namespace bridge
