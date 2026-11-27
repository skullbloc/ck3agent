// test_lifestyle_points.cpp — unspent perk points and the prerequisites check
// (bridge/include/lifestyle_points.h), engine-free.
//
// Values recorded from the 1097 dump (re_lifestyle_perks.md §3, eight rulers): focus
// stewardship_duty_focus, XP entries stewardship 871.0, diplomacy 100.0, learning 150.0 (fixed
// point 87100000 etc.), 1000 XP a point for all six lifestyles -> 0 points available for every one.
#include <cstdio>
#include <string_view>
#include "lifestyle_points.h"

namespace lp = bridge::lifestyle_points;

static int g_fail = 0, g_rows = 0;
#define ROW(name, cond)                                                     \
    do {                                                                    \
        ++g_rows;                                                           \
        if (!(cond)) { ++g_fail; std::printf("FAIL %s\n", name); }          \
        else std::printf("ok   %s\n", name);                                \
    } while (0)

int main() {
    // 1. points, as FUN_067f44e0 counts them
    ROW("1a dump: stewardship 871.0 at 1000 a point is 0 points", lp::points(87100000LL, 1000) == 0);
    ROW("1b dump: diplomacy 100.0 and learning 150.0 are 0", lp::points(10000000LL, 1000) == 0 && lp::points(15000000LL, 1000) == 0);
    ROW("1c 1000.0 is one point, 1999.99 still one, 2000.0 two",
        lp::points(100000000LL, 1000) == 1 && lp::points(199999999LL, 1000) == 1 && lp::points(200000000LL, 1000) == 2);
    ROW("1d the fraction is dropped before the division (999.99999 is 0)", lp::points(99999999LL, 1000) == 0);
    ROW("1e an unread per-point value gives 0, never a division", lp::points(500000000LL, 0) == 0 && lp::points(1, -3) == 0);
    ROW("1f negative XP is no point", lp::points(-100000000LL, 1000) <= 0);
    ROW("1g to the next point: 871.0 needs 129", lp::xp_to_next(87100000LL, 1000) == 129);
    ROW("1h to the next point: 1000.0 needs 1000; unread is -1", lp::xp_to_next(100000000LL, 1000) == 1000 &&
                                                                 lp::xp_to_next(1, 0) == -1);

    ROW("1i the per-point value is the lifestyle's own (SLifestyle+0x130; 1000 for all six in the dump, 500 here)",
        lp::points(100000000LL, 500) == 2 && lp::xp_to_next(110000000LL, 500) == 400);

    // 2. prerequisites (FUN_06b81250: held refuses, then EVERY parent)
    const uint64_t THOUGHTFUL = 0xA000, DUCAL = 0xA100, OTHER = 0xB000, TWO = 0xA200, P2 = 0xA300;
    const uint64_t none[1] = {0};
    const uint64_t ducal_parents[1] = {THOUGHTFUL};
    const uint64_t two_parents[2] = {DUCAL, P2};
    const uint64_t held0[1] = {OTHER};
    const uint64_t held1[2] = {OTHER, THOUGHTFUL};
    const uint64_t held2[3] = {OTHER, THOUGHTFUL, DUCAL};
    ROW("2a the root perk (no parent) is admitted when not held", lp::by_parents(THOUGHTFUL, none, 0, held0, 1) == lp::Pick::kByParents);
    ROW("2b ducal_conquest without thoughtful: parent missing", lp::by_parents(DUCAL, ducal_parents, 1, held0, 1) == lp::Pick::kParentMissing);
    ROW("2c ducal_conquest with thoughtful: admitted", lp::by_parents(DUCAL, ducal_parents, 1, held1, 2) == lp::Pick::kByParents);
    ROW("2d a held perk is held (before its parents are looked at)", lp::by_parents(DUCAL, ducal_parents, 1, held2, 3) == lp::Pick::kHeld);
    ROW("2e two parents: one held is not enough", lp::by_parents(TWO, two_parents, 2, held2, 3) == lp::Pick::kParentMissing);
    ROW("2f names", std::string_view(lp::pick_name(lp::Pick::kByParents)) == "parents_held");

    // 3. the verdict order of CanExecute
    ROW("3a no focus first", std::string_view(lp::refusal(false, 0, lp::Pick::kHeld)) == "no_lifestyle_focus");
    ROW("3b then the point", std::string_view(lp::refusal(true, 0, lp::Pick::kByParents)) == "perk_points_needed");
    ROW("3c then held / parent", std::string_view(lp::refusal(true, 1, lp::Pick::kHeld)) == "perk_already_held" &&
                                 std::string_view(lp::refusal(true, 2, lp::Pick::kParentMissing)) == "parent_perk_missing");
    ROW("3d admitted by the reads", lp::refusal(true, 1, lp::Pick::kByParents) == nullptr);

    std::printf("%d rows, %d failed\n", g_rows, g_fail);
    return g_fail ? 1 : 0;
}
