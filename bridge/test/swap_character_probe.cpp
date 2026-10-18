// ck-swap — the switch must work AT the cap, and must not become a way past it.
//
// Drives the REAL SessionRegistry in-process, which is the only place these
// assertions are honest: ck-zgh4 established that a refusal count read from a
// sealed bundle returns 0 both when the fix works and when the evidence has
// rotated away, so the grading cannot live in a log.
#include "bridge.h"
#include "session.h"
#include <cstdio>
#include <cstring>
#include <shared_mutex>
#include <string>
#include <vector>

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
using namespace bridge;

static int failures = 0;
static void row(const char* n, bool ok, const std::string& d) {
    printf("%-58s %s  %s\n", n, ok ? "PASS" : "FAIL", d.c_str());
    if (!ok) failures++;
}
static size_t held(Session* s) {
    std::lock_guard<std::mutex> lk(s->mutex);
    return s->claimed_character_ids.size();
}
static uint32_t primary(Session* s) {
    std::lock_guard<std::mutex> lk(s->mutex);
    return s->primary_character_id;
}

int main() {
    // ── FILL THE ROSTER TO THE CAP. The row that matters only exists here, and
    // a test that does not fill it never runs the code it is grading.
    std::vector<Session*> seats;
    for (int i = 0; i < 6; ++i) {
        char sid[32]; snprintf(sid, sizeof sid, "seat-%d", i);
        Session* s = g_session_registry.create(sid, Session::Mode::CONTROL);
        seats.push_back(s);
        std::string why;
        g_session_registry.claim_character(s, 1000 + i, &why);
    }
    // four more onto seat 0 to reach 10/10
    for (int k = 0; k < 4; ++k) {
        std::string why;
        g_session_registry.claim_character(seats[0], 2000 + k, &why);
    }
    const size_t occupancy = g_session_registry.controlled_claim_count();
    row("the_roster_is_AT_the_cap",
        occupancy == MAX_CONTROLLED_WATCHLIST,
        std::to_string(occupancy) + "/" + std::to_string(MAX_CONTROLLED_WATCHLIST));

    // ── A CLAIM IS REFUSED HERE. This is diag25's condition.
    std::string claim_why;
    const bool claimed = g_session_registry.claim_character(seats[1], 9001, &claim_why);
    row("a_CLAIM_at_the_cap_is_refused_watchlist_full",
        !claimed && claim_why == "watchlist_full", claim_why);

    // ── AND THE SWAP SUCCEEDS, with claim_seq UNCHANGED.
    Session* sub = seats[1];
    const uint64_t seq_before = sub->claim_seq;
    const size_t held_before = held(sub);
    std::string swap_why;
    const bool swapped = g_session_registry.swap_character(sub, 1001, 9001, &swap_why);
    row("the_SWAP_at_the_cap_SUCCEEDS",
        swapped, swapped ? "net zero, so the cap has nothing to refuse"
                         : ("refused: " + swap_why));
    row("claim_seq_is_UNCHANGED_across_the_swap",
        sub->claim_seq == seq_before,
        std::to_string(seq_before) + " -> " + std::to_string(sub->claim_seq)
        + "  (a move is what SS7.3i rejects)");
    row("the_seat_holds_the_SAME_NUMBER_of_slots",
        held(sub) == held_before,
        std::to_string(held_before) + " -> " + std::to_string(held(sub)));
    row("the_seat_now_steers_the_NEW_character",
        primary(sub) == 9001, std::to_string(primary(sub)));
    row("occupancy_is_unchanged",
        g_session_registry.controlled_claim_count() == occupancy,
        std::to_string(g_session_registry.controlled_claim_count()));

    // ── THE FAST-PATH PIN. claim_character's top inserts and returns true with
    // NO cap check, reachable only when ownership_ and a session's claim set
    // disagree. A swap that updated one and not the other would open it.
    std::string div;
    row("the_swap_leaves_NO_ownership_divergence",
        !g_session_registry.ownership_divergence(&div),
        div.empty() ? "watchlist and ownership agree" : div);

    // ── IT IS NOT A BACK DOOR. `from` not held is a net +1, so the cap applies.
    std::string nd_why;
    const bool netplus = g_session_registry.swap_character(seats[2], 77777, 9002, &nd_why);
    row("a_swap_whose_FROM_is_not_held_is_capped",
        !netplus && nd_why == "watchlist_full",
        nd_why.empty() ? "(no reason)" : nd_why);

    // ── AND IT REFUSES ANOTHER SEAT'S CHARACTER.
    std::string ob_why;
    const bool steal = g_session_registry.swap_character(seats[3], 1003, 1004, &ob_why);
    row("a_swap_onto_another_seats_character_is_refused",
        !steal && ob_why == "owned_by_other", ob_why);

    // ── AN OBSERVE SESSION HOLDS NOTHING, INCLUDING VIA SWAP.
    Session* obs = g_session_registry.create("watcher", Session::Mode::OBSERVE);
    std::string obs_why;
    row("an_observe_session_cannot_swap",
        !g_session_registry.swap_character(obs, 1000, 9003, &obs_why)
        && obs_why == "observe_mode", obs_why);

    // ── RULING FIVE'S REQUIRED ROW: DETACHED and LOST STILL HOLD CLAIMS
    // (session.h:148), and the counter skips only OBSERVE. A seat that
    // reconnects WITHOUT resuming consumes two slots, so the cap must see both
    // — the guard that survives the swap and the reason control_refused is kept
    // even after it.
    {
        const size_t before = g_session_registry.controlled_claim_count();
        seats[4]->state = Session::State::DETACHED;
        const size_t detached = g_session_registry.controlled_claim_count();
        seats[4]->state = Session::State::LOST;
        const size_t lost = g_session_registry.controlled_claim_count();
        seats[4]->state = Session::State::ATTACHED;
        row("a_DETACHED_seats_claims_still_count_against_the_cap",
            detached == before,
            std::to_string(before) + " attached vs " + std::to_string(detached)
            + " detached — excluding them would refuse a claim the census said "
              "there was room for");
        row("a_LOST_seats_claims_still_count_too",
            lost == before,
            std::to_string(lost) + " — LOST is reported, never a silent release");
    }

    // ── THE SLOT-LEAK BOUNDARY the refinery asked to be graded. A swap must be
    // net zero at the cap; a leak of ONE slot is the failure that would let the
    // roster creep. Assert the invariant directly rather than trusting the
    // earlier equality: occupancy AND this seat's holding, across many swaps.
    {
        Session* churn = seats[5];
        const size_t occ0 = g_session_registry.controlled_claim_count();
        const size_t held0 = held(churn);
        const uint64_t seq0 = churn->claim_seq;
        uint32_t cur = 1005;
        bool all_ok = true;
        for (uint32_t k = 0; k < 20; ++k) {
            const uint32_t nxt = 30000 + k;
            std::string w;
            if (!g_session_registry.swap_character(churn, cur, nxt, &w)) {
                all_ok = false; break;
            }
            cur = nxt;
        }
        const size_t occ1 = g_session_registry.controlled_claim_count();
        row("twenty_swaps_leak_NO_slot",
            all_ok && occ1 == occ0 && held(churn) == held0,
            "occupancy " + std::to_string(occ0) + " -> " + std::to_string(occ1)
            + ", held " + std::to_string(held0) + " -> " + std::to_string(held(churn)));
        row("and_claim_seq_never_moved_across_any_of_them",
            churn->claim_seq == seq0,
            std::to_string(seq0) + " -> " + std::to_string(churn->claim_seq));
        std::string div2;
        row("and_no_divergence_accumulated",
            !g_session_registry.ownership_divergence(&div2),
            div2.empty() ? "still agreeing after 20 swaps" : div2);
    }

    printf("\n%s (%d failing row%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
