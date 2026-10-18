// acceptance_resting_speed.cpp — ck-on4u, the C++ half.
//
// ONE QUESTION, asked of whichever tree it is compiled against:
//
//     WHAT SPEED DOES A FLUSH PLAN'S `SetSpeed` STEP CARRY?
//
// That step is the SECOND WRITER of the game speed. It reaches `game_set_speed`
// through the effects vtable (`flush_apply.cpp` -> the `set_speed(int) override`
// in `pipeline.cpp` -> `do_set_speed`), so a textual grep for `game_set_speed(`
// finds ONE call site while two paths write; and because it enters at
// `do_set_speed` rather than through the IPC dispatcher it logs no
// "Action: set_speed" line, so a log-based discriminator keyed on that line
// reports it as "not a client" — confidently, and wrongly.
//
// TWO LANES, and the first one needs no fork at all:
//
//   LANE A — THE DEFAULT. `FlushConfig{}.resting_speed` against the declared
//   pin. IDENTICAL SOURCE ON BOTH TREES: it reads 2 here and 3 on the pre-fix
//   bytes in scripts/fixtures/resting_speed/pre_on4u/, which IS the defect —
//   a pre-registered constant with a second home.
//
//   LANE B — THE WIRING. Does the DECLARATION reach the planner? The one line
//   below under `#ifdef` is the line `pipeline.cpp` holds in production. It is
//   forked because on the pre-fix tree THAT LINE DID NOT EXIST AND COULD NOT:
//   `EpisodePolicy` had no `game_speed` and `FlushScheduler` had no setter, so
//   nothing on the live path could set the resting speed at all. The fork
//   encodes an ABSENCE, not a re-implementation — every NUMBER on both sides
//   still comes from the tree's own bytes.
//
// HOST ONLY. `flush_scheduler.cpp` and `episode_policy.cpp` are engine-free by
// construction, so this runs with no CK3 and no game.
//
// Driven by scripts/acceptance_resting_speed.py.

#include "episode_policy.h"
#include "flush_scheduler.h"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace bridge;

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what, const std::string& detail) {
    ++g_checks;
    if (!ok) ++g_failures;
    printf("  [%s] %s\n         %s\n", ok ? "PASS" : "FAIL", what, detail.c_str());
}

// The plan's SetSpeed step, if it emitted one. -1 means "no SetSpeed step",
// which is a DIFFERENT answer from "a step carrying speed 0" and is kept so.
int planned_set_speed(FlushScheduler& s, int clock_speed) {
    CallRequest req;
    req.session_id = "acc";
    req.action = "declare_war";
    req.polarity = Polarity::TICK_WRITE;
    s.submit(req, 0);
    FlushPlan p = s.plan_flush(10000, ClockReading{false, clock_speed},
                               FlushTrigger::Cadence);
    for (const PlanStep& st : p.steps)
        if (st.kind == StepKind::SetSpeed) return st.speed;
    return -1;
}

}  // namespace

int main() {
#if defined(CK3_RESTING_SPEED_PREFIX_CONTROL)
    const char* tree = "PRE-FIX (scripts/fixtures/resting_speed/pre_on4u)";
    const int declared_pin = 2;      // what §7.3 pinned, for the report only
#else
    const char* tree = "SHIPPING";
    const int declared_pin = kRegisteredGameSpeed;
#endif
    printf("acceptance_resting_speed — tree: %s\n", tree);

    // ── LANE A: THE DEFAULT ────────────────────────────────────────────────
    printf("\nA  THE DEFAULT — does the planner rest at the pre-registered pin?\n");
    {
        FlushConfig cfg;
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "FlushConfig{}.resting_speed = %d, the pre-registered pin is %d",
                 cfg.resting_speed, declared_pin);
        check(cfg.resting_speed == declared_pin,
              "the planner's DEFAULT resting speed IS the declaration", buf);
    }

    // ── LANE B: THE WIRING ─────────────────────────────────────────────────
    printf("\nB  THE WIRING — does an episode's DECLARATION reach the planner?\n");
    {
        // Every speed in range, so a lane that passes because the declaration
        // happens to equal the default is impossible: 3 is what the pre-fix
        // header rests at, 2 is the pin, and 1/4/5 are neither.
        const int wanted[] = {1, 2, 3, 4, 5};
        for (size_t i = 0; i < sizeof(wanted) / sizeof(wanted[0]); ++i) {
            char val[8];
            snprintf(val, sizeof(val), "%d", wanted[i]);
            setenv("CK3_EPISODE_GAME_SPEED", val, 1);
            episode_policy().init_from_env();

            FlushConfig cfg;
#if !defined(CK3_RESTING_SPEED_PREFIX_CONTROL)
            // THE LINE pipeline.cpp HOLDS. On the pre-fix tree there is nothing
            // to write here — that is the defect, stated as an absence.
            cfg.resting_speed = episode_policy().get().game_speed;
#endif
            FlushScheduler s(cfg);
            // The clock reads a speed that is NEVER the one we declared, so a
            // plan that emits no step at all fails rather than passing quietly.
            const int reading = (wanted[i] == 5) ? 1 : 5;
            const int got = planned_set_speed(s, reading);
            char buf[220];
            snprintf(buf, sizeof(buf),
                     "declared %d, clock read %d, the plan's SetSpeed step carries "
                     "%d%s", wanted[i], reading, got,
                     got == -1 ? " (NO SetSpeed step was emitted)" : "");
            char what[120];
            snprintf(what, sizeof(what),
                     "a declared speed of %d reaches the plan", wanted[i]);
            check(got == wanted[i], what, buf);
        }
        unsetenv("CK3_EPISODE_GAME_SPEED");
    }

    // ── LANE C: THE STEP IS NOT REMOVED ────────────────────────────────────
    printf("\nC  THE RESTORE STEP SURVIVES — it is what makes E0 free-run at a "
           "FIXED speed\n");
    {
        setenv("CK3_EPISODE_GAME_SPEED", "2", 1);
        episode_policy().init_from_env();
        FlushConfig cfg;
#if !defined(CK3_RESTING_SPEED_PREFIX_CONTROL)
        cfg.resting_speed = episode_policy().get().game_speed;
#endif
        FlushScheduler s(cfg);
        const int got = planned_set_speed(s, 4);   // somebody moved the clock
        char buf[200];
        snprintf(buf, sizeof(buf),
                 "clock drifted to 4; the plan %s",
                 got == -1 ? "emitted NO SetSpeed step — the world would keep "
                             "running at 4"
                           : "restores the resting speed");
        check(got != -1, "a drifted clock still gets a SetSpeed step", buf);
        unsetenv("CK3_EPISODE_GAME_SPEED");
    }

    printf("\n%d checks, %d failures\n%s\n", g_checks, g_failures,
           g_failures ? "FAILED" : "OK");
    return g_failures ? 1 : 0;
}
