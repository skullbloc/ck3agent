// lane-multi — several rulers in ONE session: `claim_character(..., keep_primary)`.
//
// Drives the REAL SessionRegistry in-process. The rows that matter: an added
// claim keeps the session's primary, counts against the cap like any claim,
// is refused for another session's character, and releasing the primary hands
// the primary to a ruler the session still holds.
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
static bool holds(Session* s, uint32_t c) {
    std::lock_guard<std::mutex> lk(s->mutex);
    return s->claimed_character_ids.count(c) == 1;
}
static uint32_t primary(Session* s) {
    std::lock_guard<std::mutex> lk(s->mutex);
    return s->primary_character_id;
}

int main() {
    Session* a = g_session_registry.create("ck3agent", Session::Mode::CONTROL);
    std::string why;

    // ── An add with NOTHING held is a first claim: it becomes the primary.
    const bool first = g_session_registry.claim_character(a, 13902, &why, true);
    row("an_add_with_nothing_held_becomes_the_primary",
        first && primary(a) == 13902,
        "primary " + std::to_string(primary(a)) + (why.empty() ? "" : " " + why));

    // ── THE ROW THIS LANE EXISTS FOR: a second, added claim keeps the primary.
    why.clear();
    const bool added = g_session_registry.claim_character(a, 12145, &why, true);
    row("an_add_keeps_the_primary",
        added && primary(a) == 13902, "primary " + std::to_string(primary(a)));
    row("and_the_session_holds_BOTH",
        held(a) == 2 && holds(a, 13902) && holds(a, 12145),
        std::to_string(held(a)) + " held");
    row("the_global_primary_is_still_the_first",
        g_controlled_primary.load() == 13902, std::to_string(g_controlled_primary.load()));
    std::string div;
    row("the_add_leaves_NO_ownership_divergence",
        !g_session_registry.ownership_divergence(&div),
        div.empty() ? "watchlist and ownership agree" : div);

    // ── Re-adding a held ruler is the fast path and still does not move the primary.
    why.clear();
    row("re_adding_a_held_ruler_does_not_move_the_primary",
        g_session_registry.claim_character(a, 12145, &why, true) && primary(a) == 13902
            && held(a) == 2,
        "primary " + std::to_string(primary(a)));

    // ── Without keep_primary, the historical rule: the claimed id becomes primary.
    Session* b = g_session_registry.create("other", Session::Mode::CONTROL);
    g_session_registry.claim_character(b, 500, &why);
    g_session_registry.claim_character(b, 501, &why);
    row("a_plain_claim_still_moves_the_primary",
        primary(b) == 501, "primary " + std::to_string(primary(b)));

    // ── An add cannot take another session's ruler.
    why.clear();
    row("an_add_of_another_sessions_ruler_is_refused",
        !g_session_registry.claim_character(a, 500, &why, true) && why == "owned_by_other",
        why);

    // ── The cap counts added claims: fill to 10, the next add is refused.
    for (uint32_t k = 0; held(a) + held(b) < MAX_CONTROLLED_WATCHLIST; ++k) {
        std::string w;
        if (!g_session_registry.claim_character(a, 20000 + k, &w, true)) break;
    }
    why.clear();
    const bool over = g_session_registry.claim_character(a, 29999, &why, true);
    row("an_add_at_the_cap_is_refused_watchlist_full",
        !over && why == "watchlist_full",
        std::to_string(g_session_registry.controlled_claim_count()) + "/"
            + std::to_string(MAX_CONTROLLED_WATCHLIST) + " " + why);
    row("and_the_primary_survived_the_whole_fill",
        primary(a) == 13902, "primary " + std::to_string(primary(a)));

    // ── Releasing a NON-primary added ruler leaves the primary.
    row("releasing_an_added_ruler_keeps_the_primary",
        g_session_registry.release_character(a, 12145) && primary(a) == 13902
            && !holds(a, 12145),
        "primary " + std::to_string(primary(a)));

    // ── Releasing the primary hands it to a ruler the session still holds.
    const bool rel = g_session_registry.release_character(a, 13902);
    const uint32_t p = primary(a);
    row("releasing_the_primary_promotes_a_held_ruler",
        rel && p != 0 && holds(a, p), "primary " + std::to_string(p));

    // ── An observe session can add nothing.
    Session* obs = g_session_registry.create("watcher", Session::Mode::OBSERVE);
    why.clear();
    row("an_observe_session_cannot_add",
        !g_session_registry.claim_character(obs, 31000, &why, true) && why == "observe_mode",
        why);

    printf("\n%s (%d failing row%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
