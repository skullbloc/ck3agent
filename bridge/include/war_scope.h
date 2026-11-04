// war_scope.h — the optional `war_id` scope of a war read, in ONE place.
//
// L20-A (smoke17 row A3): `query_war_stance {"character_id":12015}` listed
// `wars []` at 0867.03.01 while war 13, which 12015 declared, was active and the
// record walk matched 30 records. The handler read the scope as
//
//     uint32_t want_war = static_cast<uint32_t>(json_get_int(params, "war_id"));
//     ...
//     if (want_war != 0 && (want_war & 0xFFFFFF) != idx) continue;
//
// and json_get_int answers -1 for an ABSENT key (bridge/test/
// json_int_absence_probe.cpp pins that), so a call with no war_id carried the
// scope 0xFFFFFFFF: every war's slot differed from 0xFFFFFF and every row was
// dropped. query_war_score_breakdown read its scope the same way, so its
// documented `{}` ("all cached wars") took the single-war path for war
// 0xFFFFFFFF and answered nothing.
//
// The scope, as both handlers read it now:
//   absent, or the engine's none-ref (-1 == 0xFFFFFFFF)  -> kAllWars
//   0                                                     -> kAllWars (the
//       server's `war_id: int = 0` default means "no scope", redesign.py
//       inspect; war 0 is a real war, so a caller scoping to it alone cannot,
//       which is the pre-existing contract, unchanged here)
//   anything else -> that war: the full handle (generation byte and slot) or
//       the bare slot; only the low 24 bits select the slot.
// Header-only and engine-free: bridge/test/test_war_scope.cpp compiles it.
#ifndef CK3_BRIDGE_WAR_SCOPE_H
#define CK3_BRIDGE_WAR_SCOPE_H

#include <cstdint>

namespace bridge {
namespace war_scope {

constexpr uint32_t kAllWars = 0;

// `raw` is json_get_int(params, "war_id") as the handler received it.
inline uint32_t from_param(int raw) {
#ifdef WAR_SCOPE_PREFIX_CONTROL
    return static_cast<uint32_t>(raw);           // the pre-fix read (ctest control)
#else
    return raw == -1 ? kAllWars : static_cast<uint32_t>(raw);
#endif
}

// Does the war in WAR_DB slot `slot` fall inside `scope`?
inline bool in_scope(uint32_t scope, uint32_t slot) {
    return scope == kAllWars || (scope & 0xFFFFFFu) == (slot & 0xFFFFFFu);
}

// query_war_score_breakdown's branch: every cached war, or the one war `scope`
// names (witness r1 item 8: the documented `{}` had no test).
inline bool all_wars(uint32_t scope) {
    return scope == kAllWars;
}

// query_war_stance's row rule: an active war in scope whose PRIMARY attacker is
// the queried character (only the primary attacker decides the siege).
inline bool stance_row_selected(uint32_t scope, uint32_t slot,
                                uint32_t primary_attacker, uint32_t character_id) {
    return in_scope(scope, slot) &&
           (primary_attacker & 0xFFFFFFu) == (character_id & 0xFFFFFFu);
}

}  // namespace war_scope
}  // namespace bridge

#endif  // CK3_BRIDGE_WAR_SCOPE_H
