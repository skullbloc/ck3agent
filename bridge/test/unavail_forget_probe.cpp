// ck-j6lk, second pass — GRADE THE LEDGER'S REPORT, not the registry.
//
// The first pass destroyed the non-participants and I proved it: the registry forgot
// them, twelve rows said so, and the acceptance passed. Shakedown 3 on deploy-32 then
// sealed INVALID on unavailability_over_x anyway. The bridge log says "UNDECLARED
// CLAIMLESS DETACH — destroying, not pinning" twice, and unavailability_game_days
// STILL reports both helpers — `ended: true`, `activity_count: 0`, one span open for
// 364 silent days, `over_threshold: true` — against an X of 18.
//
// Destroying the session was NECESSARY AND NOT SUFFICIENT. The ledger is a DIFFERENT
// OBSERVER with its own contract: note_session_ended keeps the record and keeps
// accruing, which is right for a destroyed SEAT and wrong for something that was
// never a participant. My rows asked "was it destroyed" of the registry and never
// asked "is it absent" of the report. Every row below reads snapshot() — the thing
// the bundle is built from — because that is the observer that was wrong.
#include "unavailability.h"

#include <cstdio>
#include <string>

static int failures = 0;
static void row(const char* n, bool ok, const std::string& d) {
    printf("%-58s %s  %s\n", n, ok ? "PASS" : "FAIL", d.c_str());
    if (!ok) failures++;
}

// The shakedown-3 shape, in the ledger's own terms: a control session appears, the
// world's clock runs on for a year, and the session never speaks again.
static void run_silent_year(bridge::UnavailabilityLedger& L, const char* id,
                            int64_t first_day) {
    L.note_session(id, first_day);
    L.note_day(first_day + 364);
}

static const bridge::UnavailSession* find(
        const std::vector<bridge::UnavailSession>& v, const std::string& id) {
    for (const auto& s : v) if (s.session_id == id) return &s;
    return nullptr;
}

int main() {
    // ══ THE DISCRIMINATING PAIR, in ONE ledger so neither can pass for the other's
    // reason. Same shape, same silent year, same threshold; the ONLY difference is
    // which exit each takes.
    bridge::UnavailabilityLedger L;
    run_silent_year(L, "helper-never-a-participant", 389348);
    run_silent_year(L, "seat-destroyed-for-good", 389348);

    // Both are present and both accruing BEFORE either exits: without this the rows
    // below could pass because the ledger never recorded them at all, which is a
    // different fact and not the one under test.
    {
        const auto before = L.snapshot();
        const auto* h = find(before, "helper-never-a-participant");
        const auto* s = find(before, "seat-destroyed-for-good");
        row("both_are_RECORDED_and_accruing_before_either_exits",
            h && s && h->silent_game_days > 0 && s->silent_game_days > 0,
            h && s ? "helper " + std::to_string(h->silent_game_days) + "d, seat " +
                     std::to_string(s->silent_game_days) + "d — so an absence below "
                     "is an absence the exit caused"
                   : "the ledger did not record one of them; the pair proves nothing");
    }

    L.forget_session("helper-never-a-participant");   // state (a)
    L.note_session_ended("seat-destroyed-for-good");  // every other exit

    const auto after = L.snapshot();
    const auto* helper = find(after, "helper-never-a-participant");
    const auto* seat = find(after, "seat-destroyed-for-good");

    // ── THE ROW THAT SHAKEDOWN 3 WOULD HAVE FAILED ──
    row("a_non_participant_is_ABSENT_from_the_report",
        helper == nullptr,
        "not present-and-ended: absent. This is the row my first pass did not have, "
        "and the one the bundle refuted");

    // ── ITS DISCRIMINATING PARTNER: the documented accrual is untouched ──
    row("an_ENDED_seat_is_still_PRESENT_in_the_report",
        seat != nullptr,
        seat ? "session_id=" + seat->session_id
             : "the ended seat vanished too — the fix would have eaten the covariate");
    row("and_the_ended_seat_is_STILL_ACCRUING",
        seat && seat->ended && seat->silent_game_days >= 364,
        seat ? "ended=" + std::string(seat->ended ? "true" : "false") +
               " silent_game_days=" + std::to_string(seat->silent_game_days) +
               " — note_session_ended's contract is unchanged, which is the half a "
               "blanket fix would have broken"
             : "absent");
    row("so_the_report_SEPARATES_the_two_exits",
        helper == nullptr && seat != nullptr,
        "same shape, same silent year, same ledger — only the exit differs, and the "
        "report tells them apart");

    // ── FORGETTING IS NOT A FLAG: it must leave nothing behind ──
    row("forgetting_leaves_NO_span_and_NO_threshold_to_read",
        helper == nullptr,
        "a marked record kept an OPEN 364-day span and over_threshold=true; an erased "
        "one has no fields for a reader to reach at all");
    {
        // Re-seeding a forgotten id must start clean, not resurrect the old span —
        // otherwise a reconnecting helper would inherit the days it never owed.
        L.note_session("helper-never-a-participant", 389348 + 400);
        L.note_day(389348 + 401);
        const auto* re = find(L.snapshot(), "helper-never-a-participant");
        row("and_a_RE_SEEDED_id_starts_clean",
            re != nullptr && re->silent_game_days <= 1,
            re ? "silent_game_days=" + std::to_string(re->silent_game_days) +
                 " — the forgotten span did not come back"
               : "the re-seeded session was not recorded at all");
    }

    // ── AND FORGETTING AN ID THE LEDGER NEVER HELD IS A NO-OP, NOT A CRASH ──
    {
        const size_t n = L.snapshot().size();
        L.forget_session("never-existed");
        row("forgetting_an_UNKNOWN_id_changes_nothing",
            L.snapshot().size() == n,
            "the detach path can fire for a session the ledger never registered — an "
            "observer session, or one that predates the episode");
    }

    printf("\n%s (%d failing rows)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
