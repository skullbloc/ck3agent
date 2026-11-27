// armies_around.h — other rulers' armies where the game looks for them (engine-free part).
//
// The game refuses an army disband with CANNOT_DISBAND_ENEMY_OR_HOSTILE_ARMIES_AROUND when a unit of a ruler
// hostile to the army's owner stands in the army's county or in a county next to it (ArmyDisbandCore, image
// 0x066891b0: the location province's county, province+0x428, and its neighbours, county+0x40 / +0x4C; each
// province's units, province+0x320 / +0x32C; the owner of each, owner link +0x16C). 1068.03-12: twelve
// disbands refused that way while no read named whose armies they were. army_disband's refusal now lists the
// other rulers' units there (army_orders.cpp emit_armies_around); query_armies lists other rulers' armies in
// the counties a caller names (`others_in_counties`, county title ids). Hostility is the game's own reading
// (FUN_066d55a0, not called here): a row is an army of another ruler, hostile or not.
// PURE. Host-tested by bridge/test/test_armies_around.cpp (built and run by ctest; not proven live).
#ifndef CK3_BRIDGE_ARMIES_AROUND_H
#define CK3_BRIDGE_ARMIES_AROUND_H

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

namespace bridge {
namespace armies_around {

constexpr int kRowsMax = 12;          // rows emitted (the rest counted in *_total)
constexpr int kCountiesMax = 64;      // the county and its neighbours walked; and ids a caller may name
constexpr int kProvincesMax = 64;     // provinces of one county
constexpr int kUnitsMax = 200;        // units on one province

struct Row {
    uint32_t army_id = 0;             // 0 when the unit resolves to no army (a fleet or an unread link)
    uint32_t owner_id = 0;
    int32_t men = -1;                 // -1: unread
    uint32_t province_id = 0;
    uint32_t county_id = 0;           // the county title id of the province (0: unread)
};

// "123,456" -> {123, 456}. False (and *out untouched) for anything else: an empty item, a sign, a
// non-digit, a zero, more than kCountiesMax ids, a value over 24 bits (a title id is masked to 24 bits).
inline bool parse_ids(std::string_view s, std::vector<uint32_t>* out) {
    std::vector<uint32_t> ids;
    if (s.empty()) return false;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string_view::npos) j = s.size();
        std::string_view item = s.substr(i, j - i);
        if (item.empty() || item.size() > 8) return false;
        uint32_t v = 0;
        for (char c : item) {
            if (c < '0' || c > '9') return false;
            v = v * 10 + static_cast<uint32_t>(c - '0');
        }
        if (v == 0 || v > 0xFFFFFFu) return false;
        ids.push_back(v);
        if (static_cast<int>(ids.size()) > kCountiesMax) return false;
        i = j + 1;
    }
    *out = std::move(ids);
    return true;
}

inline bool listed(const std::vector<uint32_t>& ids, uint32_t v) {
    return std::find(ids.begin(), ids.end(), v & 0xFFFFFFu) != ids.end();
}

// Another ruler's unit (the army's own owner and an unread owner are not rows).
inline bool foreign(uint32_t owner, uint32_t me) {
    return owner != 0 && owner != 0xFFFFFFFFu && (owner & 0xFFFFFFu) != (me & 0xFFFFFFu);
}

// Largest first (an unread size last), then by army id, so the cut keeps the stacks that matter.
inline void order(std::vector<Row>* rows) {
    std::stable_sort(rows->begin(), rows->end(), [](const Row& a, const Row& b) {
        if (a.men != b.men) return a.men > b.men;
        return a.army_id < b.army_id;
    });
}

}  // namespace armies_around
}  // namespace bridge

#endif
