// maa_currency.h — the currency a ruler's government buys men-at-arms with.
//
// WHY. A tribal ruler buys a regiment with PRESTIGE, not gold: the game's government type carries
// `regiments_prestige_as_gold = yes` ("Is this government type using prestige to buy and reinforce MaA
// Regiments? (maintenance still costs gold)", common/governments/_governments.info) and the define
// MAA_PRESTIGE_BUY_COST_MULT = 2 ("If using prestige as gold for MaA, multiply buy cost by this",
// common/defines/00_defines.txt). The bridge's recruit gate and listing priced every ruler in gold, so a
// tribal ruler with gold and no prestige was offered regiments the game would not sell him, and one with
// prestige and no gold was refused `insufficient_gold`.
//
// THE RULE. The government type's key (title_realm GOVERNMENT_TYPE_PTR -> name, HIGH, the read every
// government row makes) against kPrestigeGovernments, the government types of 1.18.3.1 whose rules set
// regiments_prestige_as_gold = yes (00_government_types.txt: tribal_government only). The government's own
// rule flag is not read from memory (its offset in CGovernmentType is not found); the list is the files',
// and ck3agent/server/test_declaration_price_and_strength.py checks it and the multiplier against the install. A key not read is
// `Unread`: the caller says so, never prices it in gold. Header-only and engine-free.
#pragma once

#include <string_view>

namespace bridge::maa_currency {

//: The government types whose regiments are bought with prestige (regiments_prestige_as_gold = yes).
inline constexpr const char* kPrestigeGovernments[] = {"tribal_government"};
//: MAA_PRESTIGE_BUY_COST_MULT: the buy cost in gold times this is the price in prestige.
inline constexpr double kPrestigeBuyCostMult = 2.0;

enum class Currency { Gold, Prestige, Unread };

inline Currency of_government(std::string_view key) {
    if (key.empty()) return Currency::Unread;
    for (const char* g : kPrestigeGovernments)
        if (key == g) return Currency::Prestige;
    return Currency::Gold;
}

inline const char* name(Currency c) {
    return c == Currency::Prestige ? "prestige" : c == Currency::Gold ? "gold" : "unread";
}

//: The recruit price in the ruler's currency, from the type's buy cost in gold (fixed point or not).
inline double price(double gold_cost, Currency c) {
    return c == Currency::Prestige ? gold_cost * kPrestigeBuyCostMult : gold_cost;
}

}  // namespace bridge::maa_currency
