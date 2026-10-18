// clock_leak_backstop.cpp — see clock_leak_backstop.h (ck-epzs)
//
// A LEAF TU BY CONSTRUCTION: this file includes its own header and nothing else,
// so a host lane can link the shipping rule without dragging the pipeline, the
// scheduler or the engine in behind it. The rule is the thing under test; the
// sampling is the caller's.

#include "clock_leak_backstop.h"

namespace bridge {

bool clock_leak_backstop_step(const ClockLeakSample& s, ClockLeakState* st,
                              uint64_t min_span_ms) {
    if (!st) return false;

    // THE CONJUNCTION. Any one of these false and the world is explicable:
    // not our clock, already held, an episode is open, or an experiment is
    // running. Only all five together describe a world burning game time with
    // nothing to account for it.
    const bool suspect = s.owns_clock && !s.hold_armed
                      && s.episode_id_empty && s.free_play;

    if (!suspect) {                       // the condition cleared — forget it
        st->suspect_since_ms = 0;
        st->fired = false;                // re-armable: a LATER leak fires again
        st->last_flush_id = s.flush_id;
        st->have_last = true;
        return false;
    }

    if (!st->have_last) {                 // first sight: nothing to compare to
        st->last_flush_id = s.flush_id;
        st->have_last = true;
        st->suspect_since_ms = s.now_ms;
        return false;
    }

    // ADVANCING is the whole point. A suspect world whose flush loop is NOT
    // running is a paused world with a stale hold flag — a different defect,
    // and one this backstop must not mistake for a leak. Note the wrap-safe
    // comparison: flush_id is monotonic in practice, but "greater than" on a
    // counter the caller supplies is a claim about the caller.
    const bool advanced = s.flush_id != st->last_flush_id;
    st->last_flush_id = s.flush_id;
    if (!advanced) {
        st->suspect_since_ms = 0;         // not spending game time; not a leak
        return false;
    }

    if (st->suspect_since_ms == 0) {      // suspicion starts here
        st->suspect_since_ms = s.now_ms;
        return false;
    }
    if (s.now_ms < st->suspect_since_ms) {         // clock went backwards
        st->suspect_since_ms = s.now_ms;
        return false;
    }
    if (s.now_ms - st->suspect_since_ms < min_span_ms) return false;

    // FIRE ONCE PER LEAK, LATCHED. The caller re-arms, which makes the next
    // sample not suspect and clears the latch through the !suspect branch — so
    // a LATER leak fires again. The latch matters for the caller that does NOT
    // re-arm: without it this returned true every span for as long as the leak
    // lasted, which the probe measured at 8 times over ten minutes and is how
    // the header's "exactly once" turned out to be a claim about intent.
    if (st->fired) return false;
    st->fired = true;
    return true;
}

}  // namespace bridge
