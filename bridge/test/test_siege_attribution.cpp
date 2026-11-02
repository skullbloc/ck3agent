// test_siege_attribution.cpp — a siege is listed under the war it belongs to
// (lane l5, F8; bridge/include/siege_attribution.h).
//
// THE DEFECT (live867 l5fx5-live-1). query_wars listed every siege whose
// besieger was among a war's attackers, so 14859's sieges 6 and 7 appeared
// under his three wars (3, 4 and the new 16777218), each with that war's
// primary defender written in as the siege's defender. The rule now needs the
// besieged holding's realm on the other side of the war.
#include "siege_attribution.h"

#include <cstdio>
#include <cstdlib>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); g_fail++; } } while (0)

using siege_attr::Side;
using siege_attr::war_side_of_siege;

static void test_one_besieger_three_wars() {
    // 14859 attacks in three wars; the besieged holding is held by 11212's vassal
    // 14538 (liege 11212), the defender of the third war only.
    const uint32_t chain[] = {14538, 11212};
    const uint32_t w3_atk[] = {14859}, w3_def[] = {13903};
    const uint32_t w4_atk[] = {14859}, w4_def[] = {12037};
    const uint32_t wn_atk[] = {14859}, wn_def[] = {11212};
    CHECK(war_side_of_siege(14859, chain, 2, w3_atk, 1, w3_def, 1) == Side::None,
          "a siege of 11212's land is not war 3's (13903)");
    CHECK(war_side_of_siege(14859, chain, 2, w4_atk, 1, w4_def, 1) == Side::None,
          "a siege of 11212's land is not war 4's (12037)");
    CHECK(war_side_of_siege(14859, chain, 2, wn_atk, 1, wn_def, 1) == Side::Attacker,
          "a siege of 11212's land is the war against 11212");
}

static void test_defender_besieges_attacker_land() {
    // A defender's army besieging an attacker's holding is the war's too (the
    // old rule looked at attackers only: 9 of 64 sieges in the 1097 dump).
    const uint32_t chain[] = {500, 400};
    const uint32_t atk[] = {400, 401}, def[] = {77};
    CHECK(war_side_of_siege(77, chain, 2, atk, 2, def, 1) == Side::Defender,
          "a defender besieging the attacker's vassal");
}

static void test_recapture_and_unread_holding_are_no_war() {
    const uint32_t atk[] = {10}, def[] = {20};
    const uint32_t own[] = {11, 10};      // the besieger's own side's holding
    CHECK(war_side_of_siege(10, own, 2, atk, 1, def, 1) == Side::None,
          "retaking an own-side holding: the occupier is not read, no war");
    CHECK(war_side_of_siege(10, nullptr, 0, atk, 1, def, 1) == Side::None,
          "an unread holding attributes the siege to no war");
    const uint32_t enemy[] = {20};
    CHECK(war_side_of_siege(0, enemy, 1, atk, 1, def, 1) == Side::None,
          "no besieger, no war");
    CHECK(war_side_of_siege(99, enemy, 1, atk, 1, def, 1) == Side::None,
          "a besieger outside the war");
}

static void test_top_liege_on_the_other_side() {
    // The defender is the realm's top liege; the holding is a baron's three
    // levels down: the chain reaches him.
    const uint32_t chain[] = {7, 6, 5, 4};
    const uint32_t atk[] = {1}, def[] = {4};
    CHECK(war_side_of_siege(1, chain, 4, atk, 1, def, 1) == Side::Attacker, "top liege defends");
}

int main() {
    test_one_besieger_three_wars();
    test_defender_besieges_attacker_land();
    test_recapture_and_unread_holding_are_no_war();
    test_top_liege_on_the_other_side();
    if (g_fail) { std::fprintf(stderr, "%d failure(s)\n", g_fail); return 1; }
    std::printf("test_siege_attribution: OK\n");
    return 0;
}
