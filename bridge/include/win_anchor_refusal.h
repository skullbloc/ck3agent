// win_anchor_refusal.h — a write whose engine call has no Windows anchor answers a typed
// refusal before its handler runs (ADR 0001).
//
// On Windows the generated ck3_offsets.h (re/scripts/windows/gen_win_offsets.py) gives an
// image address with no Windows counterpart the value CK3_WIN_UNMAPPED_N(i): top twelve
// bits 0x7FF, non-canonical, so a call through it FAULTS inside the handler's guarded
// region and the handler answers its own crash reply (`apply_crashed`, `sigsegv`, ...),
// which tells a seat the engine broke when the truth is that this build cannot make the
// call at all. Each row below names a write and the engine entry its main path calls
// unconditionally (read from the handler body; the seat-lever census).
// When that entry is unmapped, dispatch answers
//   {"success":false,"reason":"not_on_windows","missing_anchor":"<ns::NAME>",...}
// instead. The test is the VALUE, not the OS: on Linux no constant carries those bits,
// so every row reads mapped; on Windows a row lifts by itself the day its anchor is
// added to the generator's inputs (the generated header then carries a real RVA).
// A read whose closure only touches an unmapped address on a side branch is NOT listed:
// its handler degrades that field (and the parity plan grades it).
#pragma once

#include <cstdint>
#include <cstring>

#include "ck3_offsets.h"

namespace bridge::win_anchor {

//: base + this value is non-canonical: the generator's poison (CK3_WIN_UNMAPPED_N).
inline constexpr bool rva_unmapped(uintptr_t rva) { return (static_cast<unsigned long long>(rva) >> 52) == 0x7FFull; }

struct Need { const char* action; const char* anchor; uintptr_t rva; };

//: action -> the engine entry its main path calls (one row per entry; an action may repeat).
inline constexpr Need kNeeds[] = {
    {"send_decision",         "funcs::AI_TRY_DECISION_CORE",        ck3::funcs::AI_TRY_DECISION_CORE},
    {"set_lifestyle_focus",   "funcs::SET_FOCUS_APPLY",             ck3::funcs::SET_FOCUS_APPLY},
    {"set_education_focus",   "funcs::SET_FOCUS_APPLY",             ck3::funcs::SET_FOCUS_APPLY},
    {"set_council_task",      "funcs::COUNCIL_TASK_CMD_EXECUTE",    ck3::funcs::COUNCIL_TASK_CMD_EXECUTE},
    {"construction_check",    "funcs::ADD_CONSTRUCTION_VALIDITY",   ck3::funcs::ADD_CONSTRUCTION_VALIDITY},
    {"construct_building",    "funcs::ADD_CONSTRUCTION_EXECUTE",    ck3::funcs::ADD_CONSTRUCTION_EXECUTE},
    {"unlock_perk",           "add_lifestyle_perk_cmd::EXECUTE",    ck3::add_lifestyle_perk_cmd::EXECUTE},
    {"move_capital",          "funcs::CAPITAL_RELOCATE",            ck3::funcs::CAPITAL_RELOCATE},
    {"appoint_councillor",    "funcs::SWAP_COUNCIL_RAW_LEAF",       ck3::funcs::SWAP_COUNCIL_RAW_LEAF},
    // maa_recruit and hire_mercenary build their command from its vtables on Windows
    {"maa_recruit",           "funcs::CREATE_MAA_REGIMENT_CMD_VTABLE", ck3::funcs::CREATE_MAA_REGIMENT_CMD_VTABLE},
    {"maa_station",           "funcs::MAA_STATION_APPLY_LEAF",      ck3::funcs::MAA_STATION_APPLY_LEAF},
    {"maa_unstation",         "funcs::MAA_UNSTATION_APPLY_LEAF",    ck3::funcs::MAA_UNSTATION_APPLY_LEAF},
    {"hire_mercenary",        "globals::HIRE_MERC_CMD_VTABLE_PRIMARY", ck3::globals::HIRE_MERC_CMD_VTABLE_PRIMARY},
    {"set_character_faith",   "funcs::SET_FAITH",                   ck3::funcs::SET_FAITH},
    {"set_character_culture", "funcs::SET_CULTURE",                 ck3::funcs::SET_CULTURE},
    {"set_title_law",         "funcs::ADD_TITLE_LAW_COMMAND_CTOR",  ck3::funcs::ADD_TITLE_LAW_COMMAND_CTOR},
    // The costed command path (crown/tribal authority, succession) is built on Windows
    // from the command's vtables; the uncosted setter (ADD_REALM_LAW) refuses inside the handler.
    {"change_realm_law",      "funcs::ADD_LAW_CMD_VTABLE",          ck3::funcs::ADD_LAW_CMD_VTABLE},
};

//: set_ai_veto kinds whose command class is matched by a vtable with no Windows
//: anchor. With the vtable poisoned a `set` would answer success and never match a post
//: (an inert rule); it answers not_on_windows instead. Keyed by ai_veto::what_name.
struct KindNeed { const char* what; const char* anchor; uintptr_t rva; };
inline constexpr KindNeed kVetoKindNeeds[] = {
    {"legacy",  "funcs::ADD_DYNASTY_PERK_CMD_VTABLE",       ck3::funcs::ADD_DYNASTY_PERK_CMD_VTABLE},
    {"faction", "faction_write::CREATE_FACTION_CMD_VTABLE", ck3::faction_write::CREATE_FACTION_CMD_VTABLE},
    {"faction", "faction_write::JOIN_FACTION_CMD_VTABLE",   ck3::faction_write::JOIN_FACTION_CMD_VTABLE},
    {"activity", "start_activity_cmd::VTABLE",              ck3::start_activity_cmd::VTABLE},
};

//: The first unmapped anchor the veto kind needs, or nullptr (every Linux kind).
inline const KindNeed* veto_kind_missing(const char* what) {
    if (!what) return nullptr;
    for (const KindNeed& n : kVetoKindNeeds)
        if (std::strcmp(n.what, what) == 0 && rva_unmapped(n.rva)) return &n;
    return nullptr;
}

//: The first unmapped entry the action needs, or nullptr (every Linux call; every
//: Windows action not listed or whose entries are all anchored).
inline const Need* missing_for(const char* action) {
    if (!action) return nullptr;
    for (const Need& n : kNeeds)
        if (std::strcmp(n.action, action) == 0 && rva_unmapped(n.rva)) return &n;
    return nullptr;
}

}  // namespace bridge::win_anchor
