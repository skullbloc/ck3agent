// faction_war.h — a faction's war, as the game itself reads it (cleanup 2026.11, F16).
//
// The game's "faction is in war" check, CFactionPressDemandCommand::CanExecute
// (CAN_FIRE_ULTIMATUM, Ghidra FUN_05f8ca90, the source of the press refusal
// FACTION_WINDOW_PRESS_DEMANDS_TT_CANT_WAR "Cannot press demands while the faction
// is in war"), and the faction tick (FactionTick, FUN_05f8c910, which sets
// WAR_INACTIVE_FLAG when it fails) both read the war the same way:
//
//   ref  = *(uint32*)(faction + WAR_REF)              // the war's full handle
//   war  = WAR_DB slot[ref & 0xFFFFFF]                 // null slot -> the null war
//   war  = (*(uint32*)(war + WAR_ID) == ref) ? war : the null war   // the whole
//          handle, generation byte included: a reused slot is another war
//   at war = war->IsValid()                            // CWar: WAR_ID != 0xFFFFFFFF;
//                                                      // the null war: false
//
// faction_v2::AT_WAR_FLAG (+0xC1) is not this: it is the tick's copy of +0xB9, the
// "a player is concerned" flag (FUN_05fa42a0), and read false for faction 95 at war
// (w-l10-factions-5, war 51). re_factions.md section 8.
//
// The war id given out is the war's own WAR_ID, the full handle every war read
// gives (query_wars, query_war_*, the observation), so a faction row's war_id can be
// passed to the war intents as it is. Masking to the slot index happens only here,
// where the engine indexes its table.
//
// Header-only and engine-free: the WAR_DB reads are passed in, so the rule is tested
// on a host (bridge/test/test_faction_war.cpp).
#pragma once

#include <cstdint>

namespace bridge {

struct FactionWar {
    bool at_war = false;
    uint32_t war_id = 0;     // the war's full handle (its WAR_ID); 0 when not at war
};

constexpr uint32_t kFactionWarNone = 0xFFFFFFFFu;   // war_db::WAR_ID_INVALID
constexpr uint32_t kFactionWarSlotMask = 0x00FFFFFFu; // war_db::WAR_ID_IDX_MASK

// slot_war(index) -> the CWar* in WAR_DB's slot (0 when the slot is empty or out of
// range); war_id_of(war) -> *(uint32*)(war + war_obj::WAR_ID).
template <class SlotWar, class WarIdOf>
FactionWar faction_war_from(uint32_t war_ref, SlotWar slot_war, WarIdOf war_id_of) {
    FactionWar out;
    if (war_ref == kFactionWarNone) return out;
    uintptr_t war = slot_war(war_ref & kFactionWarSlotMask);
    if (!war) return out;
    uint32_t id = war_id_of(war);
    if (id != war_ref || id == kFactionWarNone) return out;
    out.at_war = true;
    out.war_id = id;
    return out;
}

}  // namespace bridge
