// test_handoff_player.cpp — rc2 B6, bridge half: the succession handoff must
// never bind the human player's character.
//
// The handoff (bridge_init.cpp check_controlled_handoff) resolves the heir from
// the dead ruler's title and SWAPS the seat onto it, and its skip-held-heir walk
// swaps onto later heirs. Nothing checked the player. Drives the REAL
// SessionRegistry through the handoff's own steps with heir == player:
//
//   * swap_character(dead -> player) and claim_character(player) are refused
//     with character_is_player and leave the seat untouched (every bind path);
//   * unbind_for_player_heir (what the handoff calls) unbinds the seat with
//     unbound_reason=heir_is_player, which sessions_json and
//     first_unbound_reason (-> get_tick_info) publish;
//   * handoff_subject then returns 0, so the next tick does not retry,
//     including when the seat had already released its corpse;
//   * a later claim clears the reason, and destroying the row (a free-play
//     disconnect) drops it, so a restarted seat reads "";
//   * an observer save (player 0xFFFFFF) has no guard, and the walk's next
//     heir is still bindable.
//
// Exit 0 pass, 1 fail.
#include "bridge.h"
#include "session.h"
#include "skip_held_heir.h"

#include <atomic>
#include <cstdio>
#include <mutex>
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
}  // namespace bridge
using namespace bridge;

namespace {
int failures = 0;
void row(const char* n, bool ok, const std::string& d) {
    std::printf("%-52s %s  %s\n", n, ok ? "PASS" : "FAIL", d.c_str());
    if (!ok) ++failures;
}
std::atomic<uint32_t> g_player{0};
uint32_t player_src() { return g_player.load(); }

uint32_t primary(Session* s) { std::lock_guard<std::mutex> l(s->mutex); return s->primary_character_id; }
bool holds(Session* s, uint32_t c) {
    std::lock_guard<std::mutex> l(s->mutex);
    return s->claimed_character_ids.count(c) != 0;
}
std::string reason(Session* s) { std::lock_guard<std::mutex> l(s->mutex); return s->unbound_reason; }
uint32_t recover(Session* s) {
    std::lock_guard<std::mutex> l(s->mutex);
    return handoff_subject(s->primary_character_id, s->last_steered_character_id,
                           s->last_release_state);
}
}  // namespace

int main() {
    g_session_registry.set_player_character_source(&player_src);
    const uint32_t dead = 5001, player = 6001, next_heir = 6002;
    g_player = player;

    Session* seat = g_session_registry.create("seat-a", Session::Mode::CONTROL);
    std::string why;
    row("seat_binds_the_ruler", g_session_registry.claim_character(seat, dead, &why), why);

    // The handoff's swap, heir == player.
    why.clear();
    const bool swapped = g_session_registry.swap_character(seat, dead, player, &why);
    row("swap_onto_player_refused", !swapped && why == "character_is_player", "reason=" + why);
    row("refused_swap_leaves_the_seat", primary(seat) == dead && holds(seat, dead) &&
                                        !holds(seat, player) &&
                                        g_session_registry.owner_of(player) == nullptr,
        "primary=" + std::to_string(primary(seat)));
    why.clear();
    const bool claimed_player = g_session_registry.claim_character(seat, player, &why);
    row("claim_of_player_refused", !claimed_player && why == "character_is_player",
        "reason=" + why);
    why.clear();
    const bool claimed_high = g_session_registry.claim_character(seat, player | 0x1000000u, &why);
    row("player_id_with_high_bits_refused", !claimed_high && why == "character_is_player",
        "the guard masks to 24 bits like reject_player_actor");

    // What check_controlled_handoff does on heir == player.
    g_session_registry.unbind_for_player_heir(seat, dead, player);
    row("seat_unbound", primary(seat) == 0 && !holds(seat, dead) &&
                        g_session_registry.owner_of(dead) == nullptr,
        "primary=" + std::to_string(primary(seat)));
    row("typed_reason_heir_is_player", reason(seat) == "heir_is_player", reason(seat));
    const std::string sj = g_session_registry.sessions_json();
    row("sessions_json_publishes_it",
        sj.find("\"unbound_reason\":\"heir_is_player\"") != std::string::npos &&
            sj.find("\"unbound_heir_id\":6001") != std::string::npos,
        "get_bridge_state.sessions row; Python side not exercised here");
    row("first_unbound_reason_for_get_tick_info",
        g_session_registry.first_unbound_reason() == "heir_is_player", "hook line");
    row("no_retry_next_tick", recover(seat) == 0, "handoff_subject -> 0");

    // A seat that had ALREADY released its corpse (recovered via Corpse state).
    Session* seat2 = g_session_registry.create("seat-b", Session::Mode::CONTROL);
    g_session_registry.claim_character(seat2, 5002, &why);
    g_session_registry.release_character(seat2, 5002, ReleaseSubjectState::Corpse);
    row("corpse_release_would_recover", recover(seat2) == 5002, "precondition");
    g_session_registry.unbind_for_player_heir(seat2, 5002, player);
    row("recovered_corpse_not_retried", recover(seat2) == 0 &&
                                        reason(seat2) == "heir_is_player",
        "state set to heir_is_player");

    // The agent picks another ruler: the reason clears.
    why.clear();
    row("later_claim_succeeds", g_session_registry.claim_character(seat, 7001, &why), why);
    row("later_claim_clears_reason", reason(seat).empty(), "\"" + reason(seat) + "\"");

    // Free play: a disconnect DESTROYS the session (ipc_server.cpp, pins_claims
    // false), and the reason lives only on the row. A new seat after an MCP
    // restart therefore reads "" even though a handoff to the player happened:
    // the protocol docs must not read "" as "no handoff" (round-5 verifier).
    row("reason_still_on_seat_b", reason(seat2) == "heir_is_player", reason(seat2));
    const std::string sid2 = seat2->session_id;
    g_session_registry.destroy(sid2);
    Session* fresh = g_session_registry.create("seat-b-restarted", Session::Mode::CONTROL);
    const std::string sj2 = g_session_registry.sessions_json();
    row("destroy_drops_the_reason",
        reason(fresh).empty() && g_session_registry.first_unbound_reason().empty() &&
            sj2.find("heir_is_player") == std::string::npos,
        "a restarted seat cannot tell a handoff from a fresh start");

    // The walk's next heir is still bindable (only the player is refused).
    why.clear();
    row("next_heir_swap_allowed", g_session_registry.swap_character(seat, 7001, next_heir, &why),
        why);

    // Observer save: no human, no guard.
    g_player = 0xFFFFFFu;
    Session* obs = g_session_registry.create("seat-c", Session::Mode::CONTROL);
    why.clear();
    row("observer_save_no_guard", g_session_registry.claim_character(obs, player, &why),
        "PLAYER_CHAR_ID 0xFFFFFF");

    std::printf("\n%s (%d failing row%s)\n", failures ? "FAIL" : "PASS", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
