// ck-handoff-swap — the SUCCESSION handoff at a full roster.
//
// check_controlled_handoff() used to claim the heir and release the corpse
// afterwards, which asks for a slot while holding the only one available. At
// 10/10 that is unsatisfiable by construction, so the seat stayed pointed at a
// corpse and the function re-walked up to 64 candidates every tick — 2,574
// watchlist_full lines in diag25, none of which said anything the first had not.
//
// These rows are at the REGISTRY, not in a log: ck-zgh4 established that a
// refusal count read from a sealed bundle returns 0 both when the fix works and
// when the evidence has rotated away, so the grading cannot live in a log.
#include "bridge.h"
#include "session.h"
#include "skip_held_heir.h"
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
static uint64_t claim_seq_of(Session* s) {
    std::lock_guard<std::mutex> lk(s->mutex);
    return s->claim_seq;
}

int main() {
    // ── THE ROSTER AT THE CAP. Every row below is about the cap-full case, and
    // a probe that does not fill it never runs the code it grades.
    std::vector<Session*> seats;
    for (int i = 0; i < 6; ++i) {
        char sid[32]; snprintf(sid, sizeof sid, "seat-%d", i);
        Session* s = g_session_registry.create(sid, Session::Mode::CONTROL);
        seats.push_back(s);
        std::string why;
        g_session_registry.claim_character(s, 1000 + i, &why);
    }
    for (int k = 0; k < 4; ++k) {
        std::string why;
        g_session_registry.claim_character(seats[0], 2000 + k, &why);
    }
    const size_t occupancy = g_session_registry.controlled_claim_count();
    row("the_roster_is_AT_the_cap", occupancy == MAX_CONTROLLED_WATCHLIST,
        std::to_string(occupancy) + "/" + std::to_string(MAX_CONTROLLED_WATCHLIST));

    // The seat whose ruler is about to die. seat-1 holds 1001; the heir is 7777,
    // which nobody holds — the ordinary succession shape.
    Session* seat = seats[1];
    const uint32_t corpse = 1001, heir = 7777;
    const uint64_t seq_before = claim_seq_of(seat);

    // ── WHAT THE OLD HANDOFF DID FIRST, and why it could never work here.
    {
        std::string why;
        const bool ok = g_session_registry.claim_character(seat, heir, &why);
        row("the_OLD_first_move_is_refused_at_the_cap",
            !ok && why == "watchlist_full",
            "claim(heir) while still holding the corpse: " + why
            + " — the slot it needs is the one it is sitting on");
    }

    // ── WHAT THE NEW HANDOFF DOES, in the same state.
    {
        std::string why;
        const bool ok = g_session_registry.swap_character(seat, corpse, heir, &why);
        row("the_NEW_first_move_SUCCEEDS_at_the_cap", ok,
            ok ? "swap(corpse -> heir) is net zero, so the cap has nothing to refuse"
               : ("refused: " + why));
        row("the_seat_now_steers_the_HEIR", primary(seat) == heir,
            std::to_string(primary(seat)));
        row("the_seat_holds_the_SAME_NUMBER_of_slots", held(seat) == 1,
            std::to_string(held(seat)));
        row("occupancy_is_unchanged",
            g_session_registry.controlled_claim_count() == MAX_CONTROLLED_WATCHLIST,
            std::to_string(g_session_registry.controlled_claim_count()));
        row("claim_seq_is_UNCHANGED", claim_seq_of(seat) == seq_before,
            std::to_string(seq_before) + " -> " + std::to_string(claim_seq_of(seat))
            + "  (a move is what SS7.3i rejects)");
    }

    // ── THE DELETED LINE WAS A NO-OP, MEASURED RATHER THAN ASSERTED.
    // check_controlled_handoff used to call release_character(ctrl) after the
    // claim. The swap already took the corpse out in the same locked step, so
    // that call can only return false now. Showing it returns false is what
    // makes deleting it safe; leaving it would have read like the mechanism that
    // frees the slot, and the ORDER is exactly what was wrong.
    row("the_removed_release_could_only_have_been_a_NO_OP",
        !g_session_registry.release_character(seat, corpse),
        "the corpse left with the swap; release(corpse) is already false");

    // ── ck-7rk6's CASE IS PRESERVED, NOT SWALLOWED. The walk exists for a
    // COLLISION: the game's heir is already held by another session. If the swap
    // quietly succeeded there it would seize another seat's character, and the
    // walk would become unreachable — the fix eating the case it must leave
    // alone. seat-2 holds 1002; seat-1 tries to inherit it.
    {
        std::string why;
        const bool ok = g_session_registry.swap_character(seat, heir, 1002, &why);
        row("a_COLLISION_still_refuses_so_the_walk_is_REACHED",
            !ok && why == "owned_by_other",
            "swap onto another seat's character: " + why
            + " — watchlist_full is fixed, owned_by_other is untouched");
        row("and_the_collision_left_the_seat_where_it_was",
            primary(seat) == heir && held(seat) == 1,
            "primary " + std::to_string(primary(seat)) + ", held "
            + std::to_string(held(seat)));
    }

    // ── THE TWO REFUSALS ARE DISTINGUISHABLE IN THE WITNESS, which is the whole
    // point of giving record_skip_held_heir_exhausted its reason: at a full
    // roster every refusal was watchlist_full, and the message asserted an
    // exhausted succession order.
    {
        HandoffBackoff bo;
        const bool first = handoff_note_failure(bo, corpse, heir, "watchlist_full");
        const bool same  = handoff_note_failure(bo, corpse, heir, "watchlist_full");
        const bool moved = handoff_note_failure(bo, corpse, heir, "owned_by_other");
        row("the_witness_reports_once_per_STATE_not_per_tick",
            first && !same && moved,
            "new state logs, repeat is silent, a different reason logs again");
    }

    printf("\n%s (%d failing row%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
