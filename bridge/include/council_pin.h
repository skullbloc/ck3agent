// council_pin.h — the per-seat council-task pin (wave 12).
//
// WHAT THE NATIVE AI DOES (re/findings/re_council_ai.md, HIGH):
//   CAIManager::PostExecute (image 0x04dedfd0) runs bucket 3 (FUN_04ded5f0) ->
//   per-character dispatcher FUN_04d89360(CAIStrategy* ai, p2) ->
//   [global byte DAT_0bfa5c83] FUN_04dca2a0(ai, p2)            (per ruler)
//     -> [!ai+0x2a] for each position id of *(ai+0x18)'s council:
//        FUN_04dc9fc0(CCharacter* ruler, position* pos, p2, taskDB)  (per seat)
//          -> keeps the current task unless another task of that seat scores
//             above 1.25 x the current one (define at 0x0c16dfe0, 125000 in the
//             867 dump), then AI_CouncilLogic sends a change-council-task command.
//   Bucket 3 comes round per ruler every 180 days (duke and up; 360 county,
//   720 barony: the tier table at 0x0c16d508, 867 dump).
//
// THE PRE-FIX LOCK NEVER MATCHED. council_suppress_hook hooked FUN_04dca2a0 and
// read the ruler id at arg+0x18 — but the arg is the CAIStrategy, whose +0x18 is
// the CCharacter POINTER. Its low 24 bits never equal an id (867 dump: 0 of
// 4593 landed rulers' AI objects). The test's control rebuilds that read and
// must fail.
//
// THE PIN: hook FUN_04dc9fc0 (one caller, FUN_04dca2a0) and skip the call for
// exactly the (ruler, position) a seat set, while that ruler is a seated
// (controlled) character. Every other seat of that ruler, and every other
// ruler, runs the native pass unchanged. Both ids are read where the engine
// reads them: ruler id at CCharacter+0x18 (ck3::character::ID), position id at
// position+0x10 (ck3::globals::COUNCIL_POS_KEY; FUN_04dca2a0 matches it
// against the council's id list before the call).
//
// PURE: no engine, no locks. council_suppress_hook.cpp holds the set under a
// mutex and calls these; bridge/test/test_council_pin.cpp tests them host-side.
#ifndef CK3_BRIDGE_COUNCIL_PIN_H
#define CK3_BRIDGE_COUNCIL_PIN_H

#include <cstdint>
#include <cstring>
#include <unordered_set>

namespace bridge {
namespace council_pin {

// Field offsets the hook reads. Mirrors of ck3_offsets.h (character::ID and
// globals::COUNCIL_POS_KEY); the test checks the mirror when it can see both.
constexpr uintptr_t kRulerIdOffset    = 0x18;  // CCharacter+0x18: u32 id (lo 24 bits)
constexpr uintptr_t kPositionIdOffset = 0x10;  // council position+0x10: u32 position id

inline uint32_t ruler_id_at(const void* ruler_char) {
    uint32_t v = 0;
    std::memcpy(&v, static_cast<const unsigned char*>(ruler_char) + kRulerIdOffset, sizeof v);
    return v & 0xFFFFFF;
}

inline uint32_t position_id_at(const void* position) {
    uint32_t v = 0;
    std::memcpy(&v, static_cast<const unsigned char*>(position) + kPositionIdOffset, sizeof v);
    return v;
}

// One pin = one ruler's one council position. The ruler id is the 24-bit id;
// the position id is kept whole (its generation byte included), as the engine
// compares it.
inline uint64_t key(uint32_t ruler_id, uint32_t position_id) {
    return (static_cast<uint64_t>(ruler_id & 0xFFFFFF) << 32) | position_id;
}

class PinSet {
public:
    // A ruler id 0 or 0xFFFFFF, or a position id 0 / 0xFFFFFFFF, is refused.
    bool add(uint32_t ruler_id, uint32_t position_id) {
        ruler_id &= 0xFFFFFF;
        if (ruler_id == 0 || ruler_id == 0xFFFFFF) return false;
        if (position_id == 0 || position_id == 0xFFFFFFFFu) return false;
        pins_.insert(key(ruler_id, position_id));
        return true;
    }
    bool has(uint32_t ruler_id, uint32_t position_id) const {
        return pins_.count(key(ruler_id, position_id)) != 0;
    }
    // Drops every pin of this ruler; returns how many.
    size_t remove_ruler(uint32_t ruler_id) {
        const uint64_t r = static_cast<uint64_t>(ruler_id & 0xFFFFFF);
        size_t n = 0;
        for (auto it = pins_.begin(); it != pins_.end();) {
            if ((*it >> 32) == r) { it = pins_.erase(it); ++n; } else ++it;
        }
        return n;
    }
    size_t size() const { return pins_.size(); }
    bool empty() const { return pins_.empty(); }
private:
    std::unordered_set<uint64_t> pins_;
};

// What the hook does with one (ruler, position) call. Only kSkipPinned skips.
enum class Pass {
    kNativeNoPins,      // fast path: nothing pinned anywhere
    kNativeBadArgs,     // a pointer the hook could not read: fail OPEN
    kNativeNotPinned,   // this seat was not set by a seat
    kNativeNotSeated,   // pinned, but the ruler is no longer a controlled character
    kNativeRulerDead,   // pinned, ruler dead: the hook releases the ruler's pins
    kSkipPinned,        // pinned + seated + alive: the native pass is skipped
};

inline Pass decide(bool any_pins, bool args_readable, bool pinned, bool seated, bool ruler_dead) {
    if (!any_pins) return Pass::kNativeNoPins;
    if (!args_readable) return Pass::kNativeBadArgs;
    if (!pinned) return Pass::kNativeNotPinned;
    if (ruler_dead) return Pass::kNativeRulerDead;
    if (!seated) return Pass::kNativeNotSeated;
    return Pass::kSkipPinned;
}

inline bool skips(Pass p) { return p == Pass::kSkipPinned; }

// What a council-task write reports about the pin (reply field "ai_pin").
enum class Arm {
    kPinned,       // read back the asked task on the seat; pinned
    kNotSeated,    // the actor is not a controlled character: no pin (default off)
    kNotApplied,   // the read-back does not show the asked task: nothing to hold
    kNoPosition,   // the seat could not be found on the actor's council
    kRefused,      // the pin set refused the ids
};

inline Arm arm_verdict(bool have_position, bool readback_matches, bool seated) {
    if (!have_position) return Arm::kNoPosition;
    if (!readback_matches) return Arm::kNotApplied;
    if (!seated) return Arm::kNotSeated;
    return Arm::kPinned;
}

inline const char* arm_name(Arm a) {
    switch (a) {
    case Arm::kPinned: return "pinned";
    case Arm::kNotSeated: return "not_seated";
    case Arm::kNotApplied: return "not_applied";
    case Arm::kNoPosition: return "no_position";
    case Arm::kRefused: return "refused";
    }
    return "refused";
}

}  // namespace council_pin
}  // namespace bridge

#endif  // CK3_BRIDGE_COUNCIL_PIN_H
