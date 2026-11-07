// decision_eval_plan.h — what the annotated decision eval runs per decision,
// and the typed skip list: one definition, engine-free (ctest:
// bridge/test/test_decision_eval_plan.cpp).
//
// L22 (smoke19 D1): `do(action_class="decisions", candidates=[five], character_id
// =11329)` made the SIGSEGV safety net fire 3x on the game thread at rip_off
// 0x745aea0 (Ghidra FUN_0755ac50 +0x250): `movups (%rbx),%xmm0` with rbx = 0 —
// a CPdxArray<16-byte> insert-range copying from a NULL source whose count read
// 0x7f29 (r12; the high dword of a pointer, i.e. not a count). Its caller is
// FUN_0755ab30, the copy constructor of the engine's 0x48-byte scope object
// (+0x10 array of 16-byte saved-scope entries, +0x28 u32 array), reached from
// the scope-list copies (FUN_0758a070) the trigger system makes while it RECORDS
// a failure (EVAL_WITH_FAILURES -> EVAL_WITH_DIAG, the diagnostic pass that
// builds the "why not" tooltip). The three faults were one per evaluated
// candidate and every one of those rows came back `shown: false` without
// eval_error, so the fault was in the failure-key extraction (a fault in the
// is_shown / is_valid / cost evals sets eval_error), not in the verdict evals.
// The bridge ran that diagnostic pass for EVERY failing block, including blocks
// of decisions the game does not show: the game's UI builds a decision's
// failure tooltip only for a decision it lists, i.e. after is_shown passed, and
// an is_valid block written for a shown decision may assume scopes is_shown
// established. The plan now: the failure key of a block is extracted only when
// the decision is shown (a not-shown decision's reply is now always
// decision_not_shown; before, the reader checked its failures array first, so
// a not-shown decision with keys read as decision_unavailable or cannot_afford:
// a behaviour change, said in the CHANGELOG). Confidence MEDIUM (static
// path + the smoke's registers and rows); the live row decides.
//
// The skip list: hashes whose trigger eval FROZE the game thread (#64, SIGALRM
// masked, no watchdog can bound it). Named here by the engine's own key hash
// (murmur3_32, the hash the decision DB entry carries at +8): the old seed
// comments had the first two swapped (0x0d3cbc02 was called "ruler 13902
// idx 3" and 0xcce24e04 "foster_integration_decision"; the hashes say
// 0x0d3cbc02 = foster_integration_decision, 0xcce24e04 =
// commission_legend_artifact_decision).
#ifndef CK3_BRIDGE_DECISION_EVAL_PLAN_H
#define CK3_BRIDGE_DECISION_EVAL_PLAN_H

#include <cstdint>
#include <cstring>

namespace bridge {
namespace decision_eval {

// MurmurHash3-32, seed 0 (CK3 FUN_08213140; the Windows recipes keep their own
// copy in bridge/src/os/windows/recipes.cpp).
inline uint32_t murmur3_32(const char* data, uint32_t len) {
    const uint32_t c1 = 0xcc9e2d51;
    const uint32_t c2 = 0x1b873593;
    const uint32_t seed = 0;

    uint32_t h = seed;
    const uint32_t nblocks = len / 4;

    const uint8_t* blocks = reinterpret_cast<const uint8_t*>(data);
    for (uint32_t i = 0; i < nblocks; i++) {
        uint32_t k;
        memcpy(&k, blocks + i * 4, 4);
        k *= c1;
        k = (k << 15) | (k >> 17);
        k *= c2;
        h ^= k;
        h = (h << 13) | (h >> 19);
        h = h * 5 + 0xe6546b64;
    }

    const uint8_t* tail = blocks + nblocks * 4;
    uint32_t k1 = 0;
    switch (len & 3) {
    case 3: k1 ^= static_cast<uint32_t>(tail[2]) << 16; // fallthrough
    case 2: k1 ^= static_cast<uint32_t>(tail[1]) << 8;  // fallthrough
    case 1: k1 ^= static_cast<uint32_t>(tail[0]);
        k1 *= c1;
        k1 = (k1 << 15) | (k1 >> 17);
        k1 *= c2;
        h ^= k1;
    }

    h ^= len;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;

    return h;
}

// Run the diagnostic failure-key pass for a block that did not pass?
inline bool extract_failure_key(bool shown, bool block_passed) {
#ifdef DECISION_EVAL_PLAN_PREFIX_CONTROL
    (void)shown;
    return !block_passed;                 // pre-L22: every failing block
#else
    return shown && !block_passed;
#endif
}

// Why a hash is on the skip list.
enum class SkipOrigin { Seed, Learned };

struct SkipSeed {
    uint32_t hash;
    const char* name;     // the decision key; murmur3_32(name) == hash (ctest)
    const char* reason;   // what was observed
};

constexpr SkipSeed kSkipSeeds[] = {
    {0x0d3cbc02u, "foster_integration_decision",
     "froze the game thread in its trigger eval (#64 full scan, ruler 13902)"},
    {0xcce24e04u, "commission_legend_artifact_decision",
     "froze the game thread in its trigger eval (#64 self-heal capture, 2026-06-19)"},
    {0x5f86cc47u, "create_armenian_empire_decision",
     "froze the game thread in its trigger eval (#109 Surface-2 live check, 2026-06-22)"},
};

// The seed entry for a hash, or nullptr (a learned hash: an in-flight marker a
// freeze stranded, promoted by load_decision_skip).
inline const SkipSeed* seed_for(uint32_t hash) {
    for (const SkipSeed& s : kSkipSeeds)
        if (s.hash == hash) return &s;
    return nullptr;
}

inline const char* origin_name(SkipOrigin o) {
    return o == SkipOrigin::Seed ? "seed" : "learned";
}

}  // namespace decision_eval
}  // namespace bridge

#endif  // CK3_BRIDGE_DECISION_EVAL_PLAN_H
