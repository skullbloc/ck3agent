// test_siege_view.cpp — a siege's need, slowdown, blocker and days left (siege_view.h).
//
// The rows are sieges of the 1097 dump read by a probe
// (fort level, garrison, the men able to besiege, siege tier, CSiege+0x18/+0x20/+0x110/+0x118/
// +0x17c) beside what the superseded bridge read for the same siege at +0x1b0/+0x1b8 (the NEXT
// pooled siege's phase length and next action), plus the 867 dump's day-0 siege and one
// synthetic marching row (marked).
//
// SIEGE_VIEW_PRE_FIX_CONTROL builds the superseded verdict (query_handlers.cpp before the siege fix:
// progress = +0x1b0 / 100000, "won" at 19.5 and above, "ongoing" when +0x1b8 == 5 and above
// 0.05, else "stalled") and compares its bucket with the decompiled rule's. It exits 0 only
// when EXACTLY the rows marked `pre_fix_disagrees` disagree: a control that redded on any
// change could not tell this fix from a rewrite.
#include "siege_view.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using siege_view::Reads;
using siege_view::Verdict;

namespace {

int g_fail = 0;
[[maybe_unused]] void check(bool ok, const char* what) {
    if (!ok) { std::printf("FAIL %s\n", what); ++g_fail; }
}

struct Row {
    const char* name;
    Reads r;
    double old_1b0;          // the superseded "progress"
    int    old_1b8;          // the superseded "state"
    const char* status;      // expected
    const char* blocker;     // expected (nullptr = none)
    int    eta_days;         // expected (-1 = none)
    bool   pre_fix_disagrees;
};

Reads R(bool ticked, bool can, double prog, int fort, int gar, int men, int tier, int breach, double phase,
        int day, int battle = 0, int moving = 0) {
    Reads r;
    r.ticked = ticked; r.can_progress = can; r.progress = prog; r.fort_level = fort; r.garrison = gar;
    r.men = men; r.siege_tier = tier; r.breach = breach; r.phase_days = phase; r.day_in_phase = day;
    r.men_in_battle = battle; r.men_moving = moving;
    return r;
}

const Row kRows[] = {
    // 0x4f000015: fort 4, no siege weapons: 13.7 of 400 at 0.714/day; the old read said "won" (20)
    {"4f000015", R(true, true, 13.68, 4, 113, 554, 0, 0, 20, 11), 20, 5, "progressing", nullptr, 542, true},
    // 0x40000019: 775 men against a garrison of 1000 at fort 7: no progress; the old read said "won"
    {"40000019", R(true, false, 0.0, 7, 1000, 775, 0, 0, 20, 0), 20, 5, "blocked", "garrison_larger", -1, true},
    // 0x4e000005: fort 2, 208 of 250, 41 days left; the old read said "won"
    {"4e000005", R(true, true, 208.41, 2, 25, 512, 0, 0, 20, 5), 20, 5, "progressing", nullptr, 41, true},
    // 0x49000003: fort 3, mangonels; the old read happened to say "ongoing" (its neighbour's 14)
    {"49000003", R(true, true, 73.19, 3, 500, 576, 2, 1, 18, 13), 14, 5, "progressing", nullptr, 252, false},
    // 0x3e000009: the besiegers are in a battle; the old read said "stalled" (no neighbour)
    {"3e000009", R(true, false, 316.88, 6, 900, 0, 0, 2, 14, 9, 3296), 0, 0, "blocked", "in_battle", -1, false},
    // 0x51000000: fort 2, 196 of 250 and moving; the old read said "stalled" (no neighbour)
    {"51000000", R(true, true, 195.87, 2, 25, 316, 0, 0, 20, 10), 0, 0, "progressing", nullptr, 54, true},
    // 0x4a000026: allies' men count (3108 on the holding against 532)
    {"4a000026", R(true, true, 187.90, 5, 532, 3108, 2, 2, 12, 6), 18, 5, "progressing", nullptr, 257, false},
    // 0x3b00004a: fort 5, 461 of 475; the old read said "stalled" (its neighbour's action 3)
    {"3b00004a", R(true, true, 461.27, 5, 650, 1542, 0, 0, 20, 1), 18, 3, "progressing", nullptr, 19, true},
    // the 867 dump's siege 0, opened that day: not yet evaluated (phase 0)
    {"867-0", R(false, false, 0.0, 3, 532, 2840, 1, 0, 0, 0), 0, 0, "starting", nullptr, -1, true},
    // synthetic: the besiegers are marching on
    {"synthetic-moving", R(true, false, 50.0, 3, 300, 0, 0, 0, 20, 3, 0, 900), 20, 5, "blocked", "moving", -1, true},
    // synthetic: nobody stands on the holding (a held army elsewhere)
    {"synthetic-nobody", R(true, false, 50.0, 3, 300, 0, 0, 0, 20, 3), 20, 5, "blocked", "no_besieger", -1, true},
};

// The superseded verdict's bucket: done / moving / stuck.
[[maybe_unused]] const char* pre_fix_bucket(double p1b0, int st) {
    double progress = p1b0;
    if (progress >= 20.0 - 0.5) return "done";
    if (st == 5 && progress > 0.05) return "moving";
    return "stuck";
}

[[maybe_unused]] const char* bucket(const Verdict& v) {
    if (std::strcmp(v.status, "blocked") == 0) return "stuck";
    return "moving";   // progressing or starting: a live siege is never fallen (a fallen one leaves the DB)
}

}  // namespace

int main() {
#ifdef SIEGE_VIEW_PRE_FIX_CONTROL
    int marked = 0, disagreed = 0, wrong = 0;
    for (const Row& row : kRows) {
        const bool dis = std::strcmp(pre_fix_bucket(row.old_1b0, row.old_1b8), bucket(siege_view::judge(row.r))) != 0;
        marked += row.pre_fix_disagrees;
        disagreed += dis;
        if (dis != row.pre_fix_disagrees) { std::printf("CONTROL row %s: disagrees=%d marked=%d\n", row.name, dis, row.pre_fix_disagrees); ++wrong; }
    }
    std::printf("pre-fix control: %d of %zu rows disagree with the decompiled rule, %d marked, %d off the marks\n",
                disagreed, sizeof(kRows) / sizeof(kRows[0]), marked, wrong);
    return (wrong == 0 && disagreed == marked && marked > 0) ? 0 : 1;
#else
    // The defines' rule (FUN_064363f0, FUN_06439b30)
    check(siege_view::needed_max(4) == 400.0, "needed_max(4) = 100 + 75 x 4");
    check(siege_view::needed_max(0) == 100.0 && siege_view::needed_max(-1) == 100.0, "needed_max floor");
    check(siege_view::tier_needed(3) == 0 && siege_view::tier_needed(4) == 1 && siege_view::tier_needed(6) == 2
              && siege_view::tier_needed(11) == 3 && siege_view::tier_needed(31) == 5, "tier_needed thresholds 4/6/11/16/31");
    check(std::fabs(siege_view::fort_speed(6, 0) - 0.49) < 1e-9, "fort 6, no siege weapons: 0.7 x 0.7");
    check(std::fabs(siege_view::fort_speed(6, 1) - 0.7) < 1e-9, "fort 6, tier 1: 0.7");
    check(siege_view::fort_speed(7, 2) == 1.0 && siege_view::fort_speed(3, 0) == 1.0, "no slowdown");
    check(siege_view::daily_at_base(100, 900, 2, 0) == 1.0, "fewer men than the garrison: base only");
    check(std::fabs(siege_view::daily_at_base(1300, 900, 2, 0) - 1.02) < 1e-9, "400 men over: 2 stacks of 200");
    check(siege_view::daily_at_base(300, 0, 31, 0) == siege_view::kMinDaily, "the floor");
    for (const Row& row : kRows) {
        const Verdict v = siege_view::judge(row.r);
        char what[160];
        std::snprintf(what, sizeof what, "row %s status %s (want %s)", row.name, v.status, row.status);
        check(std::strcmp(v.status, row.status) == 0, what);
        std::snprintf(what, sizeof what, "row %s blocker %s (want %s)", row.name, v.blocker ? v.blocker : "-",
                      row.blocker ? row.blocker : "-");
        check((v.blocker == nullptr && row.blocker == nullptr)
                  || (v.blocker && row.blocker && std::strcmp(v.blocker, row.blocker) == 0), what);
        std::snprintf(what, sizeof what, "row %s eta %d (want %d)", row.name, v.eta_days, row.eta_days);
        check(v.eta_days == row.eta_days, what);
        check(v.pct >= 0 && v.pct <= 100, "pct within 0..100 when the fort level is read");
        check(row.r.progress <= v.needed_max, "a live siege's progress never above its need (dumps 65/65)");
    }
    // A siege whose fort level was not read has no need, no pct and no eta.
    Reads unread = R(true, true, 10, -1, 100, 500, 0, 0, 20, 1);
    const Verdict u = siege_view::judge(unread);
    check(u.needed_max < 0 && u.pct < 0 && u.eta_days == -1, "fort level unread: no need, pct or eta");
    std::printf("test_siege_view: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
#endif
}
