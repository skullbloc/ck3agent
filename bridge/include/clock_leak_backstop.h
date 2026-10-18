// clock_leak_backstop.h — THE SECOND DETECTOR FOR THE ck-y6fp CLOCK LEAK (ck-epzs)
//
// NOT AN ALTERNATIVE TO ck-y6fp'S TRIGGER. That one keys on the session that
// opened the episode and is narrow, bounded and fast. This one notices when the
// narrow trigger WAS NEVER ARMED.
//
// WHY THE FLUSH COUNTER, AND THIS IS THE REUSABLE PART:
//
//     AN INSTRUMENT GATED ON THE CONDITION IT WOULD DETECT CANNOT DETECT ITS
//     VIOLATION.
//
// ck-zgh4 sampled the still-leaking deploy-13 world twice, 60 s apart, because
// the question was MOVEMENT and not values. Everything episode-scoped was
// static — tickets_open, pulses, buffer_depth, last_activity, waiting_on,
// episode_id '', episode_mode 'free_play', gate_state 'inert' — while flush_id
// went 1953 -> 1959 and the game date advanced. The bridge's OWN clock-rate
// instrument reported measured=false, samples=0 while the clock ran 3,351
// game-days, because it too is gated on an episode existing (ck-zffq).
//
// flush_id is the only witness NOT conditioned on the state being violated,
// which is exactly what makes it the right key.
//
// THE CONDITION, as a conjunction:
//     the pipeline OWNS the clock, the hold is NOT armed, the episode id is
//     EMPTY, the mode is free play, and flush_id IS ADVANCING.
//
// IT DEPENDS ON ck-kc32 (20e21f76). flush_id advances during every healthy
// episode too; what makes the conjunction safe is episode_id being NON-EMPTY
// then, and that is the field ck-kc32 started publishing. Built against a bridge
// where episode_id is always '', this would fire on every healthy episode.
//
// PURE AND LEAF ON PURPOSE. No engine symbols, no globals, no clock: the caller
// samples and passes the sample in. That is what lets the rule be driven on a
// host, including the flip — a healthy episode must NOT trip it — which is the
// leg that would have caught the paragraph above.

#pragma once

#include <cstdint>

namespace bridge {

//: One observation of the five fields the conjunction is made of.
struct ClockLeakSample {
    bool     owns_clock       = false;  // pipeline mode; E0 (null) is never here
    bool     hold_armed       = false;  // the ck-zen6 hold
    bool     episode_id_empty = true;   // ck-kc32 publishes this
    bool     free_play        = true;   // episode_mode
    uint64_t flush_id         = 0;      // THE UNCONDITIONED WITNESS
    uint64_t now_ms           = 0;      // monotonic
};

//: Carried between observations. Zero-initialised is a valid starting state.
struct ClockLeakState {
    uint64_t last_flush_id   = 0;
    uint64_t suspect_since_ms = 0;   // 0 = not currently suspect
    bool     have_last        = false;
    //: LATCHED until the condition CLEARS. Without it the rule re-fires every
    //: span for as long as the leak lasts — the probe measured 8 times over ten
    //: minutes, which is how I found out the header's "exactly once" was a
    //: claim about intent rather than about the code.
    bool     fired            = false;
};

//: How long the conjunction must HOLD before the backstop fires. A single
//: observation cannot see "advancing", and a fix that fires on one sample would
//: race the very window between a release and the next state update.
constexpr uint64_t kClockLeakMinSpanMs = 60000;

//: Pure. Returns true exactly once per leak, when the conjunction has held
//: continuously for `min_span_ms` AND flush_id advanced across it. The caller
//: re-arms the hold on true; the condition then clears itself.
bool clock_leak_backstop_step(const ClockLeakSample& s, ClockLeakState* st,
                              uint64_t min_span_ms = kClockLeakMinSpanMs);

}  // namespace bridge
