// tick_bookkeeping.h — WHAT THE CONDUCTOR RECORDS EACH ITERATION, and why the
// two things it records are scoped differently (ck-zffq).
//
// THE DEFECT THIS EXISTS TO PREVENT RECURRING. Both recordings used to sit
// inside one `if (episode_is_experiment())`, so on a world that had run away
// unattended the clock-rate meter read measured=false, samples=0, game_days=0
// WHILE THE CLOCK ADVANCED 3,351 GAME-DAYS (measured by ck-zgh4).
//
//     AN INSTRUMENT GATED ON THE CONDITION IT WOULD DETECT CANNOT DETECT ITS
//     VIOLATION.
//
// It went blind in precisely the situation where game time is being spent with
// nobody watching — the one situation where someone would later want to ask what
// the rate had been.
//
// THE TWO ARE NOT THE SAME QUESTION:
//   UNAVAILABILITY  what an agent's silence costs THE EXPERIMENT. Meaningless
//                   without an experiment, so episode-scoped, correctly.
//   CLOCK RATE      game-days per wall-second: A PROPERTY OF THE WORLD. Nothing
//                   about it requires an episode, and `measured:false` is
//                   supposed to mean NOT MEASURED rather than NOT MEASURABLE
//                   HERE.
//
// A PURE LEAF ON PURPOSE. The scoping was an `if` inside a loop that needs the
// engine to run, so the only available check was READING the call site. Naming
// the policy makes it drivable on a host, which is the difference between a
// rule that is asserted and one that is executed.

#pragma once

namespace bridge {

//: What the conductor should record on this iteration.
struct TickBookkeeping {
    bool note_unavailability = false;  // §2.7's covariate — episode-scoped
    bool note_clock_rate     = false;  // the world's rate — NOT episode-scoped
};

//: Pure. `is_experiment` is the ONLY input, and that is the claim: nothing else
//: about an episode may narrow what the clock-rate meter sees.
constexpr TickBookkeeping tick_bookkeeping(bool is_experiment) {
    return TickBookkeeping{is_experiment, true};
}

}  // namespace bridge
