// family_ids.h — a parent id read from the family sub-struct, and the relation two characters share.
//
// THE HAZARD (a Carinthia test run, 1066.09.15): `inspect character` gave relation "sibling", is_kin true, for
// Markward 31010 (father 25223) and Kuno 33202 (father 28100) towards Berthold 30331 (father 28063): three
// fathers, and no mother read for any of them (the reply's mother_id null). The relation block masked each raw
// id with 0xFFFFFF before comparing; a missing parent is 0xFFFFFFFF (family::FATHER_ID / MOTHER_ID, "0xFFFFFFFF
// = none"), which the mask turns into 0xFFFFFF, a non-zero id both "share". Every character with no mother read
// was the ruler's sibling. The reply's own father_id / mother_id test the raw 0xFFFFFFFF first (null); this is
// that rule, for every reader.
#pragma once

#include <cstdint>

namespace bridge::family_ids {

constexpr uint32_t NONE_RAW = 0xFFFFFFFFu;

// The character id a raw family slot names, or 0 for none (0xFFFFFFFF, or 0).
inline uint32_t parent_id(uint32_t raw) { return raw == NONE_RAW ? 0u : (raw & 0xFFFFFFu); }

// Two characters are siblings when a parent each names is the same character (half siblings included). The
// ids are parent_id() values: 0 is "none" and never matches.
inline bool siblings(uint32_t a_father, uint32_t a_mother, uint32_t b_father, uint32_t b_mother) {
    return (a_father != 0 && a_father == b_father) || (a_mother != 0 && a_mother == b_mother);
}

}  // namespace bridge::family_ids
