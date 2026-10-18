// handoff_release_state_probe.cpp — WAS IT ALIVE WHEN THE AGENT LET IT GO? (ck-7yrp)
//
// handoff_subject's own documentation draws its distinction on the state AT
// RELEASE: "primary==0, last is ALIVE -> the agent chose to stop steering a living
// character. NOT a succession. Hands it no heir, because inventing one would seize
// a character the agent deliberately dropped" versus "primary==0, last is DEAD ->
// the agent released its own corpse and blinded the handoff. Recover it."
//
// The test performed was `last_steered_is_dead` evaluated AT HANDOFF TIME. Those
// two agree only while nothing dies in between, and in set28 ep-03 something did:
// the agent released 36552 ALIVE at tick ~3200 (the log line reads "re-enabled
// native AI for 36552", which needs a live ai_ctrl), it died later, and at tick
// ~6300 the recovery branch fired and seized it — 3100 ticks after a deliberate
// drop, where a genuine corpse release is recovered at the next check ~50 ticks
// later. The comment was a literal description of what the code was meant to do.
//
// THE PARAMETER'S TYPE IS THE FIX. A bool could not express "nobody recorded it",
// and release_character has many callers — tests and internal paths — none of which
// can see aliveness, because lookup_character and g_char_is_alive live on the engine
// side. So the state is three-valued and Unknown means NO RECOVERY: the cost of a
// wrong recovery is seizing a character the agent chose to release, and the cost of
// a missed one is a seat that stays empty until the agent re-claims.

#include "skip_held_heir.h"

#include <cstdio>
#include <string>

static int g_failures = 0;
static void row(const char* name, bool ok, const std::string& detail) {
    printf("%-58s %s  %s\n", name, ok ? "PASS" : "FAIL", detail.c_str());
    if (!ok) ++g_failures;
}

int main() {
    using S = bridge::ReleaseSubjectState;
    const uint32_t kLast = 36552;   // ep-03's deliberately dropped character
    const uint32_t kHeld = 47368;   // the character the recovery branch seized into

    // ── THE ep-03 CASE, which is the whole bead ──────────────────────────────
    row("released_ALIVE_is_never_recovered_even_after_it_dies",
        bridge::handoff_subject(0, kLast, S::Alive) == 0,
        "the state is the one recorded AT RELEASE, so a later death cannot turn a "
        "deliberate drop into a succession — this is the seizure ep-03 suffered");

    // ── AND THE CASE THE RECOVERY EXISTS FOR STILL WORKS ─────────────────────
    row("released_as_a_CORPSE_is_recovered",
        bridge::handoff_subject(0, kLast, S::Corpse) == kLast,
        "a seat that released its own corpse blinded the handoff; recovering it is "
        "the ck-7rk6 feature and it is preserved");

    // ── ABSENCE IS NOT A CORPSE ──────────────────────────────────────────────
    row("UNKNOWN_is_not_recovered",
        bridge::handoff_subject(0, kLast, S::Unknown) == 0,
        "nobody recorded it, so nothing is seized — absence is a state, not a "
        "default that happens to look like an answer");

    // ── A HELD PRIMARY STILL WINS, whatever was released before ──────────────
    bool primary_wins = true;
    for (S s : {S::Unknown, S::Alive, S::Corpse})
        if (bridge::handoff_subject(kHeld, kLast, s) != kHeld) primary_wins = false;
    row("a_held_primary_is_returned_regardless_of_the_release_state", primary_wins,
        "the recovery branch is only reachable when primary is 0; this is the row "
        "that keeps the fix from changing the ordinary path");

    // ── NO LAST SUBJECT MEANS NOTHING TO RECOVER ─────────────────────────────
    bool no_last = true;
    for (S s : {S::Unknown, S::Alive, S::Corpse})
        if (bridge::handoff_subject(0, 0, s) != 0) no_last = false;
    row("no_last_subject_yields_nothing_in_every_state", no_last,
        "a Corpse state with no id must not invent one");

    // ── THE PROPERTY: ONLY Corpse RECOVERS ───────────────────────────────────
    // Three rows agreeing is a table; the claim is that exactly one state recovers.
    // Assert it directly, so a future edit that starts recovering on Unknown reds
    // here even if it happens to agree on the cells above.
    int recovering = 0;
    for (S s : {S::Unknown, S::Alive, S::Corpse})
        if (bridge::handoff_subject(0, kLast, s) != 0) ++recovering;
    row("EXACTLY_ONE_state_recovers_and_it_is_Corpse",
        recovering == 1 && bridge::handoff_subject(0, kLast, S::Corpse) == kLast,
        std::to_string(recovering) + " of 3 states recover — the conservative "
        "direction is the one that cannot seize");

    // ── THE NAMES ARE WHOLE LITERALS, for the log's witness ──────────────────
    row("each_state_has_its_own_literal_name",
        std::string(bridge::release_subject_state_name(S::Alive)) == "alive"
        && std::string(bridge::release_subject_state_name(S::Corpse)) == "corpse"
        && std::string(bridge::release_subject_state_name(S::Unknown)) == "unknown",
        "so the branch log's state is findable by `strings` rather than assembled "
        "at runtime");

    printf("\n%s (%d failing rows)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
