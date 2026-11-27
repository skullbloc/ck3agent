// A title's holder history entry (CTitleHolderHistoryEntry, re/offsets title_obj::THHE_*), decoded.
//
// The packed date is (year<<16)|(month<<8)|day with month and day ZERO-based: a seated game's 1096 save (read live)
// reads 1075.00.11 for Ulster won on 1075.01.12, 1073.08.07 for Oriel won 1073.09.08, 1083.08.07 for Ailech
// (1083.09.08), 1084.07.11 for Ossory (1084.08.12) against the bridge's own war history; the decode does not print
// them a month and a day early.
//
// The type, by the causes read beside each in that save (MEDIUM: one save, re/findings/re_title_history_type.md):
// 0 conquest (three ducal conquests won), 2 conquest by a claimant (the Oriel claim war), 4 inheritance (every death
// in the line; the history files' holders too: was printed "grant"), 6 destroyed (no holder), 7 created (a duchy),
// 10 revoked (Ormond after refused_liege_demand_war 1072; Dublin and Leinster to the dead liege's sons 1091).
#pragma once
#include <cstdint>

namespace title_history {

struct Date { int y, m, d; };

inline Date decode(uint32_t packed) {
    return {static_cast<int>((packed >> 16) & 0xFFFF), static_cast<int>((packed >> 8) & 0xFF) + 1,
            static_cast<int>(packed & 0xFF) + 1};
}

// A "Y.M.D" cutoff (1-based, as every reply prints dates) in the entry's packing; month/day 0 mean "from its start".
inline uint32_t encode(int y, int m, int d) {
    const int mm = m > 0 ? m - 1 : 0, dd = d > 0 ? d - 1 : 0;
    return (static_cast<uint32_t>(y) << 16) | ((static_cast<uint32_t>(mm) & 0xFF) << 8) | (static_cast<uint32_t>(dd) & 0xFF);
}

inline const char* type_name(uint32_t t) {
    switch (t) {
    case 0: return "conquest";
    case 2: return "conquest_claim";
    case 4: return "inheritance";
    case 6: return "destroyed";
    case 7: return "created";
    case 10: return "revoked";
    case 20: return "succession";   // the earlier reading, not seen in the 1096 save
    default: return "unknown";
    }
}

}  // namespace title_history
