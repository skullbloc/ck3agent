// dejure_bar.h — the de jure land a kingdom or empire creation needs (create_title's
// gate, intent_dispatch.cpp), as the game's own rule.
//
// The game's files (1.18.3.1, localization/english/game_concepts_l_english.yml):
// game_concept_duchy_desc and game_concept_kingdom_desc "controls at least half of
// its counties", game_concept_empire_desc "controls more than 80% of the counties
// within" (live 867: "You only have 89 of 94 required Counties" for e_france, 117
// counties = floor(0.8 x 117) + 1). The bridge used 51% from a kingdom up: a kingdom
// at exactly half (23 of 46) was refused here although the game's rule allows it.
// A tier the files give no text for (a hegemony, 6) keeps the old 51% bar.
// (every inspect title k_ireland read "need 7 of 14" beside the game's own "You only have 6 of 8
// required Counties"): the localisation's "at least half" is not the check. The game's own lines across the
// recorded runs: d_meath 2 counties -> 2 required, d_ulster 3 -> 2, d_don_valley 4 -> 3, d_moldova 6 -> 4,
// k_ireland 14 -> 8, e_france 117 -> 94: MORE than half for a duchy and a kingdom (floor(total / 2) + 1), more than
// 80% for an empire.
// Mirrored by ck3agent/server/redesign.py GAME_DEJURE_SHARE and MINT_DEJURE_MIN_FRAC
// (test_empire_land_rule / test_mint_price read these constants from this file).
// PURE. Host-tested by bridge/test/test_dejure_bar.cpp (built and run by ctest; not proven live).
#ifndef CK3_BRIDGE_DEJURE_BAR_H
#define CK3_BRIDGE_DEJURE_BAR_H

namespace bridge {
namespace dejure_bar {

constexpr int CREATE_TITLE_DEJURE_MIN_TIER     = 4;   // the gate runs from a kingdom up
constexpr int CREATE_TITLE_DEJURE_HALF_NUM     = 1;   // kingdom: held * DEN > total * NUM
constexpr int CREATE_TITLE_DEJURE_HALF_DEN     = 2;
constexpr int CREATE_TITLE_DEJURE_EMPIRE_TIER  = 5;
constexpr int CREATE_TITLE_DEJURE_EMPIRE_NUM   = 4;   // empire: held * DEN > total * NUM
constexpr int CREATE_TITLE_DEJURE_EMPIRE_DEN   = 5;
constexpr int CREATE_TITLE_DEJURE_OTHER_PCT    = 51;  // a tier with no game text: held * 100 >= total * PCT

// True when `held` of `total` de jure counties does NOT meet the rule for `tier`.
// total <= 0 (a titular title) is the caller's own refusal, not decided here.
constexpr bool short_of(int tier, long long held, long long total) {
    if (tier < CREATE_TITLE_DEJURE_MIN_TIER || total <= 0) return false;
    if (tier == CREATE_TITLE_DEJURE_EMPIRE_TIER)
        return held * CREATE_TITLE_DEJURE_EMPIRE_DEN <= total * CREATE_TITLE_DEJURE_EMPIRE_NUM;
    if (tier < CREATE_TITLE_DEJURE_EMPIRE_TIER)
        return held * CREATE_TITLE_DEJURE_HALF_DEN <= total * CREATE_TITLE_DEJURE_HALF_NUM;
    return held * 100 < total * CREATE_TITLE_DEJURE_OTHER_PCT;
}

// The percentage the log line names for `tier`.
constexpr double required_pct(int tier) {
    return tier == CREATE_TITLE_DEJURE_EMPIRE_TIER ? 80.0
         : tier < CREATE_TITLE_DEJURE_EMPIRE_TIER ? 50.0
         : static_cast<double>(CREATE_TITLE_DEJURE_OTHER_PCT);
}

}  // namespace dejure_bar
}  // namespace bridge

#endif  // CK3_BRIDGE_DEJURE_BAR_H
