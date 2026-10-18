// acceptance_exclusion_fraction.cpp — THE ck-2ngg ACCEPTANCE, product lane.
// Host only: no CK3, no game, no engine.
//
// THE CLAIM: the unavailability exclusion threshold X is a RULE — 5% of the
// episode's DECLARED horizon, computed per episode — and not the constant 365,
// which is that rule's answer at the registered 20-game-year horizon and at no
// other.
//
// Driven by scripts/acceptance_exclusion_fraction.py, which also runs the
// NEGATIVE CONTROL: the same four lanes over the REAL pre-ck-2ngg bytes pulled
// out of git history (scripts/fixtures/exclusion_rule/pre_2ngg/), which must
// FAIL lanes B and D — it judges the smoke horizon at 365 and passes an episode
// the rule excludes at 78.
//
// LANES
//   A  the REGISTERED horizon      -> X = 365          (the paper's number, unmoved)
//   B  the SMOKE horizon           -> X = 78           (where the constant was 4.7x out)
//   C  no declared horizon         -> UNARMED, typed   (never 365)
//   D  an episode the rule EXCLUDES and the constant would have PASSED
//
// Each lane prints `LANE <id> <PASS|FAIL> <detail>` so the driver can assert on
// the failing SET rather than on a count — a control that drifts into failing
// something else must not read as a control doing its job (ck-ijfa L1).

#include "unavailability.h"
#include "episode_policy.h"
#ifndef CK3_EXCLUSION_PREFIX_CONTROL
#include "exclusion_rule.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace bridge;

static int g_fail = 0;
static int g_lanes = 0;

static void lane(const char* id, bool ok, const std::string& detail) {
    ++g_lanes;
    if (!ok) ++g_fail;
    printf("LANE %s %s %s\n", id, ok ? "PASS" : "FAIL", detail.c_str());
}

// The episode every lane models: 1066.09.15, twelve months of nothing, a gap of
// `unsteered` game-days that is 400 WALL-seconds long (over the 360 s anomaly
// gate, so it is genuinely anomalous under the shipped ck-ijfa rule).
static const int64_t D0 = 1066 * 365 + 243 + 14;
static const uint64_t W0 = 1'000'000;

static bool episode_with_gap_is_invalid(int64_t x_days, bool armed,
                                        int64_t unsteered) {
    UnavailabilityLedger& l = unavailability();
    l.reset();
#ifdef CK3_EXCLUSION_PREFIX_CONTROL
    (void)armed;
    l.configure(360, x_days);
#else
    l.configure(360, x_days, armed);
#endif
    l.note_session("english-earl", D0, W0);
    l.note_activity("english-earl", D0, W0);
    l.note_day(D0 + unsteered, W0 + 400'000);
    l.note_activity("english-earl", D0 + unsteered, W0 + 400'000);
    return l.episode_invalid();
}

// Resolve X the way the BRIDGE does, from the environment the harness sets.
// Under the control this is the pre-ck-2ngg path, which reads no horizon at all
// and keeps its stored constant — which is precisely what is being measured.
static void resolve_from_env(const char* start, const char* end) {
    setenv("CK3_EPISODE_MODE", "experiment", 1);
    if (start) setenv("CK3_EPISODE_HORIZON_START", start, 1);
    else       unsetenv("CK3_EPISODE_HORIZON_START");
    if (end)   setenv("CK3_EPISODE_HORIZON_END", end, 1);
    else       unsetenv("CK3_EPISODE_HORIZON_END");
    unsetenv("CK3_UNAVAIL_X_DAYS");
    unsetenv("CK3_UNAVAIL_EXCLUSION_FRACTION");
    episode_policy().init_from_env();
}

int main() {
#ifdef CK3_EXCLUSION_PREFIX_CONTROL
    printf("=== ck-2ngg NEGATIVE CONTROL: the PRE-FIX constant module, real "
           "bytes from git history ===\n");
#else
    printf("=== ck-2ngg acceptance: X = 5%% of the DECLARED horizon, computed "
           "per episode ===\n");
#endif

    // ── A. THE REGISTERED HORIZON: the number the paper cites does not move ──
    resolve_from_env("1066.09.15", "1086.09.15");
    EpisodePolicy p = episode_policy().get();
    lane("A", p.unavail_exclusion_days == 365,
         "registered horizon 1066.09.15 -> 1086.09.15 (7300 game-days) x 0.05 "
         "-> X=" + std::to_string((long long)p.unavail_exclusion_days) +
         ", want 365");

    // ── B. THE SMOKE HORIZON: where the constant was 4.7x too permissive ────
    // 1066.09.15 -> 1071.01.01 is 1568 game-days on the ENGINE's calendar
    // (ck3::game_date, ck-05mg's real month lengths). 1568 x 0.05 = 78.4, and
    // the rounding rule is round-half-away-from-zero, so X = 78.
    resolve_from_env("1066.09.15", "1071.01.01");
    p = episode_policy().get();
    lane("B", p.unavail_exclusion_days == 78,
         "smoke horizon 1066.09.15 -> 1071.01.01 (1568 game-days) x 0.05 -> X=" +
         std::to_string((long long)p.unavail_exclusion_days) + ", want 78 "
         "(the pre-fix constant answers 365 here, 4.7x too permissive)");

    // ── C. NO HORIZON: UNARMED and TYPED, never 365 ─────────────────────────
    resolve_from_env(nullptr, nullptr);
    p = episode_policy().get();
#ifdef CK3_EXCLUSION_PREFIX_CONTROL
    const bool armed_ok = false;   // the pre-fix policy has no such concept
    const std::string armed_detail =
        "the pre-fix module has NO unarmed state: with no horizon it keeps X=" +
        std::to_string((long long)p.unavail_exclusion_days);
#else
    const bool armed_ok =
        !p.unavail_exclusion_armed &&
        p.unavail_exclusion_unarmed_reason == std::string(kUnarmedNoHorizon) &&
        p.unavail_exclusion_days == 0;
    const std::string armed_detail =
        "no declared horizon -> armed=" +
        std::string(p.unavail_exclusion_armed ? "true" : "false") +
        " reason='" + p.unavail_exclusion_unarmed_reason + "' X=" +
        std::to_string((long long)p.unavail_exclusion_days);
#endif
    lane("C", armed_ok, armed_detail);

    // ── D. THE DISCRIMINATOR, on ONE episode ────────────────────────────────
    // A smoke-horizon episode with an English session unsteered for 120
    // game-days. Under the RULE (X=78) it is EXCLUDED. Under the pre-fix
    // constant (X=365) it PASSES. Same history, same anomaly gate, opposite
    // verdicts — and the whole difference is whether X was computed for this
    // episode's own horizon. This is the lane the bead named.
    resolve_from_env("1066.09.15", "1071.01.01");
    p = episode_policy().get();
    const bool excluded = episode_with_gap_is_invalid(
        p.unavail_exclusion_days,
#ifdef CK3_EXCLUSION_PREFIX_CONTROL
        true,
#else
        p.unavail_exclusion_armed,
#endif
        /*unsteered*/120);
    lane("D", excluded,
         "120 unsteered game-days on the smoke horizon, judged against X=" +
         std::to_string((long long)p.unavail_exclusion_days) + " -> " +
         (excluded ? "EXCLUDED (correct: 120 > 78)"
                   : "PASSED (the defect: 120 < 365, so a session unsteered for "
                     "7.7% of the whole horizon is waved through)"));

    printf("\n%d lanes, %d failures\n", g_lanes, g_fail);
    printf(g_fail == 0 ? "PASS\n" : "FAIL\n");
    return g_fail == 0 ? 0 : 1;
}
