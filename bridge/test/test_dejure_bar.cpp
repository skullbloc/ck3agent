// test_dejure_bar.cpp — create_title's de jure gate is the game's
// rule (dejure_bar.h): a kingdom more than half, an empire more than 80%, a tier with
// no game text 51%. Rows from the live 867 reads (e_france 89/94 of 117; k_france 47,
// k_aquitaine 45 counties) and the boundaries.
// Built twice: plain (every row must pass) and -DDEJURE_BAR_PRE_FIX_CONTROL, the
// superseded rule (held/total < 0.51 from a kingdom up, plus the empire's 80%), which
// exits 0 only when EXACTLY the rows marked pre_fix_differs disagree with it.
// Built and run by ctest; not proven live.
#include <cstdio>
#include "dejure_bar.h"

namespace {
struct Row { const char* what; int tier; long long held, total; bool short_; bool pre_fix_differs; };
const Row kRows[] = {
    // More than half (the game's own "required Counties" lines; k_ireland 14 -> 8)
    {"kingdom 23 of 46: exactly half is not enough",      4, 23, 46, true,  false},
    {"kingdom 24 of 46: more than half",                  4, 24, 46, false, false},
    {"kingdom 24 of 47 (k_france): more than half",       4, 24, 47, false, false},
    {"kingdom 23 of 47: below half",                      4, 23, 47, true,  false},
    {"kingdom 50 of 100: exactly half",                   4, 50, 100, true, false},
    {"kingdom 7 of 14 (k_ireland: the game's 8 required)", 4, 7, 14, true, false},
    {"kingdom 8 of 14 (k_ireland)",                       4, 8, 14, false, false},
    {"kingdom 51 of 101: more than half, under 51%",      4, 51, 101, false, true},
    {"kingdom 26 of 51: more than half, under 51%",       4, 26, 51, false, true},
    {"empire 89 of 117 (e_france, the game refused)",      5, 89, 117, true, false},
    {"empire 94 of 117 (the game's 94 required)",          5, 94, 117, false, false},
    {"empire 80 of 100: not more than 80%",                5, 80, 100, true, false},
    {"empire 81 of 100",                                   5, 81, 100, false, false},
    {"hegemony 50 of 100: no game text, the 51% bar",      6, 50, 100, true, false},
    {"hegemony 51 of 100",                                 6, 51, 100, false, false},
    {"duchy 1 of 4: not this gate (the game's own check)", 3, 1, 4, false, false},
    {"titular kingdom 0 of 0: the caller's own refusal",   4, 0, 0, false, false},
};

#ifdef DEJURE_BAR_PRE_FIX_CONTROL
bool pre_fix_short(int tier, long long held, long long total) {
    if (tier < 4 || total <= 0) return false;
    const bool empire_short = tier == 5 && held * 5 <= total * 4;
    return (static_cast<double>(held) / static_cast<double>(total)) < 0.51 || empire_short;
}
#endif
}  // namespace

int main() {
    int bad = 0;
    for (const Row& r : kRows) {
        const bool got = bridge::dejure_bar::short_of(r.tier, r.held, r.total);
#ifdef DEJURE_BAR_PRE_FIX_CONTROL
        const bool differs = pre_fix_short(r.tier, r.held, r.total) != r.short_;
        if (differs != r.pre_fix_differs) {
            std::printf("CONTROL UNEXPECTED %s: pre-fix differs=%d, expected %d\n", r.what, differs, r.pre_fix_differs);
            ++bad;
        }
        (void)got;
#else
        if (got != r.short_) {
            std::printf("FAIL %s: short_of=%d, expected %d\n", r.what, got, r.short_);
            ++bad;
        }
#endif
    }
    static_assert(bridge::dejure_bar::required_pct(4) == 50.0, "kingdom: half");
    static_assert(bridge::dejure_bar::required_pct(5) == 80.0, "empire: 80%");
    std::printf("%s: %d bad of %zu rows\n", bad ? "FAILED" : "ok", bad, sizeof(kRows) / sizeof(kRows[0]));
    return bad ? 1 : 0;
}
