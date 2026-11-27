// include/engine_id.h — the id a game function compares exactly.
//
// A database object's id is a slot index in the low 24 bits and a generation byte in the
// high 8 (re/offsets/ck3_offsets.h: the war candidate entries, "high 8 bits: generation/type
// tag (must match char struct +0x18)"; COUNCIL_SEAT_ASSIGN, "the non-zero generation byte in
// bits 24-31"). A slot freed and used again gets a new generation, so a character born during
// the game usually carries a non-zero top byte (e.g. 0x01006dd0 for slot 0x6dd0).
//
// The bridge's own lookups compare the low 24 bits (dispatch_router.cpp lookup_character,
// ad_db_lookup), and its replies carry the low 24 bits. The game's command and effect
// functions resolve an id by slot AND compare the whole stored id with the one they were
// given, falling to the null sentinel when the two differ (decompiles: the mercenary hire's
// CanExecute/Execute, the men-at-arms recruit's Execute, the raid toggle's CanExecute, the
// council seat swap, the war creation). So an id handed to such a function is the whole id
// read off the object, never the low 24 bits.
#pragma once
#include <cstdint>

namespace engine_id {

constexpr uint32_t kSlotMask = 0x00FFFFFFu;
constexpr uint32_t kNone     = 0xFFFFFFFFu;

inline uint32_t slot(uint32_t id) { return id & kSlotMask; }

// The whole id for `asked` (a full id or its low 24 bits), given `stored`, the id read off the
// object the bridge resolved for it: `stored` when it names the same slot, else `asked`
// unchanged (nothing resolved: the game refuses it, no id is made up).
inline uint32_t full_or(uint32_t asked, uint32_t stored) {
    if (asked == kNone || stored == kNone || stored == 0) return asked;
    return slot(stored) == slot(asked) ? stored : asked;
}

}  // namespace engine_id
