// ck-475j — count every typed refusal, so a fix that made a caller's bug QUIET
// does not also make it invisible.
//
// WHY THIS EXISTS. ck-vf92 stopped `{"character_id":0}` from crashing the
// bridge. Before it, that input produced four unmissable recovered SIGSEGVs in
// armb1; after it, the call is refused with a typed reason and the episode
// carries on. THE CALLER'S BUG IS UNCHANGED — only its visibility changed. The
// sole evidence it is still happening is one log line, and nothing counted it:
// not the heartbeat, not the bundle. A reader who does not grep that exact
// string sees a clean run, and "fixed" reads identically to "still happening,
// silently absorbed".
//
// THAT SHAPE HAPPENED THREE TIMES IN ONE NIGHT and is the reason this is a
// header rather than a one-off counter:
//   - log rotation truncated the trace it existed to preserve (ck-otj9)
//   - the backpressure throttle hid drops 2-99 process-wide (ck-0zfc)
//   - ck-vf92 turned four SIGSEGVs into one uncounted line (this bead)
// Each time, the thing that made a problem LEGIBLE was removed by the thing
// that made it SAFE.
//
// AND THE EPISTEMIC HALF, which is the actual deliverable: a clean bundle
// cannot support the claim that the character_id-0 caller was fixed. That needs
// the marker's ABSENCE ACROSS A RUN THAT WOULD HAVE SHOWN IT — which is what a
// counter on the heartbeat provides and a grep does not — or a driver-side
// change with its own evidence.
//
// Lock-free by construction: a fixed enum-indexed table of atomics. Refusals
// are rare, but they are recorded from the reader thread and the game thread
// both, and after ck-1v1t I am not adding a mutex to a path that a siglongjmp
// can escape.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace bridge {

extern FILE* g_logfile;

// One entry per typed refusal the bridge can emit. Adding a refusal to the code
// without adding it here is the failure this guards against, so the acceptance
// counts REFUSING sites against this enum and reds when they diverge.
enum class RefusalReason : uint8_t {
    kCharacterIdInvalid = 0,   // ck-vf92: character_id <= 0 for set_character_ai
    kAiControllerUnmapped,     // ck-bl15: controller non-null but +0x28 unreadable
    kAiControllerUnbound,      // ck-23hv: decision actor's AI controller unbound
    kCount
};

inline const char* refusal_name(RefusalReason r) {
    switch (r) {
        case RefusalReason::kCharacterIdInvalid:  return "character_id_invalid";
        case RefusalReason::kAiControllerUnmapped:return "ai_controller_unmapped";
        case RefusalReason::kAiControllerUnbound: return "ai_controller_unbound";
        case RefusalReason::kCount:               break;
    }
    return "unknown";
}

// THE `= {}` IS LOAD-BEARING, NOT STYLE. std::atomic's default constructor is
// not constexpr in C++17, so without it this array gets DYNAMIC initialisation
// and the including TU emits a second, unprioritised _GLOBAL__sub_I that runs
// AFTER dll_init — exactly the hazard init_order.h:40 documents and the repo's
// test_init_order catches. record_refusal() is called from the accept-thread
// heartbeat, so an initialiser after dll_init means these counters can be
// touched before construction.
//
// Value-initialisation makes it constant-initialised, so NO dynamic initialiser
// is emitted at all — strictly better than CK3_EARLY_INIT, which schedules an
// initialiser rather than removing one. Measured against this header in a TU
// that calls record_refusal: `= {}` -> _GLOBAL__sub_I = 0; without it -> 1.
//
// Caught by ck-t5bc. reply_record.h one header over braces every counter `{0}`
// for this reason — I had already solved it and then wrote the array without.
//
// No check is added here for it: test_init_order owns this property repo-wide
// and measures real initialisers. A second spelling of it in this acceptance is
// precisely what cost two lanes on ck-1v1t.
inline std::atomic<uint64_t> g_refusals[static_cast<size_t>(RefusalReason::kCount)] = {};

inline void record_refusal(RefusalReason r) {
    const size_t i = static_cast<size_t>(r);
    if (i < static_cast<size_t>(RefusalReason::kCount))
        g_refusals[i].fetch_add(1, std::memory_order_relaxed);
}

inline uint64_t refusal_count(RefusalReason r) {
    const size_t i = static_cast<size_t>(r);
    return i < static_cast<size_t>(RefusalReason::kCount)
         ? g_refusals[i].load(std::memory_order_relaxed) : 0;
}

// Renders ONLY non-zero reasons, so a clean run adds nothing to the heartbeat
// and any occurrence at all is visible. "refusals=none" is said explicitly
// rather than omitted — an absent field is indistinguishable from an older
// binary that never had one.
inline const char* format_refusals(char* buf, size_t n) {
    size_t off = 0;
    off += snprintf(buf + off, n - off, "refusals=");
    bool any = false;
    for (size_t i = 0; i < static_cast<size_t>(RefusalReason::kCount); ++i) {
        const uint64_t c = g_refusals[i].load(std::memory_order_relaxed);
        if (!c) continue;
        if (off < n)
            off += snprintf(buf + off, n - off, "%s%s=%llu", any ? "," : "",
                            refusal_name(static_cast<RefusalReason>(i)),
                            (unsigned long long)c);
        any = true;
    }
    if (!any && off < n) snprintf(buf + off, n - off, "none");
    return buf;
}

} // namespace bridge
