// include/claim_rule.h — when a claim may carry a claim war.
//
// THE EVIDENCE (a live run of the peace
// checks, row 4): script_action declare_war claim_cb by 35156 on his OWN UNPRESSED
// claim on c_berestye (title 3401; query_claims read pressed:false, block_reason
// claim_not_pressed) — the game's own guard, can_declare_war, RAN and the war
// started. The bridge's reads and its native write path culled exactly that case.
// In the game an unpressed claim is pressed by its holder only; pressing ANOTHER
// character's claim (a courtier's) needs it pressed. One definition for every read
// that culls a claim row and for the native write path.
#pragma once

#include <cstdint>
#include <set>

namespace claim_rule {

// HAS_CLAIM: the claimant holds a claim on the title at all; PRESSED: that claim
// is pressed; CLAIMANT_IS_ACTOR: the war is declared by the claimant himself.
inline bool claim_usable(bool has_claim, bool pressed, bool claimant_is_actor) {
    return has_claim && (pressed || claimant_is_actor);
}

// A claim war is fought against the realm the
// claimed title lies in. claim_cb's should_invalidate
// (game/common/casus_belli_types/00_claim.txt) keeps the war while a target title's
// holder `= scope:defender` OR `target_is_liege_or_above = scope:defender`. The reads
// matched the claimed title's holder against the defender only, so Donnchad's own
// claim on c_athlone (held by Conchobar 29585, a VASSAL of 68530) read
// claim_not_pressed against 68530 while the game's check passed the war.
// True when `defender_id` is the holder or a liege above him: the holder's chain is
// walked with `liege_of(handle)` (0 when independent) and `lookup(id)` (a null-like
// handle when the id does not resolve); a loop, an unresolvable liege or more than
// 30 hops ends the walk without a match. Engine-free so a host test reaches it.
template <class Handle, class LiegeOf, class Lookup>
bool title_in_defender_realm(uint32_t holder_id, Handle holder, uint32_t defender_id,
                             LiegeOf liege_of, Lookup lookup) {
    uint32_t cur_id = holder_id & 0xFFFFFF;
    defender_id &= 0xFFFFFF;
    if (cur_id == 0 || defender_id == 0) return false;
    if (cur_id == defender_id) return true;
    Handle cur = holder;
    std::set<uint32_t> seen{cur_id};
    for (int depth = 0; depth < 30 && cur; depth++) {
        uint32_t liege = liege_of(cur) & 0xFFFFFF;
        if (liege == 0 || liege == 0xFFFFFF || liege == cur_id || seen.count(liege)) return false;
        if (liege == defender_id) return true;
        seen.insert(liege);
        cur = lookup(liege);
        cur_id = liege;
    }
    return false;
}

// Whom a claim war on a title held by `holder_id` is declared against, for
// `attacker_id` whose lieges (all of them up to his top liege) are `attacker_lieges`: the holder's chain is climbed until a character whose liege is
// none or is one of the attacker's lieges (the attacker's realm meets it there). An
// independent attacker (`attacker_lieges` empty) therefore gets the holder's top
// liege; the holder himself when he is independent. 0 when no war is declared: the holder is the attacker or
// sits under him (his own vassal: claim_cb's "claimant_titles_held_by_attacker_vassals"),
// or the walk cannot be finished (a loop, an unresolvable liege, 30 hops).
template <class Handle, class LiegeOf, class Lookup>
uint32_t claim_war_defender(uint32_t holder_id, Handle holder, uint32_t attacker_id,
                            const std::set<uint32_t>& attacker_lieges,
                            LiegeOf liege_of, Lookup lookup) {
    uint32_t cur_id = holder_id & 0xFFFFFF;
    attacker_id &= 0xFFFFFF;
    if (cur_id == 0 || attacker_id == 0 || cur_id == attacker_id) return 0;
    if (attacker_lieges.count(cur_id)) return cur_id;   // the attacker's own liege holds it
    Handle cur = holder;
    std::set<uint32_t> seen{cur_id};
    for (int depth = 0; depth < 30 && cur; depth++) {
        uint32_t liege = liege_of(cur) & 0xFFFFFF;
        if (liege == attacker_id) return 0;                                      // his own vassal
        if (liege == 0 || liege == 0xFFFFFF || liege == cur_id) return cur_id;   // independent
        if (attacker_lieges.count(liege)) return cur_id;                        // realms meet
        if (seen.count(liege)) return 0;
        seen.insert(liege);
        cur = lookup(liege);
        cur_id = liege;
    }
    return 0;
}

}  // namespace claim_rule
