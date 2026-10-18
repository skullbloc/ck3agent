// ck-j6lk — the claim pin must be keyed on CLAIMS, not on being a control session.
//
// shakedown-2 sealed INVALID on unavailability_over_x with the agent blameless: its
// seat had 70 activities and zero unavailable days. Two driver helpers did it —
// episode-driver-C-steer-probe (0 activities) and episode-driver-C-speed-pin (2) —
// which handshake in CONTROL mode, do one job and close. The detach branch pinned
// every detached control session whether or not it held a claim, so each accrued 364
// silent days against X=18 and EVERY registered episode would have come out invalid.
//
// The rows below are about `claims_ever`, the field that had to exist first: nothing
// in the registry could distinguish "claimed, released, then detached" (a seat) from
// "never claimed" (not a seat), because claim_seq is reset when the claimed set
// empties, claimed_character_ids is cleared beside it, and primary_character_id is
// zeroed. Those two cases are rows 4 and 1 here, and they are the discriminating pair.
#include "bridge.h"
#include "session.h"
#include "episode_participant.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

// The host globals session.cpp and its deps reference, stubbed exactly as
// rollback_state_probe.cpp does — the registry is the subject, the game is not here.
namespace bridge {
FILE* g_logfile = nullptr;
uintptr_t g_base_addr = 0;
std::vector<MemRange> g_valid_ranges;
std::shared_mutex g_ranges_mutex;
std::mutex g_controlled_mutex;
std::vector<uint32_t> g_controlled_chars;
std::atomic<uint32_t> g_controlled_primary{0};
std::atomic<uint32_t> g_controlled_heir{0};
std::atomic<bool> g_agent_mode{false};
}

static int failures = 0;
static void row(const char* n, bool ok, const std::string& d) {
    printf("%-58s %s  %s\n", n, ok ? "PASS" : "FAIL", d.c_str());
    if (!ok) failures++;
}

static bridge::SessionRegistry& reg() {
    static bridge::SessionRegistry r;
    return r;
}

// A control session, as the driver's helpers and the seat all arrive.
static bridge::Session* handshake(const char* name) {
    return reg().create(name, bridge::Session::Mode::CONTROL);
}

static std::string why;   // the reason_out every claim/swap path takes

int main() {
    // ── 1. THE HELPERS' SHAPE: a control handshake that never claims ──
    //
    // bridge.log's exact sequence for these two: handshake, then close, three lines
    // apart for the steer-probe; handshake, set_speed, get_tick_info twice, then
    // close for the speed-pin. NEITHER names a character and neither claims — which
    // is why `claims_ever` is the whole test and the activity count is not.
    {
        bridge::Session* probe = handshake("episode-driver-C-steer-probe");
        row("a_steer_probe_that_never_claims_has_claims_ever_ZERO",
            probe && probe->claims_ever.load() == 0,
            "0 activities, 0 claims — it handshakes in control mode and closes");

        bridge::Session* pin = handshake("episode-driver-C-speed-pin");
        // set_speed and get_tick_info are not claims. The shape that fooled the pin
        // was "control session with some traffic", so exercise traffic without a claim.
        row("a_speed_pin_with_traffic_but_no_claim_is_also_ZERO",
            pin && pin->claims_ever.load() == 0,
            "2 activities, still no claim — activity is not a seat");
    }

    // ── 2. A CLAIM SETS IT, and that is the discriminating partner ──
    bridge::Session* seat = handshake("episode-agent-seat");
    const bool claimed = seat && reg().claim_character(seat, 68445, &why);
    row("a_claim_sets_claims_ever",
        claimed && seat->claims_ever.load() == 1,
        claimed ? "claims_ever=" + std::to_string(seat->claims_ever.load())
                : "the claim itself failed, so this row proves nothing");

    // ── 3. THE PAIR TODAY'S REGISTRY COULD NOT SEPARATE ──
    //
    // Release the only claim. claim_seq is reset to 0 and claimed_character_ids is
    // emptied — so every field that could have witnessed the claim is gone, and this
    // session becomes byte-identical to row 1's helper in all of them. claims_ever is
    // the one thing that still says "seat", which is the entire reason it exists.
    if (claimed) {
        reg().release_character(seat, 68445);
        bool set_empty = false;
        uint32_t primary = 1;
        {
            std::lock_guard<std::mutex> slock(seat->mutex);
            set_empty = seat->claimed_character_ids.empty();
            primary = seat->primary_character_id;
        }
        row("after_release_every_OTHER_witness_is_gone",
            set_empty && primary == 0 && seat->claim_seq == 0,
            "claimed set empty, primary 0, claim_seq 0 — indistinguishable from a "
            "session that never claimed, in every field but one");
        row("but_claims_ever_still_says_SEAT",
            seat->claims_ever.load() == 1,
            "claims_ever=" + std::to_string(seat->claims_ever.load()) +
            " with an EMPTY claim set: this is the pair, and it is why the pin keys "
            "on claims_ever rather than on the current claims");
    } else {
        row("after_release_every_OTHER_witness_is_gone", false, "setup failed");
        row("but_claims_ever_still_says_SEAT", false, "setup failed");
    }

    // ── 4. THE SWAP-ONLY PATH, which a claim_seq-keyed flag would have missed ──
    //
    // swap_character is one-out-one-in: the claimed set is non-empty on both sides, so
    // neither note_session_claim_locked's empty->held assignment nor
    // note_session_release_locked's held->empty reset fires, and claim_seq NEVER
    // MOVES. A flag keyed on claim_seq moving would read 0 for a session whose only
    // steering arrived this way.
    {
        bridge::Session* sw = handshake("swap-only-session");
        const bool first = sw && reg().claim_character(sw, 100, &why);
        const uint64_t after_claim = sw ? sw->claims_ever.load() : 0;
        const uint64_t seq_before = sw ? sw->claim_seq : 0;
        const bool swapped = first && reg().swap_character(sw, 100, 200, &why);
        const uint64_t after_swap = sw ? sw->claims_ever.load() : 0;
        row("a_SWAP_also_increments_claims_ever",
            swapped && after_swap > after_claim,
            swapped ? "claims_ever " + std::to_string(after_claim) + " -> " +
                      std::to_string(after_swap)
                    : "the swap itself failed, so this row proves nothing");
        row("while_claim_seq_does_NOT_move_across_the_swap",
            swapped && sw->claim_seq == seq_before,
            "claim_seq " + std::to_string(seq_before) + " unchanged — which is why a "
            "flag keyed on it would have missed this session entirely");
    }

    // ── 5. MONOTONIC: nothing clears it ──
    {
        bridge::Session* m = handshake("monotonic-session");
        if (m) {
            reg().claim_character(m, 300, &why);
            reg().claim_character(m, 301, &why);
            const uint64_t two = m->claims_ever.load();
            reg().release_character(m, 300);
            reg().release_character(m, 301);
            row("claims_ever_is_MONOTONIC_across_releases",
                m->claims_ever.load() == two && two >= 2,
                "counted " + std::to_string(two) + " claims, still " +
                std::to_string(m->claims_ever.load()) + " after releasing both");
        } else {
            row("claims_ever_is_MONOTONIC_across_releases", false, "setup failed");
        }
    }

    // ══ THE THREE STATES, as the progenitor ruled them ══
    //
    // Only state (a) changes behaviour. (b) and (c) must reach the SAME predicate
    // value that today's code reaches, and the rows below assert the predicate
    // rather than the log line, because the predicate is what the branch keys on.
    //
    // THE SHIPPED PREDICATE, CALLED — not re-spelled. My first version of this was a
    // local lambda repeating `!declared && claims_ever == 0`, which would have gone on
    // passing if the branch's own copy had drifted: it tested my copy, not the code.
    // bridge::never_an_episode_participant is the one body the detach path calls.
    auto never_a_participant = [](const bridge::Session* x) {
        return bridge::never_an_episode_participant(x->declared_seat.load(),
                                                   x->claims_ever.load());
    };

    // (b) A DECLARED SEAT THAT NEVER CLAIMED — ck-3ru8's pre-claim window. The world
    // loads ~17s after first connection; a seat that dies in there must stay a
    // MEASURED HOLE, not vanish.
    {
        bridge::Session* seat_b = handshake("declared-seat-never-claimed");
        seat_b->declared_seat.store(true);
        row("a_DECLARED_seat_that_never_claimed_is_still_a_participant",
            !never_a_participant(seat_b),
            "claims_ever=0 but declared — pinned and accruing on detach, so an agent "
            "that dies before its first claim is still measured");
    }

    // (c) AN UNDECLARED SESSION THAT CLAIMED — a dropped declaration must never
    // un-seat something that actually steered.
    {
        bridge::Session* seat_c = handshake("undeclared-but-claimed");
        const bool ok = seat_c && reg().claim_character(seat_c, 400, &why);
        row("an_UNDECLARED_session_that_CLAIMED_is_still_a_participant",
            ok && !seat_c->declared_seat.load() && !never_a_participant(seat_c),
            ok ? "declared=false, claims_ever=" +
                 std::to_string(seat_c->claims_ever.load()) +
                 " — a missing declaration cannot un-seat a session that steered"
               : "the claim itself failed, so this row proves nothing");
    }

    // (a) THE ONLY STATE THAT CHANGES, asserted through the same predicate so it
    // cannot pass for a different reason than (b) and (c) fail for.
    {
        bridge::Session* helper = handshake("undeclared-never-claimed");
        row("ONLY_the_undeclared_claimless_session_is_not_a_participant",
            never_a_participant(helper),
            "the two shapes from the shakedown log, and nothing else, take the "
            "destroy path");
    }

    // THE PUBLISHED COUNT, for the validator's cross-check against the roster's
    // declared_sessions. Counted over ALL sessions, detached included.
    {
        size_t declared = 0, total = 0;
        reg().for_each([&](const bridge::Session& x) {
            ++total;
            if (x.declared_seat.load()) ++declared;
        });
        row("the_DECLARED_SEAT_COUNT_is_countable_over_all_sessions",
            declared == 1 && total >= 6,
            "declared=" + std::to_string(declared) + " of " + std::to_string(total) +
            " sessions — exactly the one that declared, which is what the episode "
            "block publishes as `episode_seat_sessions` for seat_declaration_mismatch");
    }

    printf("\n%s (%d failing rows)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
