// speed_runtime_probe.cpp — THE SHARED VECTOR TABLE, DRIVEN THROUGH THE PATH
// THE FLUSH PLANNER ACTUALLY READS (ck-xp7b).
//
// WHY THIS EXISTS BESIDE speed_vector_probe.cpp, WHICH LOOKS LIKE THE SAME FILE.
//
// `speed_vector_probe.cpp` calls `bridge::resolve_game_speed` DIRECTLY. That
// answers "does the pure function follow the byte rule", and it is a real
// question — but it is not the question an episode asks. THE PLANNER NEVER
// CALLS THAT FUNCTION. What it reads is `FlushConfig::resting_speed`, and the
// chain that fills it is:
//
//     episode_policy.cpp  init_from_env()        -> EpisodePolicy::game_speed
//     pipeline.cpp        sched.set_resting_speed(p.game_speed)
//     flush_scheduler.cpp cfg_.resting_speed     -> the plan's SetSpeed step
//                                                -> game_set_speed -> the clock
//
// Between the pure function and the clock sit two translation units and one
// early-return, and NOTHING DROVE AN UGLY INPUT THROUGH THEM. Measured before
// this probe was written: every value any check in the tree ever put in
// CK3_EPISODE_GAME_SPEED is whitespace-free ("%d" of 1..5, "2", "6", "0"). A
// glue that trimmed before resolving would rest the planner at 3 for
// `' 3 '` while `scripts/registered_speed.py` refused it and the validator
// judged every sample against the pre-registered 2 — ck-aqio's own defect,
// mirrored, for a whole horizon, with every existing lane green.
//
// SO THIS PROBE LINKS THE REAL TRANSLATION UNITS. It is not header-only, and
// that cost is the entire point: a probe that links nothing cannot see a defect
// that lives in what links.
//
// ONE ROW PER PROCESS. `getenv` is read inside `init_from_env`, and a resolver
// that answered row N from row N-1's environment would agree with the table on
// every row for the wrong reason. The caller re-executes this binary per row;
// there is no loop here to get that wrong.
//
// IT ECHOES THE BYTES IT ACTUALLY RECEIVED (count and rendered form), for the
// reason speed_vector_probe.cpp states and for one more found the hard way: a
// harness that prints the RAW input into a delimited field reports a
// disagreement the resolvers never had, the moment the input contains the
// delimiter. Tab and newline are both rows of this table.
//
// Build:  g++ -std=c++17 -I bridge/include -I re/offsets -o speed_runtime_probe \
//             bridge/test/speed_runtime_probe.cpp \
//             bridge/src/episode_policy.cpp bridge/src/flush_scheduler.cpp -lpthread
// Usage:  ./speed_runtime_probe UNSET
//         ./speed_runtime_probe SET <raw-bytes>

#include "episode_policy.h"
#include "flush_scheduler.h"
#include "registered_speed.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: speed_runtime_probe <UNSET | SET <raw>>\n");
        return 2;
    }
    const bool unset = (strcmp(argv[1], "UNSET") == 0);
    std::string raw;
    if (unset) {
        unsetenv(bridge::kGameSpeedEnv);
    } else {
        if (strcmp(argv[1], "SET") != 0 || argc < 3) {
            fprintf(stderr, "usage: speed_runtime_probe <UNSET | SET <raw>>\n");
            return 2;
        }
        raw = argv[2];
        setenv(bridge::kGameSpeedEnv, raw.c_str(), 1);
    }

    // ── the three statements pipeline_episode_init_from_env() holds ─────────
    bridge::episode_policy().init_from_env();
    const bridge::EpisodePolicy p = bridge::episode_policy().get();
    bridge::FlushScheduler sched;                       // FlushConfig{} defaults
    const int  default_resting = sched.config().resting_speed;
    const bool set_ok  = sched.set_resting_speed(p.game_speed);
    const int  resting = sched.config().resting_speed;

    // ── and the step the planner would hand to the clock ────────────────────
    // A PAUSED-polarity call is what makes a flush touch the clock at all; an
    // all-ANY flush is required by §2.3 to touch nothing, so planning one would
    // report "no SetSpeed step" for every row and discriminate nothing. The
    // clock is read RUNNING at a speed the restore step must disagree with.
    bridge::CallRequest req;
    req.session_id = "runtime_probe";
    req.action     = "query_character";
    req.polarity   = bridge::Polarity::PAUSED;
    sched.register_session(req.session_id);
    sched.submit(req, 1000);
    const int reading = (resting == 5) ? 1 : 5;
    const bridge::FlushPlan plan =
        sched.plan_flush(2000, bridge::ClockReading{false, reading},
                         bridge::FlushTrigger::Cadence);
    int planned_setspeed = -1;
    for (const bridge::PlanStep& s : plan.steps)
        if (s.kind == bridge::StepKind::SetSpeed) planned_setspeed = s.speed;

    printf("declared\tspeed\tpresent\trefused\tset_ok\tresting\tdefault_resting\t"
           "planned_setspeed\tclock_after\traw_len\traw_rendered\n");
    printf("%d\t%d\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%zu\t%s\n",
           p.game_speed_declared ? 1 : 0,
           p.game_speed,
           p.game_speed_present ? 1 : 0,
           bridge::render_bytes(p.refused_game_speed_env).c_str(),
           set_ok ? 1 : 0,
           resting,
           default_resting,
           planned_setspeed,
           plan.clock_after.speed,
           unset ? static_cast<size_t>(0) : raw.size(),
           unset ? "UNSET" : bridge::render_bytes(raw).c_str());
    return 0;
}
