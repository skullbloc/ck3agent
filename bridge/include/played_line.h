// played_line.h — WHO THE LINE IS NOW: the two decisions the heir handoff makes,
// as pure functions over values (METERLINE, owner ruling 2026-11-11).
//
// ONE DEFINITION, TWO CALLERS, BOTH IN THE BRIDGE. `bridge_init.cpp`
// `check_controlled_handoff` follows a control session's character through
// succession: while he lives it caches his PRIMARY title (the highest-tier title
// he holds), and when he dies the session moves to that title's CURRENT HOLDER,
// unless the title reads no holder or still names the dead man (then it retries
// next tick). `check_line_watches` follows the experiment's PLAYED LINE by the
// same two decisions (line_watch.h `successor`, `tick`), per tick, with no seat
// and no session; the growth meter READS that decision (query_character
// `played_line`) rather than re-deriving it. The case table
// `scripts/fixtures/played_line_vectors.tsv` pins the two decisions
// (`bridge/test/test_played_line.cpp`, ctest).
//
// Pure: no engine read, no allocation. The callers do the memory reads (title
// lookups, tiers, holder ids) and hand the values in.
#pragma once

#include <cstddef>
#include <cstdint>

namespace played_line {

constexpr uint32_t kIdMask = 0xFFFFFFu;

// One held title as the scan sees it, in held-array order. A title whose object
// did not look up is handed in with `tier < 0` and is skipped, exactly as
// highest_tier_held_title skipped it (`continue` on a null / unmapped object).
struct HeldTitle {
    uint32_t title_id;   // masked id; 0 = an empty slot (skipped)
    int tier;            // 1 barony .. 5 empire; < 0 = unreadable (skipped)
};

// The PRIMARY title: the highest tier held; among equal tiers the FIRST in held
// order (a later title must be STRICTLY higher to replace it). 0 when nothing
// readable is held. This is the title the handoff caches while the character
// lives (session->primary_title_id), refreshed only when non-zero.
inline uint32_t pick_primary(const HeldTitle* titles, std::size_t n) {
    int best_tier = -1;
    uint32_t best = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const uint32_t tid = titles[i].title_id & kIdMask;
        if (tid == 0) continue;
        if (titles[i].tier < 0) continue;
        if (titles[i].tier <= best_tier) continue;
        best_tier = titles[i].tier;
        best = tid;
    }
    return best;
}

// The HEIR, from a read of the dead character's cached primary title: its
// current holder, masked. 0 (no heir YET or at all) when the title has no
// holder (0 or the 0xFFFFFF sentinel) or still names the dead character. The
// handoff treats 0 as "retry next tick"; the meter tells the two 0s apart by
// whether a holder was read at all (`scripts/played_line.py`).
inline uint32_t heir_from_holder(uint32_t raw_holder, uint32_t dead_id) {
    const uint32_t holder = raw_holder & kIdMask;
    if (holder == 0 || holder == kIdMask) return 0;
    if (holder == dead_id) return 0;
    return holder;
}

// THE REALM GUARD (owner ruling 2026-11-12; v3reg2 s3 D1: the handoff moved a
// duke's seat onto Carloman, an independent rival king). A succession must never
// move a seat onto a ruler OUTSIDE the seat's registered realm.
//   root            the seat's registered realm root: the top liege of the
//                   seat's ruler as first read alive by the handoff (latched;
//                   a K seat's root is its own ruler and follows its own line).
//                   0 = nothing registered: no guard (unchanged behaviour).
//   dead            the seat's ruler who died
//   target          the character the succession would bind
//   root_now        who heads the registered realm NOW: `root` while he lives,
//                   else the played line's character from `root` (line_watch.h);
//                   0 when that cannot be read (not armed, pending, ended)
//   target_top      the target's top liege (liege_walk.h; 0: the walk failed)
// InRealm: bind. OutsideRealm: REFUSE and record (an observed fact). Unresolved:
// a reading is missing; retry next tick, never bind and never unbind on it.
enum class RealmCheck { InRealm, OutsideRealm, Unresolved };

inline const char* realm_check_name(RealmCheck c) {
    switch (c) {
        case RealmCheck::InRealm: return "in_realm";
        case RealmCheck::OutsideRealm: return "heir_outside_realm";
        default: return "realm_unresolved";
    }
}

inline RealmCheck succession_realm_check(uint32_t root, uint32_t dead, uint32_t target,
                                         uint32_t root_now, uint32_t target_top) {
    root &= kIdMask; dead &= kIdMask; target &= kIdMask;
    root_now &= kIdMask; target_top &= kIdMask;
    if (root == 0) return RealmCheck::InRealm;          // nothing registered
    if (root == dead) return RealmCheck::InRealm;       // the seat IS the realm's line
    if (root_now == 0 || root_now == kIdMask) return RealmCheck::Unresolved;
    if (target == root_now) return RealmCheck::InRealm;
    if (target_top == 0 || target_top == kIdMask) return RealmCheck::Unresolved;
    return target_top == root_now ? RealmCheck::InRealm : RealmCheck::OutsideRealm;
}

}  // namespace played_line
