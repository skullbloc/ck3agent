// ck-7ndj — the ledger's ONE code, driven rather than grepped.
//
// The gate case is the shape reject_player_actor actually emits: the CODE under
// "error" and a PROSE SENTENCE under "reason". Before this bead the emitter read
// "reason" only, so the row carried the sentence and map_raw_reason turned it
// into gate=unknown severity=blocked RETRYABLE — a hard, never-retryable refusal
// reported to the agent as retryable.
#include "refusal_code.h"
#include <cstdio>
#include <string>

using bridge::normalise_refusal_code;

static int failures = 0;
static void row(const char* n, bool ok, const std::string& d) {
    printf("%-54s %s  %s\n", n, ok ? "PASS" : "FAIL", d.c_str());
    if (!ok) failures++;
}

//: The real reply, verbatim in shape from dispatch_router.cpp:248-255.
static const char* kPlayerGuardReply =
    "{\"success\":false,\"action\":\"set_controlled_character\","
    "\"error\":\"cannot_act_as_player\","
    "\"reason\":\"The agent inhabits an AI character and must not act as the "
    "human player. The player is a valid TARGET, not a valid ACTOR.\","
    "\"player_character_id\":31808}";

int main() {
    {   // ── THE GATE. Fails before this bead; passes only with the emitter right.
        const auto r = normalise_refusal_code(kPlayerGuardReply);
        row("the_player_guard_reply_yields_the_CODE", r.code == "cannot_act_as_player",
            r.code);
        row("and_NOT_the_prose_sentence",
            r.code.find("inhabits") == std::string::npos &&
            r.code.find(' ') == std::string::npos,
            "no spaces: a code, not a sentence");
        row("and_it_is_recognised", r.known, "one of the nine");
    }
    {   // A registry refusal has no error key at all — the fallback must work.
        const auto r = normalise_refusal_code(
            "{\"success\":false,\"action\":\"set_controlled_character\","
            "\"reason\":\"watchlist_full\",\"character_id\":68445}");
        row("a_reason_only_reply_still_yields_its_code",
            r.code == "watchlist_full" && r.known, r.code);
    }
    {   // PREFERENCE IS BY KEY, NOT BY POSITION. With "reason" written FIRST, a
        // first-match reader would take the sentence. This is the row that says
        // the fix is not accidentally "whichever key comes first".
        const auto r = normalise_refusal_code(
            "{\"success\":false,\"reason\":\"a prose sentence here\","
            "\"error\":\"owned_by_other\"}");
        row("error_wins_even_when_reason_is_written_FIRST",
            r.code == "owned_by_other", r.code);
    }
    {   // Every DECLARED code must be recognised, enumerated from the header
        // rather than retyped — a list retyped beside its source is a second
        // list to drift.
        //
        // SO THIS ROW CANNOT FAIL FOR "THERE ARE NOT NINE", and it used to be
        // named as though it could. Deleting a code from kRefusalCodes leaves it
        // GREEN: the loop bounds come from kRefusalCodeCount, so it tests the
        // eight that remain and all eight pass. Graded as M5 and confirmed.
        // The CARDINALITY is pinned independently, by the literal 9 in
        // acceptance_7ndj's the_header_declares_NINE_codes, which is the row
        // that caught M5 — and that is why neither row is redundant.
        bool all = true; std::string missed;
        for (size_t i = 0; i < bridge::kRefusalCodeCount; ++i) {
            std::string reply = std::string("{\"success\":false,\"error\":\"") +
                                bridge::kRefusalCodes[i] + "\"}";
            const auto r = normalise_refusal_code(reply);
            if (!(r.known && r.code == bridge::kRefusalCodes[i])) {
                all = false; missed += bridge::kRefusalCodes[i]; missed += " ";
            }
        }
        row("all_DECLARED_codes_are_recognised", all,
            missed.empty() ? std::to_string(bridge::kRefusalCodeCount) + " codes"
                           : "missed: " + missed);
    }
    {   // THE RUNTIME RED. A value outside the nine is carried VERBATIM and
        // marked — never normalised into the code field as though it were one.
        // A static enumerator over string literals cannot produce this row: the
        // twelve runtime error-writers have no literal to scan.
        const auto r = normalise_refusal_code(
            "{\"success\":false,\"error\":\"address range not mapped\"}");
        row("an_UNKNOWN_value_is_marked_not_laundered",
            !r.known && r.code == "address range not mapped",
            "carried verbatim, known=false");
    }
    {   // ABSENCE IS NOT AN UNKNOWN CODE. A success carries no refusal at all,
        // and marking that as unknown would make every success look suspect.
        const auto r = normalise_refusal_code("{\"success\":true,\"action\":\"x\"}");
        row("an_ABSENT_reason_is_not_an_unknown_code",
            r.code.empty() && r.known, "absence is its own state");
    }
    // ── THE DISAGREEMENT CASE (bt-bbd's ruling on the ck-7ndj re-cut) ──────
    // A canonical mapping may CHOOSE which key to key the classifier on. A
    // LEDGER may not discard the half it did not choose: two keys that disagree
    // mean the handler was ambiguous, and a row carrying only one of them says
    // the opposite. So the state is three-valued at the source — agree, disagree,
    // only-one-present — and each value has its own row here, because a flag
    // that is only ever exercised in its true case is a flag nobody has seen be
    // false.
    {   // BOTH PRESENT AND EQUAL. Not a disagreement, and this is the leg that
        // must keep passing for the next row to mean anything: if `disagree`
        // were simply "both keys present", it would be true here too and the
        // flag would be measuring presence rather than conflict.
        const auto r = normalise_refusal_code(
            "{\"success\":false,\"error\":\"owned_by_other\","
            "\"reason\":\"owned_by_other\"}");
        row("both_keys_AGREEING_is_not_a_disagreement",
            !r.disagree && r.code == "owned_by_other" && r.known,
            "two keys saying the same thing is one answer, not a conflict");
    }
    {   // ONLY ONE PRESENT. Also not a disagreement — the other leg that has to
        // pass, or `disagree` would be firing on absence.
        const auto only_err = normalise_refusal_code(
            "{\"success\":false,\"error\":\"watchlist_full\"}");
        const auto only_rsn = normalise_refusal_code(
            "{\"success\":false,\"reason\":\"watchlist_full\"}");
        row("a_LONE_key_is_never_a_disagreement",
            !only_err.disagree && !only_rsn.disagree
            && only_err.code == "watchlist_full"
            && only_rsn.code == "watchlist_full",
            "absence of the other key is not conflict with it");
    }
    {   // THE RULED CASE. error carries a code, reason carries a sentence, they
        // are not the same claim — so BOTH are recorded and the row is flagged.
        // The canonical field still resolves, so a classifier keeps working;
        // what it must not do is look unambiguous.
        const auto r = normalise_refusal_code(
            "{\"success\":false,\"error\":\"owned_by_other\","
            "\"reason\":\"that character is claimed by session s-2\"}");
        row("both_keys_DISAGREEING_records_BOTH_and_flags",
            r.disagree
            && r.error_field == "owned_by_other"
            && r.reason_field == "that character is claimed by session s-2"
            && r.code == "owned_by_other" && r.known,
            "canonical code resolves AND neither raw field is discarded");
    }
    {   // THE EXTRACTION IS A SPAN INTO THE REPLY, not a copy the header made.
        // This is what lets the emitter be SEEN extracting from `response`
        // (ck-324r), and it is checkable here: the span must reproduce the field
        // when applied to the original string.
        const std::string reply =
            "{\"success\":false,\"error\":\"character_dead\"}";
        const auto r = normalise_refusal_code(reply);
        row("the_raw_field_is_a_SPAN_into_the_reply",
            r.has_raw && reply.substr(r.raw_off, r.raw_len) == "character_dead",
            "the caller extracts from the handler's own JSON, never from a copy");
    }

    printf("\n%s (%d failing row%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
