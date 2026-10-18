// ck-7rk6 — does a firing get counted, and does a clean run say zero out loud?
#include "skip_held_heir.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace bridge { FILE* g_logfile = nullptr; }

static int failures = 0;
static void row(const char* n, bool ok, const char* d) {
    printf("%-46s %s  %s\n", n, ok ? "PASS" : "FAIL", d);
    if (!ok) failures++;
}
static std::string cap(void (*fn)()) {
    char* b = nullptr; size_t sz = 0;
    FILE* f = open_memstream(&b, &sz);
    bridge::g_logfile = f; fn(); fflush(f); fclose(f); bridge::g_logfile = nullptr;
    std::string s(b ? b : ""); free(b); return s;
}

int main() {
    char buf[128];
    bridge::format_skip_held_heir(buf, sizeof buf);
    row("clean_run_states_zero_explicitly",
        std::strcmp(buf, "skip_held_heir=0 skip_held_heir_exhausted=0 "
                         "skip_held_heir_exhausted_cap_full=0") == 0, buf);
    // ck-handoff-swap: the cap-full subset is on the SAME line, so a clean run
    // says all three zeroes out loud. A counter published nowhere is not
    // evidence, and a zero that is absent is not a zero.
    row("the_cap_full_subset_is_PUBLISHED",
        std::strstr(buf, "skip_held_heir_exhausted_cap_full=") != nullptr,
        "a split that lives only in the source cannot be read from a bundle");

    std::string s = cap([]{
        bridge::record_skip_held_heir("sess-A", 1001, 2002, 3003, 2);
    });
    row("firing_names_all_four_fields",
        s.find("sess-A") != std::string::npos && s.find("1001") != std::string::npos &&
        s.find("2002") != std::string::npos && s.find("3003") != std::string::npos &&
        s.find("position 2") != std::string::npos,
        "session, dead, would-have-inherited, received, order index");
    row("firing_says_the_seat_was_RETAINED",
        s.find("Seat retained") != std::string::npos,
        "the outcome, not just the event");

    // Exhaustion is a DIFFERENT outcome and must not be folded into the firing
    // count — that would report a seat surviving where one ended.
    std::string e = cap([]{
        bridge::record_skip_held_heir_exhausted("sess-B", 1001, 2002, 5,
                                                "owned_by_other");
    });
    row("exhaustion_says_the_seat_ENDS",
        e.find("THE SEAT ENDS") != std::string::npos &&
        e.find("no character invented") != std::string::npos,
        "refusal is explicit, and says nothing was invented");
    row("exhaustion_demands_the_episode_be_flagged",
        e.find("must be flagged") != std::string::npos,
        "a seat ending is an episode-level fact");

    bridge::format_skip_held_heir(buf, sizeof buf);
    // ck-handoff-swap widened this line with the cap-full subset. The expected
    // string follows it — and cap_full stays 0 HERE on purpose: the exhaustion
    // above was owned_by_other, a genuine collision, which is exactly the case
    // the subset must NOT absorb.
    row("the_two_outcomes_count_separately",
        std::strcmp(buf, "skip_held_heir=1 skip_held_heir_exhausted=1 "
                         "skip_held_heir_exhausted_cap_full=0") == 0, buf);

    // ── THE SYNTHETIC FIXTURE. arma6 does NOT exercise this path: its release
    //    follows the death by 624 failed handoff attempts, so the corpse was
    //    still controlled every time the loop ran. A release BEFORE the death
    //    tick is the case the loop could not reach, and nothing in any sealed
    //    bundle covers it — which is exactly why it has to be synthetic.
    // ck-7yrp CHANGED THIS PARAMETER'S TYPE and left these five callers behind.
    // The third argument was `bool last_steered_is_dead`, evaluated at HANDOFF time;
    // it is now ReleaseSubjectState, recorded AT RELEASE, because a character
    // released alive that dies later is not a corpse release and was being seized.
    // The mapping is exact — false was "alive at the moment we looked" and true was
    // "dead" — so every row below keeps the case it was written for, and the third
    // state (Unknown, nobody recorded it) is ck-7yrp's own lane to cover.
    using S = bridge::ReleaseSubjectState;
    row("ordinary_path_is_untouched",
        bridge::handoff_subject(5001, 0, S::Alive) == 5001 &&
        bridge::handoff_subject(5001, 4000, S::Corpse) == 5001,
        "a live primary wins regardless of what was steered before");
    row("nothing_to_recover_stays_zero",
        bridge::handoff_subject(0, 0, S::Alive) == 0,
        "no primary and nothing steered -> no handoff");
    row("released_CORPSE_is_recovered",
        bridge::handoff_subject(0, 4000, S::Corpse) == 4000,
        "the agent released its own corpse; the death is still seen");
    row("released_LIVE_character_is_NOT_seized",
        bridge::handoff_subject(0, 4000, S::Alive) == 0,
        "the agent chose to stop steering a living character — handing it an "
        "heir would seize a character it deliberately dropped");

    // ── ck-handoff-swap: THE TWO CAUSES ARE DISTINGUISHABLE ──────────────────
    // The old message asserted "NO unheld heir exists in the game's succession
    // order" whatever the reason. At a full roster the cause is watchlist_full
    // and that sentence is FALSE — and a reader of the evidence would have gone
    // looking in the succession code for a roster problem.
    std::string cap_full = cap([]{
        bridge::record_skip_held_heir_exhausted("sess-C", 1001, 2002, 5,
                                                "watchlist_full");
    });
    row("exhaustion_NAMES_its_reason",
        cap_full.find("watchlist_full") != std::string::npos,
        "the refusal that produced it travels with the record");
    row("exhaustion_no_longer_ASSERTS_an_exhausted_order",
        cap_full.find("NO unheld heir exists") == std::string::npos,
        "a cap-full seat is not an exhausted succession; the old text said it was");
    char c2[160];
    bridge::format_skip_held_heir(c2, sizeof c2);
    row("cap_full_is_counted_SEPARATELY",
        std::strstr(c2, "skip_held_heir_exhausted_cap_full=1") != nullptr &&
        std::strstr(c2, "skip_held_heir_exhausted=2") != nullptr,
        "2 exhausted, 1 of them cap-full — a sum alone loses both");

    // ── the walk budget, DRIVEN rather than grepped ───────────────────────────
    {
        bridge::HandoffBackoff bo;
        const bool first_walks = !bridge::handoff_should_skip(bo, 900, 901);
        const bool logged1 = bridge::handoff_note_failure(bo, 900, 901, "watchlist_full");
        row("the_FIRST_encounter_walks_and_logs",
            first_walks && logged1,
            "a new state is never skipped and is always reported");

        // One skip, then a walk: the interval doubles rather than latching.
        int walks = 0, skips = 0;
        for (int i = 0; i < 7; ++i) {
            if (bridge::handoff_should_skip(bo, 900, 901)) { ++skips; continue; }
            ++walks;
            bridge::handoff_note_failure(bo, 900, 901, "watchlist_full");
        }
        char wb[128];
        snprintf(wb, sizeof wb,
                 "7 ticks -> %d walk(s), %d skip(s); bounded, not silent forever",
                 walks, skips);
        row("a_STATIONARY_failure_stops_walking_every_tick",
            skips > walks && walks >= 1, wb);

        bool logged_again = false;
        for (int i = 0; i < 7; ++i)
            if (!bridge::handoff_should_skip(bo, 900, 901))
                logged_again |= bridge::handoff_note_failure(bo, 900, 901,
                                                             "watchlist_full");
        row("an_UNCHANGED_state_is_logged_ONCE",
            !logged_again,
            "2,574 copies of one line said nothing the first did not");

        // THE PART THAT MAKES IT A BACKOFF AND NOT A LATCH.
        row("a_STATE_CHANGE_walks_immediately",
            !bridge::handoff_should_skip(bo, 900, 999),
            "a different heir is a different question — a real succession is "
            "never delayed by a stale entry");
        row("a_NEW_REASON_is_reported_again",
            bridge::handoff_note_failure(bo, 900, 999, "owned_by_other"),
            "watchlist_full and owned_by_other are different defects");
        bridge::HandoffBackoff fresh;
        bridge::handoff_note_failure(fresh, 1, 2, "watchlist_full");
        row("the_backoff_is_CAPPED",
            [&]{ for (int i = 0; i < 40; ++i)
                     bridge::handoff_note_failure(fresh, 1, 2, "watchlist_full");
                 return fresh.backoff == bridge::kHandoffBackoffMax; }(),
            "doubling stops at kHandoffBackoffMax, so a wedged seat is still "
            "re-checked rather than abandoned");
    }

    printf("\n%s (%d failing row%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
