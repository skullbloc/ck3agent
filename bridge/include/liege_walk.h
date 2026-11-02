// liege_walk.h — the independent ruler at the head of a liege chain (L15 A2).
//
// query_handlers.cpp's get_top_realm_ruler_id walks get_liege_id_from_addr hop
// by hop until a ruler with no liege is reached; that ruler is the top liege
// (an independent character walks to its OWN id). The walk used to return the
// id it had reached so far when it could not go on (a liege it could not look
// up, a liege loop, 30 hops): from the first hop that is the character's own id,
// so a VASSAL whose liege did not resolve read as INDEPENDENT (top == self), and
// from a later hop an intermediate liege read as the top.
//
// Now a walk that cannot finish answers 0, which every reader takes as "the walk
// could not be made" (query_character top_liege_id, query_dejure_progress's
// verbose rows, tools.shield_decision_why_blocked, redesign's observe reach).
// Engine-free so a host test reaches it (bridge/test/test_liege_walk.cpp).
#pragma once

#include <cstdint>
#include <set>

namespace liege_walk {

//: The result for a walk that could not be made.
constexpr uint32_t kUnresolved = 0;
//: The hop cap (CK3's liege chains are a handful deep).
constexpr int kMaxDepth = 30;

// `self_id`: the start character's id (24-bit). `start`: its handle.
// `liege_of(handle)`: its liege's id, 0 when it has none (independent).
// `lookup(id)`: the liege's handle, or a null-equivalent (`!handle`) when the
// id does not resolve to a character.
template <class Handle, class LiegeOf, class Lookup>
uint32_t top_liege(uint32_t self_id, Handle start, LiegeOf liege_of, Lookup lookup) {
    uint32_t current_id = self_id & 0xFFFFFF;
    Handle current = start;
    std::set<uint32_t> seen{current_id};
    for (int depth = 0; depth < kMaxDepth; depth++) {
        uint32_t liege_id = liege_of(current) & 0xFFFFFF;
        if (liege_id == 0 || liege_id == current_id) return current_id;  // no liege: the top
        if (seen.count(liege_id)) return kUnresolved;                    // a liege loop
        seen.insert(liege_id);
        Handle next = lookup(liege_id);
        if (!next) return kUnresolved;                                   // liege unresolvable
        current_id = liege_id;
        current = next;
    }
    return kUnresolved;                                                  // deeper than the cap
}

}  // namespace liege_walk
