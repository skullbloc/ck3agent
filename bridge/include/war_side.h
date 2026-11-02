// war_side.h — which side of a war a character is on: the one definition.
//
// A CWar holds its two sides as CPdxArray<CWarParticipant*> at
// war_obj::ATK_PARTICIPANT / DEF_PARTICIPANT: POINTERS, 8-byte stride, the
// character id at war_participant::CHAR_ID of each dereferenced participant
// (ck3_offsets.h war_obj, HIGH; the layout the live game confirms: in w13-obs
// the duke of war 5 is its primary_defender and is in the DEFENDER array read
// this way, while a reader that indexed the array as inline 0x20-byte entries
// called him the attacker). Every reply that says which side a character is on
// asks here, so two replies cannot disagree about it.
//
// Every read is safe_read (process_vm_readv): served inline on the IPC reader
// thread, a freed participant is a skipped slot, never a fault.
#pragma once

#include "bridge.h"
#include "ck3_offsets.h"

#include <cstdint>

namespace bridge {
namespace war_side {

//: A side's count field above this is not a reading (the array is corrupt or
//: freed): the side is read as empty.
constexpr int32_t kMaxParticipants = 100;

// The participant character ids (low 24 bits) of the side at war_ptr+side_off,
// in array order, at most cap. Returns how many were written.
inline int ids(uintptr_t war_ptr, size_t side_off, uint32_t* out, int cap) {
    const uintptr_t arr = safe_read<uintptr_t>(war_ptr + side_off + pdx_array::DATA_PTR);
    const int32_t count = safe_read<int32_t>(war_ptr + side_off + pdx_array::COUNT);
    if (!arr || !is_mapped(arr) || count <= 0 || count > kMaxParticipants) return 0;
    int n = 0;
    for (int32_t j = 0; j < count && n < cap; j++) {
        const uintptr_t part = safe_read<uintptr_t>(arr + static_cast<uintptr_t>(j) * 8);
        if (!part || !is_mapped(part)) continue;
        const uint32_t pid = safe_read<uint32_t>(part + ck3::war_participant::CHAR_ID) & 0xFFFFFF;
        if (pid) out[n++] = pid;
    }
    return n;
}

// Is char_id (low 24 bits compared) a participant on the side at war_ptr+side_off?
inline bool has(uintptr_t war_ptr, size_t side_off, uint32_t char_id) {
    const uint32_t want = char_id & 0xFFFFFF;
    if (!want) return false;
    uint32_t buf[kMaxParticipants];
    const int n = ids(war_ptr, side_off, buf, kMaxParticipants);
    for (int i = 0; i < n; i++)
        if (buf[i] == want) return true;
    return false;
}

enum class Side { Attacker, Defender, Neither };

// The side char_id is on: the participant arrays first; a primary that is in
// neither array (not observed, but the primary refs are the war's own record of
// its leaders) is on the primary's side.
inline Side of(uintptr_t war_ptr, uint32_t char_id) {
    const uint32_t want = char_id & 0xFFFFFF;
    if (!want) return Side::Neither;
    if (has(war_ptr, ck3::war_obj::ATK_PARTICIPANT, want)) return Side::Attacker;
    if (has(war_ptr, ck3::war_obj::DEF_PARTICIPANT, want)) return Side::Defender;
    if ((safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_ATK_REF) & 0xFFFFFF) == want) return Side::Attacker;
    if ((safe_read<uint32_t>(war_ptr + ck3::war_obj::PRIMARY_DEF_REF) & 0xFFFFFF) == want) return Side::Defender;
    return Side::Neither;
}

// The reply word for a side. Neither is "neutral" for a character that was
// asked about, "unknown" when no character was (scope id 0).
inline const char* name(Side s, uint32_t scope_char_id) {
    switch (s) {
        case Side::Attacker: return "attacker";
        case Side::Defender: return "defender";
        default: return scope_char_id ? "neutral" : "unknown";
    }
}

}  // namespace war_side
}  // namespace bridge
