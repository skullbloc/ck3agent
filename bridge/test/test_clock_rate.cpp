// test_clock_rate.cpp — ck-ijfa. The meter that converts the covariate's unit
// into the unit every question about it is asked in.
//
// NO GAME. Wall time is passed IN, so a year of episode runs in a microsecond.
//
// THE CLAIM: the rate is MEASURED, never assumed; it refuses to answer from a
// window too short to mean anything; it re-anchors rather than averaging across
// a world reload; and it carries its CONDITION, because a rate without one is
// what filed this bead.

#include "clock_rate.h"
#include "tick_bookkeeping.h"

#include <cmath>
#include <cstdio>
#include <string>

using namespace bridge;

static int g_checks = 0;
static int g_fails  = 0;

static void check(bool ok, const char* what, const std::string& detail = "") {
    g_checks++;
    if (!ok) g_fails++;
    printf("  %-4s %s%s%s\n", ok ? "ok" : "FAIL", what,
           detail.empty() ? "" : " — ", detail.c_str());
}

static void check_near(double actual, double expected, double tol,
                       const char* what) {
    const bool ok = std::fabs(actual - expected) <= tol;
    char d[128];
    snprintf(d, sizeof(d), "expected %.6f +/- %.6f, got %.6f", expected, tol, actual);
    check(ok, what, ok ? "" : d);
}

int main() {
    printf("test_clock_rate (ck-ijfa): the measured game-days-per-wall-second, "
           "with its condition\n");

    // -----------------------------------------------------------------------
    printf("\n1. NOT MEASURED IS A REAL ANSWER\n");
    {
        ClockRateMeter m;
        check(!m.get().measured, "a meter that has seen nothing is NOT measured");
        check(m.get().game_days_per_wall_second == 0.0,
              "and its rate is 0.0 — which a consumer must publish as null, never "
              "as a number (the whole defect was a rate borrowed from elsewhere)");

        // 30 wall-seconds is below kMinWallSeconds: one conductor iteration's
        // jitter is a large fraction of that denominator.
        m.note(bridge::tick_bookkeeping(true), 0,      1000, 3, false);
        m.note(bridge::tick_bookkeeping(true), 60,    31000, 3, false);
        check(!m.get().measured,
              "a window shorter than kMinWallSeconds stays NOT measured",
              "30s < 60s");
        check_near(m.get().wall_seconds, 30.0, 0.001,
                   "though the window it HAS seen is reported exactly");

        // The world never moved: a paused episode has no rate, and dividing zero
        // game-days by real seconds would publish 0.0 as though it were measured.
        ClockRateMeter p;
        p.note(bridge::tick_bookkeeping(true), 400,       0, 0, true);
        p.note(bridge::tick_bookkeeping(true), 400, 3600000, 0, true);
        check(!p.get().measured,
              "an hour of PAUSED wall time is NOT a measured rate of zero");
        check(p.get().paused_samples == 2, "but the paused samples are counted");
    }

    // -----------------------------------------------------------------------
    printf("\n2. THE MEASUREMENT, in both the units the design is written in\n");
    {
        // The node's free-running null-mode figure, as an arithmetic fixture:
        // 1.73 game-days per wall-second over ten wall-minutes.
        ClockRateMeter m;
        const double rate = 1.73;
        for (int i = 0; i <= 600; i += 10) {
            m.note(bridge::tick_bookkeeping(true), (int64_t)(rate * i), (uint64_t)i * 1000ull, 4, false);
        }
        const ClockRateSample s = m.get();
        check(s.measured, "a ten-minute window IS measured");
        check_near(s.game_days_per_wall_second, rate, 0.01,
                   "game-days per wall-second comes back as it went in");
        check_near(s.game_years_per_wall_hour, rate * 3600.0 / 365.0, 0.1,
                   "and the same number in game-years per wall-hour, which is the "
                   "unit Appendix A.19 and design 2.7 are written in");
        check_near(s.wall_seconds, 600.0, 0.001, "over the window it names");

        // THE CONDITION travels with the number.
        check(s.speed_min == 4 && s.speed_max == 4,
              "the CK3 speed it was measured at is published beside it");
        check(s.paused_samples == 0, "and how much of the window was paused");
        check(s.samples == 61, "and how many samples it rests on");
        check(s.reanchors == 0, "and that no reload interrupted it");
    }

    // -----------------------------------------------------------------------
    printf("\n3. A RELOAD RE-ANCHORS — it never averages across the discontinuity\n");
    {
        ClockRateMeter m;
        // 1.0 game-day/s for 100s, then the save is reloaded back to day 0.
        for (int i = 0; i <= 100; i += 10) m.note(bridge::tick_bookkeeping(true), i, (uint64_t)i * 1000ull, 3, false);
        check(m.get().measured, "the first window measures");

        m.note(bridge::tick_bookkeeping(true), 0, 101000, 3, false);                    // the reload
        check(m.get().reanchors == 1, "the day going backwards RE-ANCHORS");
        check(!m.get().measured,
              "and the meter is NOT measured again until the new window is long "
              "enough — averaging a reload would report a rate the run never ran "
              "at, and a negative one when the jump back is large");

        for (int i = 10; i <= 200; i += 10)
            m.note(bridge::tick_bookkeeping(true), i * 2, 101000 + (uint64_t)i * 1000ull, 3, false);
        check(m.get().measured, "the SECOND window measures on its own");
        check_near(m.get().game_days_per_wall_second, 2.0, 0.05,
                   "at the new rate, with nothing carried over from before the reload");
        check(m.get().reanchors == 1, "and the reanchor is still reported");
    }

    // -----------------------------------------------------------------------
    printf("\n4. SPEED CHANGES ARE VISIBLE, not averaged away silently\n");
    {
        ClockRateMeter m;
        for (int i = 0; i <= 60; i += 10) m.note(bridge::tick_bookkeeping(true), i, (uint64_t)i * 1000ull, 2, false);
        for (int i = 70; i <= 120; i += 10) m.note(bridge::tick_bookkeeping(true), i * 3, (uint64_t)i * 1000ull, 5, false);
        const ClockRateSample s = m.get();
        check(s.measured, "the window measures");
        check(s.speed_min == 2 && s.speed_max == 5,
              "and it reports the speed RANGE, so a rate measured across a speed "
              "change cannot be quoted as though it were one speed's rate");
    }

    // -----------------------------------------------------------------------
    printf("\n5. reset() is total\n");
    {
        ClockRateMeter m;
        for (int i = 0; i <= 100; i += 10) m.note(bridge::tick_bookkeeping(true), i, (uint64_t)i * 1000ull, 3, false);
        m.reset();
        const ClockRateSample s = m.get();
        check(!s.measured && s.samples == 0 && s.reanchors == 0,
              "reset clears the window, the samples and the condition");
    }

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    if (g_fails) { printf("test_clock_rate: FAIL\n"); return 1; }
    printf("test_clock_rate: PASS\n");
    return 0;
}
