// ck-7rk6 — count every skip-held-heir firing, and say zero out loud.
//
// THE RULE (progenitor, "i approve with recommended option"; relayed by bt-bbd):
// on a succession collision — the game's heir for a controlled character is
// ALREADY held by another participant session, so claim_character() fails and
// that session loses its seat (arma6's six->five) — the losing session instead
// CONTINUES under the next eligible heir in the GAME'S OWN succession order who
// is not held by anyone. Seat count stays six, so the instrument does not change
// across arms and there is no post-treatment variable to split on.
//
// WHY THIS COUNTER IS REQUIRED AND NOT OPTIONAL (ck-m1u1's correction of an
// overclaim): the rule is applied IDENTICALLY to every arm, but it FIRES at each
// arm's own collision rate, which is UNMEASURED — one instance so far. So the
// residual asymmetry is bounded and declared, and it must be COUNTED rather than
// assumed. An arm where this fired nine times and an arm where it never fired
// are not the same experiment, and only a count can tell them apart.
//
// A CLEAN RUN SAYS skip_held_heir=0 EXPLICITLY. An omitted field is
// indistinguishable from an older binary that never had one — the same rule that
// decided UNKNOWN-not-0 for an unresolvable elapsed, "refusals=none" on a quiet
// heartbeat, and "rip_off=UNRESOLVED" for an unresolved fault address.
#pragma once

#include <atomic>
#include <string>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cstdio>

namespace bridge {

extern FILE* g_logfile;

// Fired: a collision occurred and a later heir in the game's order was claimed.
inline std::atomic<uint64_t> g_skip_held_heir_fired{0};
// Exhausted: a collision occurred and NO unheld heir existed in the game's
// order. The seat ends. Counted separately because it is a different outcome
// for the episode, and folding it into the firing count would report a seat
// that survived where one did not.
inline std::atomic<uint64_t> g_skip_held_heir_exhausted{0};
// ck-handoff-swap: the cap-full SUBSET of the exhausted count. Two causes that
// need different fixes must be two numbers, or a bundle records their sum and no
// reader can recover either. A census reads this instead of parsing the message.
inline std::atomic<uint64_t> g_skip_held_heir_exhausted_cap_full{0};

// Records ONE firing with everything bt-bbd asked for: which session, the
// character it WOULD have inherited, the one it RECEIVED, and where that one sat
// in the game's own succession order.
//
// `received` is also what the caller must write into session->heir_character_id.
// ck-9rto drove their predicate on both options: recording the REDIRECTED heir
// grades as succession; recording the PRIMARY heir while claiming the redirect
// grades as `change` and INVALIDATES the episode. The invariant their witness
// reads is `heir_character_id == the character actually claimed`, so the
// succession INDEX below is evidence beside the witness, never inside it.
inline void record_skip_held_heir(const char* session_id, uint32_t dead,
                                  uint32_t would_have, uint32_t received,
                                  int order_index) {
    g_skip_held_heir_fired.fetch_add(1, std::memory_order_relaxed);
    if (g_logfile) {
        fprintf(g_logfile,
            "[CK3-Bridge] SKIP_HELD_HEIR: session %s, %u died; heir %u was "
            "already held by another participant, so this session received %u "
            "(position %d in the game's succession order). Seat retained "
            "(ck-7rk6)\n",
            session_id ? session_id : "?", dead, would_have, received, order_index);
        fflush(g_logfile);
    }
}

// Records the REFUSAL: no unheld heir existed. The seat ends. No character is
// invented — bt-bbd's explicit instruction, and the alternative (handing the
// session someone outside the game's succession order) would make the bridge the
// author of a succession the game did not perform.
// ck-handoff-swap: THE REASON IS A PARAMETER, because the old message asserted a
// cause it could not know. It said the succession order held no unheld heir —
// true when the refusal is owned_by_other, FALSE when it is watchlist_full, and
// at a full roster watchlist_full was the whole population. One counter and one
// sentence served two defects with different fixes, so the structured witness
// could not tell "every heir is taken" from "this seat has no room", and a reader
// of the evidence would have gone looking in the succession code.
inline void record_skip_held_heir_exhausted(const char* session_id, uint32_t dead,
                                            uint32_t would_have, int order_len,
                                            const char* reason) {
    g_skip_held_heir_exhausted.fetch_add(1, std::memory_order_relaxed);
    const char* why = (reason && reason[0]) ? reason : "reason_not_reported";
    if (std::strcmp(why, "watchlist_full") == 0) {
        g_skip_held_heir_exhausted_cap_full.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_logfile) {
        fprintf(g_logfile,
            "[CK3-Bridge] SKIP_HELD_HEIR EXHAUSTED: session %s, %u died; heir %u "
            "could not be taken (%s) and no candidate in the game's succession "
            "order could either (%d examined). THE SEAT ENDS — no character "
            "invented; this episode must be flagged (ck-7rk6)\n",
            session_id ? session_id : "?", dead, would_have, why, order_len);
        fflush(g_logfile);
    }
}

// WHICH CHARACTER THE HANDOFF SHOULD TREAT AS THE DEAD RULER.
//
// Extracted so the synthetic fixture can DRIVE it. Written inline in the tick
// loop this was a condition a test could only grep for, and a grep cannot tell
// a live condition from a defeated one — the lesson that cost two lanes on
// ck-1v1t. arma6 does NOT exercise the release-before-death path (its release
// follows the death by 624 failed handoff attempts), so a synthetic fixture is
// the ONLY thing that can cover it.
//
//   primary != 0                  -> the ordinary path, unchanged
//   primary == 0, no last steered -> nothing to recover
//   primary == 0, last is ALIVE   -> the agent chose to stop steering a living
//                                    character. NOT a succession. Hands it no
//                                    heir, because inventing one would seize a
//                                    character the agent deliberately dropped.
//   primary == 0, last is DEAD    -> the agent released its own corpse and
//                                    blinded the handoff. Recover it.
//: ck-7yrp: WAS THE RELEASED SUBJECT ALIVE OR DEAD **AT RELEASE**?
//:
//: handoff_subject's own documentation draws the distinction on the state AT
//: RELEASE — "the agent chose to stop steering a LIVING character. NOT a
//: succession" versus "the agent released its own CORPSE and blinded the handoff.
//: Recover it" — and the test performed was `last_steered_is_dead` evaluated AT
//: HANDOFF TIME. Those agree only while nothing dies in between. In set28 ep-03
//: the agent released 36552 alive at tick ~3200; it died later; and at tick ~6300
//: the recovery branch fired and seized it, 3100 ticks after a deliberate drop.
//: The comment was a literal description of what the code was supposed to do.
//:
//: THREE STATES, NOT TWO. `Unknown` exists because release_character has many
//: callers — tests and internal paths — and the registry itself cannot see
//: aliveness (lookup_character and g_char_is_alive live on the engine side). A
//: caller that does not know must be able to SAY it does not know, and Unknown
//: must mean NO RECOVERY: inventing an heir for a character whose release we
//: cannot characterise would seize one the agent may have dropped on purpose, and
//: that is the failure this bead exists to stop. Absence is a state, never a
//: default that happens to look like an answer.
enum class ReleaseSubjectState : uint8_t {
    Unknown = 0,   //: nobody recorded it — do NOT recover
    Alive   = 1,   //: a deliberate drop of a living character — do NOT recover
    Corpse  = 2,   //: the seat released its own corpse — recovery is correct

    //: ck-qv51: A CLAIM BEING ROLLED BACK, which is not a release at all.
    //:
    //: do_set_controlled_character claims a character, then checks the roster; if the
    //: roster diverged it calls release_character to undo the claim. The seat NEVER
    //: STEERED that character. ck-3ru8's census found this path reaching the handoff
    //: with Unknown — but the worse half is that it also overwrote
    //: last_steered_character_id with the undone id, and that field is set only on the
    //: way out of primary and never cleared, so a genuine earlier value was destroyed
    //: and a later real succession could be blinded.
    //:
    //: SEVERITY IS BOUNDED AT "SEAT STAYS EMPTY", measured by ck-zgh4: the id and the
    //: state are written as a pair inside one session->mutex and one
    //: `primary_character_id == char_id` guard, so they cannot be torn. A blinded
    //: handoff returns 0; it can never recover the WRONG character. That ceiling is
    //: what ck-qv51 and ck-cwal are negotiating over.
    //:
    //: So this state means: update the primary, because the claim really is gone, but
    //: record NOTHING about a release that did not happen.
    UndoneClaim = 3,

    //: rc2 B6: the ruler died and its heir is the HUMAN PLAYER, so the handoff
    //: unbound the seat. Not Corpse, so handoff_subject never recovers it and
    //: the handoff does not retry the player every tick.
    HeirIsPlayer = 4,
};

inline const char* release_subject_state_name(ReleaseSubjectState s) {
    //: Whole literals, one per branch, so the log line's state is findable by
    //: `strings` rather than assembled at runtime.
    switch (s) {
        case ReleaseSubjectState::Alive:  return "alive";
        case ReleaseSubjectState::Corpse: return "corpse";
        case ReleaseSubjectState::UndoneClaim: return "undone_claim";
        case ReleaseSubjectState::HeirIsPlayer: return "heir_is_player";
        default:                          return "unknown";
    }
}

//: ck-7yrp: THE STATE AT RELEASE, NOT THE STATE NOW.
//:
//: The old signature took `bool last_steered_is_dead`, which the caller evaluated
//: at HANDOFF time — so a character released ALIVE and dying later looked exactly
//: like a corpse release, and the recovery branch seized it. The parameter's TYPE
//: is the fix: a bool could not express "I do not know", and the three cases here
//: are genuinely three.
inline uint32_t handoff_subject(uint32_t primary_character_id,
                                uint32_t last_steered_character_id,
                                ReleaseSubjectState state_at_release) {
    if (primary_character_id != 0) return primary_character_id;
    if (last_steered_character_id == 0) return 0;
    //: ONLY a recorded corpse. Alive is a deliberate drop; Unknown is nobody
    //: looking. Both mean NO RECOVERY, because the cost of a wrong recovery is
    //: seizing a character the agent chose to release, and the cost of a missed
    //: one is a seat that stays empty until the agent re-claims.
    return state_at_release == ReleaseSubjectState::Corpse
               ? last_steered_character_id
               : 0;
}

// ── ck-handoff-swap: THE PER-TICK WALK BUDGET, EXTRACTED SO A FIXTURE CAN DRIVE IT
//
// The 64-candidate walk answers a COLLISION question, and the answer does not
// change between adjacent ticks: same corpse, same heir, same holders, same
// result. Re-walking every tick turned ONE unresolvable handoff into 2,574
// indistinguishable watchlist_full lines in diag25, and the volume is what hid
// it — a reader cannot tell 2,574 attempts from 2,574 distinct problems.
//
// A BACKOFF AND NOT A LATCH. The answer CAN change later: another session
// releasing a character frees a candidate. A permanent skip would wedge a
// handoff that becomes resolvable, which is a worse defect than the noise.
//
// Extracted for the reason ck-7rk6 extracted handoff_subject: written inline in
// the tick loop this is a condition a test can only grep for, and a grep cannot
// tell a live condition from a defeated one.
struct HandoffBackoff {
    uint32_t    dead       = 0;
    uint32_t    heir       = 0;
    std::string reason;
    uint32_t    skips_left = 0;
    uint32_t    backoff    = 0;
};

//: Cap on the doubling, so the retry interval stays bounded and a long-running
//: episode still re-checks a wedged seat roughly once per 1024 ticks.
inline constexpr uint32_t kHandoffBackoffMax = 1024u;

//: True when this tick should SKIP the candidate walk. Consumes one skip. A
//: state CHANGE (different corpse or different heir) never skips — a real
//: succession is never delayed by a stale entry.
//: ck-48z8: DOES THE HEIR'S NATIVE AI GET DISABLED AT A HANDOFF?
//:
//: Extracted for the reason ck-7rk6 extracted handoff_subject: written inline in
//: the tick loop this is a condition a test can only GREP for, and a grep cannot
//: tell a live condition from a defeated one.
//:
//: agent_mode IS A PARAMETER IT DELIBERATELY IGNORES. That is not an oversight —
//: it is what lets a fixture REFUTE the defect. The old code disabled the heir's
//: AI whenever g_agent_mode was true, and g_agent_mode is rebuilt as
//: `primary != 0`, so it is true in every arm episode with any control session
//: whatever that session asked for. A signature that could not express the wrong
//: rule could not be tested against it, so the wrong input is present and the
//: truth table proves the answer does not move with it.
//:
//: THE REGISTERED DESIGN IS STEER MODE: "the last personality steer persists; the
//: native AI executes as it always does". So the only thing that may disable an
//: heir's AI is an explicit disable_native_ai=true on the claim this session made.
inline bool handoff_should_disable_heir_ai(bool claim_disabled_native_ai,
                                          bool agent_mode) {
    (void)agent_mode;   //: present to be proven irrelevant, never consulted
    return claim_disabled_native_ai;
}

inline bool handoff_should_skip(HandoffBackoff& bo, uint32_t dead, uint32_t heir) {
    if (bo.dead != dead || bo.heir != heir) return false;
    if (bo.skips_left == 0) return false;
    --bo.skips_left;
    return true;
}

//: Record that the walk found nothing. Returns TRUE when this is a state the
//: caller has not already reported, which is the only time it should log or emit
//: a structured record — once per state change, not once per tick.
inline bool handoff_note_failure(HandoffBackoff& bo, uint32_t dead, uint32_t heir,
                                 const char* reason) {
    const char* why = (reason && reason[0]) ? reason : "reason_not_reported";
    const bool changed = (bo.dead != dead || bo.heir != heir || bo.reason != why);
    if (changed) {
        bo.dead = dead;
        bo.heir = heir;
        bo.reason = why;
        bo.backoff = 0;          //: a NEW state starts its own backoff sequence
    }
    bo.backoff = bo.backoff ? std::min<uint32_t>(bo.backoff * 2u, kHandoffBackoffMax)
                            : 1u;
    bo.skips_left = bo.backoff;
    return changed;
}

inline const char* format_skip_held_heir(char* buf, size_t n) {
    // The cap-full subset RIDES THE SAME LINE. A counter that exists, is
    // incremented, and is never published is not evidence of anything — the
    // split would have lived only in the source.
    snprintf(buf, n, "skip_held_heir=%llu skip_held_heir_exhausted=%llu "
                     "skip_held_heir_exhausted_cap_full=%llu",
             (unsigned long long)g_skip_held_heir_fired.load(std::memory_order_relaxed),
             (unsigned long long)g_skip_held_heir_exhausted.load(std::memory_order_relaxed),
             (unsigned long long)g_skip_held_heir_exhausted_cap_full.load(std::memory_order_relaxed));
    return buf;
}

} // namespace bridge
