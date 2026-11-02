// vassal_roster.h — a ruler's direct vassals, read from the ruler's vassal contracts.
//
// The one definition every reader of "direct vassals" calls (query_realm, the
// realm list, query_vassal_loyalty, get_ai_evaluations, the valid-CB and realm
// county walks). A ruler's title_realm carries the refs of the contracts it is
// the liege of (title_realm::VASSAL_CONTRACT_ARRAY, a CPdxArray: ck3_offsets.h
// pdx_array). Each ref names a CSubjectContract in VASSAL_CONTRACT_DB (its
// key at subject_contract::REF_KEY); the contract's SUBJECT is the vassal and its
// LIEGE is the ruler. A contract whose liege is not the ruler, a ref the DB does
// not hold, or a subject that is not a live DB character is left out.
//
// Evidence (re/findings/re_vassal_contract.md, "Contract parties"): in dumps
// ivar867 and 1097 the subjects of every ruler's contracts equal the rulers whose
// realm-parent holder (get_liege_id_from_addr PATH2) is that ruler. title_realm
// +0xA8, which the bridge used to read as "direct vassals", holds no landed
// character at all (re_realm_size_vassal_inclusive.md).
//
// Header-only and engine-free: the memory comes in through `Mem`, so the unit
// test (bridge/test/test_vassal_roster.cpp) drives the same code the bridge runs.
//
//   struct Mem {
//     uint32_t  u32(uintptr_t addr) const;   // 0 when unreadable
//     uintptr_t ptr(uintptr_t addr) const;   // 0 when unreadable
//     bool      mapped(uintptr_t addr) const;
//     uintptr_t contract(uint32_t ref) const; // VASSAL_CONTRACT_DB entry keyed on REF_KEY, or 0
//     uint32_t  char_id(uintptr_t ch) const;  // 24-bit id of the live DB character at ch, or 0
//   };
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "ck3_offsets.h"

namespace bridge::vassal_roster {

//: Bound on the contract array a read will walk (the largest on dump ivar867 holds
//: 88 contracts).
constexpr int32_t MAX_CONTRACTS = 4096;

//: One of the ruler's contracts: the CSubjectContract and its subject's id.
struct LiegeContract {
    uintptr_t contract;
    uint32_t  subject_id;
};

//: The contracts the ruler is the liege of, in the array's order: a contract whose
//: liege is not the ruler, a ref the DB does not hold, or a subject that is not a
//: live DB character (or is the ruler) is left out.
template <class Mem>
std::vector<LiegeContract> liege_contracts(const Mem& m, uintptr_t title_realm, uint32_t ruler_id) {
    std::vector<LiegeContract> out;
    ruler_id &= 0xFFFFFF;
    if (!title_realm || !m.mapped(title_realm)) return out;
    const uintptr_t arr = title_realm + ck3::title_realm::VASSAL_CONTRACT_ARRAY;
    const uintptr_t data = m.ptr(arr);
    const int32_t count = static_cast<int32_t>(m.u32(arr + ::pdx_array::COUNT));
    if (!data || !m.mapped(data) || count <= 0 || count > MAX_CONTRACTS) return out;
    for (int32_t i = 0; i < count; i++) {
        const uint32_t ref = m.u32(data + static_cast<uintptr_t>(i) * 4);
        if (!ref || ref == 0xFFFFFFFF) continue;
        const uintptr_t c = m.contract(ref);
        if (!c || !m.mapped(c)) continue;
        if (m.char_id(m.ptr(c + ::subject_contract::LIEGE)) != ruler_id) continue;
        const uint32_t vid = m.char_id(m.ptr(c + ::subject_contract::SUBJECT));
        if (!vid || vid == ruler_id) continue;
        out.push_back({c, vid});
    }
    return out;
}

//: The ruler's direct vassals: the subjects of its contracts, each once.
template <class Mem>
std::vector<uint32_t> direct_vassal_ids(const Mem& m, uintptr_t title_realm, uint32_t ruler_id) {
    std::vector<uint32_t> out;
    for (const LiegeContract& lc : liege_contracts(m, title_realm, ruler_id)) {
        bool seen = false;
        for (uint32_t x : out) seen = seen || x == lc.subject_id;
        if (!seen) out.push_back(lc.subject_id);
    }
    return out;
}

//: The ruler's contract with one vassal, or 0 (set_obligation_level, the
//: vassal's contract read).
template <class Mem>
uintptr_t contract_with(const Mem& m, uintptr_t title_realm, uint32_t ruler_id, uint32_t vassal_id) {
    vassal_id &= 0xFFFFFF;
    for (const LiegeContract& lc : liege_contracts(m, title_realm, ruler_id))
        if (lc.subject_id == vassal_id) return lc.contract;
    return 0;
}

}  // namespace bridge::vassal_roster
