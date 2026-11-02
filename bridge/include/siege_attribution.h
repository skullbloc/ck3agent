// siege_attribution.h — which war a siege belongs to (lane l5, F8).
//
// A CWar stores no sieges (re_siege_progress_struct.md §1). query_wars used to
// list every siege whose besieger was one of the war's ATTACKERS, so a ruler
// fighting three wars showed the same siege under all three, each with that
// war's primary defender written in as the siege's defender (live867 l5fx5:
// sieges 6 and 7 of 14859 listed under wars 3, 4 and 16777218).
//
// The besieged holding is read now: the barony whose CProvince back-links the
// siege (province+0x368, re_siege_progress_struct.md §4; 64/64 sieges of the
// 1097 dump and 1/1 of the 867 dump, one barony each), and its holder. A siege
// belongs to a war when the besieger and the holding's realm (the holder or
// one of his lieges) are on OPPOSITE sides of that war. That is the rule here,
// engine-free so a host test reaches it (bridge/test/test_siege_attribution.cpp).
//
// What the rule cannot see, stated: a siege of a holding on the besieger's own
// side (retaking an occupied holding: 4 of 64 in the 1097 dump) has its enemy
// in the occupier, which is not read; it is attributed to no war. A siege
// whose besieger and holding realm oppose each other in two wars is listed
// under both (1 of 64 in the 1097 dump): the reads cannot tell them apart.
#pragma once

#include <cstdint>

namespace siege_attr {

enum class Side : int { None = 0, Attacker = 1, Defender = 2 };

inline bool has_id(const uint32_t* ids, int n, uint32_t id) {
    if (!ids || id == 0) return false;
    for (int i = 0; i < n; i++)
        if (ids[i] == id) return true;
    return false;
}

inline bool any_in(const uint32_t* chain, int chain_n, const uint32_t* ids, int n) {
    for (int i = 0; i < chain_n; i++)
        if (has_id(ids, n, chain[i])) return true;
    return false;
}

// The besieger's side in this war, when the siege is this war's; Side::None
// when it is not. `holder_chain` is the besieged holding's holder followed by
// his lieges up to the top (holder first; empty when the holding is unheld or
// unread, which attributes the siege to no war).
inline Side war_side_of_siege(uint32_t besieger,
                              const uint32_t* holder_chain, int chain_n,
                              const uint32_t* attackers, int atk_n,
                              const uint32_t* defenders, int def_n) {
    if (besieger == 0 || chain_n <= 0) return Side::None;
    if (has_id(attackers, atk_n, besieger) && any_in(holder_chain, chain_n, defenders, def_n))
        return Side::Attacker;
    if (has_id(defenders, def_n, besieger) && any_in(holder_chain, chain_n, attackers, atk_n))
        return Side::Defender;
    return Side::None;
}

}  // namespace siege_attr
