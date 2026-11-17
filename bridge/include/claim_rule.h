// include/claim_rule.h — lane integ11: when a claim may carry a claim war.
//
// THE EVIDENCE (docs/cleanup-2026.11/evidence/v4integ11, live run i11s3, the peace
// lane's row 4): script_action declare_war claim_cb by 35156 on his OWN UNPRESSED
// claim on c_berestye (title 3401; query_claims read pressed:false, block_reason
// claim_not_pressed) — the game's own guard, can_declare_war, RAN and the war
// started. The bridge's reads and its native write path culled exactly that case.
// In the game an unpressed claim is pressed by its holder only; pressing ANOTHER
// character's claim (a courtier's) needs it pressed. One definition for every read
// that culls a claim row and for the native write path.
#pragma once

namespace claim_rule {

// HAS_CLAIM: the claimant holds a claim on the title at all; PRESSED: that claim
// is pressed; CLAIMANT_IS_ACTOR: the war is declared by the claimant himself.
inline bool claim_usable(bool has_claim, bool pressed, bool claimant_is_actor) {
    return has_claim && (pressed || claimant_is_actor);
}

}  // namespace claim_rule
