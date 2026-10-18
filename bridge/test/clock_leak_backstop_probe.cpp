// clock_leak_backstop_probe.cpp — DRIVES THE SHIPPING RULE (ck-epzs)
//
// The rule lives in a pure leaf so this can link it WITHOUT the pipeline, the
// scheduler or the engine — the sampling is the conductor's, the judgement is
// the rule's, and only the judgement is under test here.
//
// THE FLIP IS NOT OPTIONAL. flush_id advances during every healthy episode too;
// what makes the conjunction safe is episode_id being NON-EMPTY then. A rule
// built against a bridge where episode_id is always '' would fire on every
// healthy episode, and only the flip leg can see that.

#include "clock_leak_backstop.h"

#include <cstdio>

using bridge::ClockLeakSample;
using bridge::ClockLeakState;
using bridge::clock_leak_backstop_step;

namespace {

constexpr uint64_t SPAN = 60000;

// Feed `n` samples `step_ms` apart, advancing flush_id unless told not to.
// Returns how many times the rule fired.
int drive(ClockLeakSample s, int n, uint64_t step_ms, bool advance,
          uint64_t span = SPAN) {
    ClockLeakState st;
    int fired = 0;
    for (int i = 0; i < n; ++i) {
        if (advance) s.flush_id += 6;
        if (clock_leak_backstop_step(s, &st, span)) ++fired;
        s.now_ms += step_ms;
    }
    return fired;
}

ClockLeakSample leaking() {
    ClockLeakSample s;
    s.owns_clock = true; s.hold_armed = false;
    s.episode_id_empty = true; s.free_play = true;
    s.flush_id = 1953; s.now_ms = 0;
    return s;
}

}  // namespace

int main() {
    std::printf("case\tvalue\n");

    // THE LEAK ITSELF — ck-zgh4's world, 10 s apart, flush_id advancing.
    std::printf("leak_fires\t%d\n", drive(leaking(), 12, 10000, true));

    // THE FLIP: A HEALTHY EPISODE. Same advancing flush loop, same released
    // hold — and an episode id. Must never fire.
    ClockLeakSample healthy = leaking();
    healthy.episode_id_empty = false;
    std::printf("healthy_episode_fires\t%d\n", drive(healthy, 60, 10000, true));

    // AN EXPERIMENT ARM: episode_mode is not free play.
    ClockLeakSample arm = leaking();
    arm.free_play = false;
    std::printf("experiment_arm_fires\t%d\n", drive(arm, 60, 10000, true));

    // E0: NULL MODE DOES NOT OWN THE CLOCK. The node runs eight of these.
    ClockLeakSample e0 = leaking();
    e0.owns_clock = false;
    std::printf("e0_null_mode_fires\t%d\n", drive(e0, 60, 10000, true));

    // THE HOLD ALREADY ARMED — preflight. Nothing to re-arm.
    ClockLeakSample held = leaking();
    held.hold_armed = true;
    std::printf("held_world_fires\t%d\n", drive(held, 60, 10000, true));

    // SUSPECT BUT NOT ADVANCING: a paused world with a stale flag is a
    // different defect and must not be mistaken for a leak.
    std::printf("suspect_but_static_fires\t%d\n",
                drive(leaking(), 60, 10000, false));

    // INSIDE THE SPAN: the same leak, sampled for less than the bound.
    std::printf("leak_inside_span_fires\t%d\n", drive(leaking(), 4, 10000, true));

    // FIRES ONCE, NOT EVERY TICK, for a caller that ignores the return.
    std::printf("leak_fires_once_over_10min\t%d\n",
                drive(leaking(), 60, 10000, true));

    // A CLOCK THAT GOES BACKWARDS must not fire early.
    {
        ClockLeakSample s = leaking();
        ClockLeakState st;
        int fired = 0;
        s.flush_id += 6; fired += clock_leak_backstop_step(s, &st, SPAN) ? 1 : 0;
        s.now_ms = 10000; s.flush_id += 6;
        fired += clock_leak_backstop_step(s, &st, SPAN) ? 1 : 0;
        s.now_ms = 5000;  s.flush_id += 6;   // backwards
        fired += clock_leak_backstop_step(s, &st, SPAN) ? 1 : 0;
        std::printf("clock_backwards_fires_early\t%d\n", fired);
    }

    // AND THE RESIDUE THIS BEAD EXISTS FOR: a release with NO OWNER recorded.
    // ck-y6fp's trigger cannot fire — it has nothing to key on — and the state
    // the world is left in is EXACTLY the leaking sample above. The backstop
    // does not know or care why the owner is missing, which is the point: it is
    // not conditioned on a declaration having been seen.
    std::printf("residue_no_owner_fires\t%d\n", drive(leaking(), 12, 10000, true));
    return 0;
}
