// truce_read.h — a character's one-way truce on another, as the game stores it.
//
// The game keeps a truce in the pairwise CDiplomaticRelation of the two characters
// (re/offsets/ck3_offsets.h diplomatic_relation, relation_list), one slot per character:
// the slot of the character whose full id is at +0x08 at +0x28 (expiry date, war name
// +0x30, result +0x50), the other's at +0x58 (+0x60, +0x80). add_truce_one_way
// (FUN_04fc6000) writes the slot of its SCOPE character (FUN_06369460(rel, scope.id, ...)),
// and the game's war endings call it from scope:attacker with character = scope:defender
// (common/scripted_effects/00_war_effects.txt add_truce_*_effect): the slot of X is X's
// truce on the other, "X cannot attack him" (the has_truce trigger's own words: "Truces are
// one way"). FUN_06368b70 (rel, today) clears a slot whose expiry is <= today: sentinel
// date, result 3. re/findings/re_truce_relation_slot.md has the decompiles.
//
// The bridge first searched GetTruceList(attacker) (char+0x1C0+0x6A0, an array of
// character pointers) for the defender: a seated game read `truce false` three times
// (1094-1096) while the game's has_truce held and refused the declaration.
//
// Header-only and engine-free: test_truce_read.cpp checks it on a host with no game.
#pragma once

#include <cstdint>

namespace bridge::truce {

//: result code of a slot that holds no truce (FUN_06368b70 writes it on expiry)
inline constexpr int32_t kResultNone = 3;

struct Slot {
    bool matched = false;   // `me` is one of the relation's two characters
    bool active = false;    // me's slot holds a truce that has not expired
    int32_t expiry = 0;     // raw game date (days), 0 when not active
    int32_t result = kResultNone;
};

//: The slot of `me` (24-bit id) in a relation holding ids `lo` (+0x08) and `hi` (+0x0C), from
//: the four slot words the caller read. `today` 0 = unread (no expiry test); `sentinel` the
//: game's no-truce date (globals::TRUCE_SENTINEL_DATE), read by the caller.
inline Slot slot_of(uint32_t lo, uint32_t hi, uint32_t me, int32_t exp_a, int32_t res_a, int32_t exp_b,
                    int32_t res_b, int32_t today, int32_t sentinel) {
    Slot s;
    me &= 0xFFFFFF;
    int32_t e = 0, r = kResultNone;
    if ((lo & 0xFFFFFF) == me && me != 0) {
        e = exp_a; r = res_a;
    } else if ((hi & 0xFFFFFF) == me && me != 0) {
        e = exp_b; r = res_b;
    } else {
        return s;
    }
    s.matched = true;
    if (e == sentinel || r == kResultNone || e <= 0) return s;
    if (today > 0 && e <= today) return s;      // the game clears it on that day
    s.active = true;
    s.expiry = e;
    s.result = r;
    return s;
}

}  // namespace bridge::truce
