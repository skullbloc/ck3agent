// primary_cause.h — WHY THE CONTROLLED PRIMARY CHANGED, recorded by the action
// that changed it (ck-6xi6).
//
// `g_controlled_primary` is a DERIVED MIRROR: SessionRegistry::
// rebuild_legacy_mirrors_locked rewrites it for a claim, a release and a handoff
// alike, so an observer that notices the value changed cannot tell WHICH of
// those happened. Inferring the cause from what else appeared in the same tick
// would be a coincidence dressed as a fact — two events sharing a tick is not
// one causing the other.
//
// So the two originating actions each record their own cause:
//   do_set_controlled_character      -> control
//   check_controlled_handoff's swap  -> handoff   (stamped at the HANDOFF site,
//                                                  not inside swap_character,
//                                                  which the control path shares)
//   do_release_character             -> release
//
// THE ID IS STORED WITH THE CAUSE IN ONE WORD, so the pairing is CHECKABLE
// rather than assumed. A reader trusts the cause only when its id matches the
// primary it actually observed, and reports `unknown` otherwise. A cause that
// does not identify what it is the cause OF is not evidence — and a stale stamp
// left by an earlier change is exactly what would otherwise be read as current.
//
// THIS HEADER IS DELIBERATELY DEPENDENCY-FREE. It lives apart from
// ai_enabled_watch.h so that dispatch_router.cpp — which only needs to RECORD a
// cause — does not pull in perf_event_open, signal handling and the ring buffer
// to do it. The watch is one consumer of this fact, not its owner.
#pragma once

#include <atomic>
#include <cstdint>

namespace bridge {

enum AiPrimaryCause : uint32_t {
    kPrimaryCauseUnknown = 0,
    kPrimaryCauseControl = 1,
    kPrimaryCauseHandoff = 2,
    kPrimaryCauseRelease = 3,
};

inline std::atomic<uint64_t> g_primary_change_note{0};

inline void note_primary_change(uint32_t new_id, AiPrimaryCause cause) {
    g_primary_change_note.store((static_cast<uint64_t>(new_id) << 32)
                                | static_cast<uint32_t>(cause),
                                std::memory_order_release);
}

inline AiPrimaryCause primary_change_cause_for(uint32_t observed_id) {
    const uint64_t note = g_primary_change_note.load(std::memory_order_acquire);
    if (static_cast<uint32_t>(note >> 32) != observed_id) return kPrimaryCauseUnknown;
    return static_cast<AiPrimaryCause>(static_cast<uint32_t>(note & 0xFFFFFFFFu));
}

//: WHOLE LITERALS, one per branch. A word assembled at runtime has no .rodata
//: string, so the pre-deploy `strings` witness cannot see it — the run-1 lesson
//: that cost a build witness.
inline const char* ai_primary_cause_name(AiPrimaryCause c) {
    switch (c) {
        case kPrimaryCauseControl: return "control";
        case kPrimaryCauseHandoff: return "handoff";
        case kPrimaryCauseRelease: return "release";
        default:                   return "unknown";
    }
}

}  // namespace bridge
