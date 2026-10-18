// handoff_ai_state_probe.cpp — DOES A SUCCESSION LEAVE THE HEIR STEERABLE? (ck-48z8)
//
// check_controlled_handoff disabled the heir's per-character AI tick whenever
// g_agent_mode was true, under a comment claiming the opposite ("Re-enable agent
// steering of the heir"). g_agent_mode is rebuilt as `primary != 0`, so it is TRUE
// in every arm episode that holds any control session at all — whatever that
// session asked for. The registered design is STEER mode ("the last personality
// steer persists; the native AI executes as it always does") and
// do_set_controlled_character defaults disable_native_ai=false.
//
// So every succession in set28 promoted its heir into Drive mode: no self-declared
// wars, no sieges, occupation stuck at 0. ep-03's agent said so itself after the
// handoff to 47368 — "the bridge has disabled his native AI, freezing his tick" —
// and then made nine set_character_ai calls trying to undo it. Its 23 control calls
// all carried disable_native_ai:false explicitly; true appears zero times.
//
// WHY A TRUTH TABLE AND NOT A GREP. The decision is extracted so a fixture can
// drive it, following ck-7rk6: inline in the tick loop it is a condition a test can
// only grep for, and a grep cannot tell a live condition from a defeated one.
// agent_mode is a parameter the function ignores ON PURPOSE — a signature that
// cannot express the wrong rule cannot be tested against it, so the wrong input is
// present and these rows prove the answer does not move with it.

#include "skip_held_heir.h"

#include <cstdio>
#include <string>

static int g_failures = 0;
static void row(const char* name, bool ok, const std::string& detail) {
    printf("%-54s %s  %s\n", name, ok ? "PASS" : "FAIL", detail.c_str());
    if (!ok) ++g_failures;
}

int main() {
    // ── THE CELL THAT WAS BROKEN, and it is the only cell set28 ever ran ──────
    row("default_take_with_agent_mode_ON_leaves_AI_ENABLED",
        !bridge::handoff_should_disable_heir_ai(/*claim_disabled=*/false,
                                               /*agent_mode=*/true),
        "every set28 take was disable_native_ai=false and every arm episode has "
        "agent_mode true — so this is THE production cell, and it used to disable");

    row("default_take_with_agent_mode_off_leaves_AI_ENABLED",
        !bridge::handoff_should_disable_heir_ai(false, false),
        "unchanged, which is what makes the row above a statement about agent_mode "
        "rather than about the default");

    // ── AND THE EXPLICIT TAKE IS STILL HONOURED ──────────────────────────────
    row("an_EXPLICIT_disable_take_still_disables",
        bridge::handoff_should_disable_heir_ai(true, true),
        "a session that asked for Drive mode keeps it across the succession — the "
        "fix preserves the claim's state, it does not hardcode ENABLED");

    row("and_does_so_independently_of_agent_mode",
        bridge::handoff_should_disable_heir_ai(true, false),
        "same answer with agent_mode off");

    // ── THE PROPERTY, STATED AS A PROPERTY ───────────────────────────────────
    // Two rows agreeing is a table; the claim is that ONE input decides. Assert
    // that directly, so a future edit that starts consulting agent_mode reds here
    // even if it happens to agree on the four cells above.
    bool moves_with_agent_mode = false;
    for (int cd = 0; cd <= 1; ++cd) {
        if (bridge::handoff_should_disable_heir_ai(cd != 0, true)
            != bridge::handoff_should_disable_heir_ai(cd != 0, false)) {
            moves_with_agent_mode = true;
        }
    }
    row("the_answer_NEVER_moves_with_agent_mode", !moves_with_agent_mode,
        "agent_mode answers 'is anyone steering anything'; a handoff needs 'did "
        "THIS claim disable the AI'. They differ for every default take, which is "
        "all of them");

    bool tracks_the_claim = true;
    for (int am = 0; am <= 1; ++am) {
        if (bridge::handoff_should_disable_heir_ai(false, am != 0) != false) tracks_the_claim = false;
        if (bridge::handoff_should_disable_heir_ai(true,  am != 0) != true)  tracks_the_claim = false;
    }
    row("and_tracks_the_CLAIM_exactly", tracks_the_claim,
        "the decision IS the claim's flag — preserved, not re-derived");

    printf("\n%s (%d failing rows)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
