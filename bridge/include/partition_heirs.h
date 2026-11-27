// partition_heirs.h — who shares a partition succession, engine-free.
//
// The game divides partition titles "amongst their eligible children, with the player heir always
// being given the primary title" (localization/english/succession_laws_l_english.yml:7,
// partition_base_desc). do_query_succession_simulation does not share the land round-robin among the
// first entries of the PRIMARY TITLE's line of succession, which runs depth-first through the whole
// family: grandchildren, siblings, cousins. Recorded misses (re/findings/re_succession_survival.md
// P3): a test run named the granddaughter 15819 for eleven years while the son 37528 took two counties;
// A test run named the ruler's brother 32805.
//
// The heirs of a partition here: the line's head (the player heir, the game's own), then every
// later entry of the line who is a CHILD of the late ruler, filtered by the realm's gender law
// (male preference: daughters only when no son of the line is a child). The line already leaves out
// whoever an inheritance_blocker trait or the gender law excludes from inheriting at all.
//
// Basis values (the reply's `partition_heir_basis`):
//   children_in_line      at least one child of the ruler besides the player heir is in the line
//   no_child_in_line      none is: the land goes to the player heir here; whether the game then
//                         shares it with others of the line (siblings) is NOT decided (the server
//                         says unknown)
#pragma once

#include <cstdint>
#include <vector>

namespace bridge::partition_heirs {

// EffSuccRule::gender values (query_handlers.cpp decode_gender_law)
enum Gender : uint8_t { MALE_ONLY = 0, MALE_PREFERENCE = 1, EQUAL = 2, FEMALE_PREFERENCE = 3,
                        FEMALE_ONLY = 4, UNSET = 5 };

enum class Basis { children_in_line, no_child_in_line };

struct Result {
    std::vector<uint32_t> heirs;      // player heir first
    std::vector<uint32_t> excluded;   // line entries that take no partition share (in line order)
    Basis basis = Basis::no_child_in_line;
};

inline const char* basis_name(Basis b) {
    return b == Basis::children_in_line ? "children_in_line" : "no_child_in_line";
}

// line: the primary title's line, position 1 first. is_child(id): a child of the late ruler.
// is_female(id): the character's sex. gender: the primary title's effective gender law.
template <class IsChild, class IsFemale>
Result select(const std::vector<uint32_t>& line, IsChild is_child, IsFemale is_female, uint8_t gender) {
    Result r;
    if (line.empty()) return r;
    r.heirs.push_back(line[0]);
    std::vector<uint32_t> sons, daughters;
    for (size_t i = 1; i < line.size(); i++) {
        const uint32_t h = line[i];
        if (h == line[0]) continue;
        if (!is_child(h)) { r.excluded.push_back(h); continue; }
        (is_female(h) ? daughters : sons).push_back(h);
    }
    std::vector<uint32_t> keep;
    switch (gender) {
        case MALE_ONLY: keep = sons; break;
        case FEMALE_ONLY: keep = daughters; break;
        case MALE_PREFERENCE: keep = !sons.empty() ? sons : daughters; break;
        case FEMALE_PREFERENCE: keep = !daughters.empty() ? daughters : sons; break;
        default:  // equal, or unset: the line's own order
            for (size_t i = 1; i < line.size(); i++)
                if (line[i] != line[0] && is_child(line[i])) keep.push_back(line[i]);
            break;
    }
    for (uint32_t h : keep) r.heirs.push_back(h);
    for (uint32_t h : sons) {
        bool in = false; for (uint32_t k : keep) in = in || k == h;
        if (!in) r.excluded.push_back(h);
    }
    for (uint32_t h : daughters) {
        bool in = false; for (uint32_t k : keep) in = in || k == h;
        if (!in) r.excluded.push_back(h);
    }
    r.basis = keep.empty() ? Basis::no_child_in_line : Basis::children_in_line;
    return r;
}

}  // namespace bridge::partition_heirs
