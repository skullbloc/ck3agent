// lifestyle_points.h — a ruler's unspent lifestyle perk points and which perks his
// prerequisites admit (re/findings/re_lifestyle_perks.md §2-§3). Engine-free: the caller reads
// core_data's XP entries (core+0x208, count +0x214: {SLifestyle*, int64 fp unspent XP}), the
// lifestyle's XP per point (SLifestyle+0x130) and the perk's parents (CLifestylePerk+0x308,
// count +0x314); this file says what the game computes from them.
//
//   points: FUN_067f44e0(char, lifestyle) = (int)(xp / 100000) / per_point, C division both
//           times (the decompile's 0x29f16b11c6d1e109 multiply-shift is a signed /100000).
//   parents: the perk's can-pick FUN_06b81250(perk, char, why) refuses a perk already held,
//           then requires EVERY parent held, then runs the perk's own trigger (perk+0x70, not
//           read here: the game's check at unlock_perk decides it).
// The command's CanExecute (FUN_067ca090) also needs an active focus (any lifestyle's) and
// >= 1 point in the perk's own lifestyle.
#pragma once
#include <cstdint>

namespace bridge::lifestyle_points {

inline constexpr int64_t kFp = 100000;

//: Unspent points in one lifestyle, as the game counts them. 0 for an unread per-point value.
inline int points(int64_t xp_fp, int32_t per_point) {
    if (per_point <= 0) return 0;
    return static_cast<int>(xp_fp / kFp) / per_point;
}

//: XP still needed for the next point (whole XP), or -1 for an unread per-point value.
inline int64_t xp_to_next(int64_t xp_fp, int32_t per_point) {
    if (per_point <= 0) return -1;
    const int64_t whole = xp_fp / kFp;
    const int64_t have = whole < 0 ? 0 : whole % per_point;
    return per_point - have;
}

enum class Pick : uint8_t { kHeld = 0, kParentMissing = 1, kByParents = 2 };
inline const char* pick_name(Pick p) {
    return p == Pick::kHeld ? "held" : p == Pick::kParentMissing ? "parent_missing" : "parents_held";
}

//: What the perk's prerequisites say: held already, a parent missing, or every parent held
//: (the perk's own trigger then decides, in the game's check).
inline Pick by_parents(uint64_t perk, const uint64_t* parents, int n_parents, const uint64_t* held, int n_held) {
    auto has = [&](uint64_t p) {
        for (int i = 0; i < n_held; ++i)
            if (held[i] == p) return true;
        return false;
    };
    if (has(perk)) return Pick::kHeld;
    for (int i = 0; i < n_parents; ++i)
        if (!has(parents[i])) return Pick::kParentMissing;
    return Pick::kByParents;
}

//: The unlock verdict from the bridge's reads, in the order the game's CanExecute tests
//: (FUN_067ca090): a focus, a point in the perk's lifestyle, then the perk's can-pick. nullptr =
//: the reads admit it (the game's own check still runs: the perk's trigger).
inline const char* refusal(bool focus_set, int points_in_lifestyle, Pick pick) {
    if (!focus_set) return "no_lifestyle_focus";            // SELECT_PERK_NO_LIFESTYLE_FOCUS
    if (points_in_lifestyle < 1) return "perk_points_needed"; // PERK_POINTS_NEEDED
    if (pick == Pick::kHeld) return "perk_already_held";
    if (pick == Pick::kParentMissing) return "parent_perk_missing";
    return nullptr;
}

}  // namespace bridge::lifestyle_points
