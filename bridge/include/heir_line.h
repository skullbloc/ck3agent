// heir_line.h — the line of succession of a title, with the holder's own children first.
//
// THE HAZARD (a seated 1066 Lorraine game). Every succession read took the
// title's cached heir array (title+0x290 / +0x29C) as the line. On d_lower_lorraine that array was
// built when Gottfried 34471 inherited (1070.10.26, no children then): his sister Ida 33804 first,
// then her line. It did not change for nine years of reads (1074.09 to 1079.05, the same 20 entries):
// his daughter Virginia 45066 (1074) and his son Goffredo 69510 (1076) were never in it. At his death
// (1079.08.01) the game gave every title to Goffredo. The cache is the line as of its last rebuild,
// not the line the game will use at the death.
//
// Population check (the 1097 core, re_per_title_succession.md section 8): of 777 titles of duchy tier
// or higher whose holder has a living child, 23 have an EMPTY cache and 185 a cache whose first entry
// is not a living child of the holder. Children of another house ARE in caches (247 rows), so the
// cache does not leave out a child for his dynasty: a child missing from it was not born yet when it
// was built.
//
// The rule here (traversal "children" first; the game's line runs through the holder's children
// before his siblings: succession_laws_l_english.yml, and the game's own grant at Goffredo's
// father's death): among the holder's living children who are not barred from inheriting (an
// inheritance_blocker trait) and whom the title's gender law admits (male preference: daughters only
// when no such son lives), the eldest comes first. When the cache's first entry is such a child, the
// cache stands. When it is a child of one of the holder's DEAD children (a dead son's line ranks before
// his younger brothers), it stands too. Otherwise the cache is stale: the eldest admitted child is the
// primary heir, the admitted children lead the line in age order, and the cache's entries follow.
//
// Not decided here: the rank order of an ultimogeniture-style law (oldest first is assumed; the
// game's default rank), a designated heir (the reply carries designated_heir_id beside the line),
// and elective titles (the caller does not correct them).
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace bridge::heir_line {

// EffSuccRule::gender values (partition_heirs.h)
enum Gender : uint8_t { MALE_ONLY = 0, MALE_PREFERENCE = 1, EQUAL = 2, FEMALE_PREFERENCE = 3,
                        FEMALE_ONLY = 4, UNSET = 5 };

struct Child {
    uint32_t id = 0;
    bool alive = false;
    bool female = false;
    int age = 0;          // years; ordering only
    bool barred = false;  // an inheritance_blocker trait
};

struct Result {
    std::vector<uint32_t> line;      // the corrected line, position 1 first
    uint32_t cache_primary = 0;      // the cache's own first entry (0: empty)
    bool stale = false;              // the cache leaves out an admitted child or names someone else first
    std::vector<uint32_t> missing;   // admitted children the cache does not hold
    std::vector<uint32_t> admitted;  // the admitted children, eldest first
};

// The admitted children, eldest first (stable among equal ages: the family array's order).
inline std::vector<uint32_t> admitted_children(const std::vector<Child>& kids, uint8_t gender) {
    std::vector<Child> sons, daughters;
    for (const Child& c : kids) {
        if (!c.id || !c.alive || c.barred) continue;
        (c.female ? daughters : sons).push_back(c);
    }
    std::vector<Child> keep;
    switch (gender) {
        case MALE_ONLY: keep = sons; break;
        case FEMALE_ONLY: keep = daughters; break;
        case MALE_PREFERENCE: keep = !sons.empty() ? sons : daughters; break;
        case FEMALE_PREFERENCE: keep = !daughters.empty() ? daughters : sons; break;
        case EQUAL:
            for (const Child& c : kids)
                if (c.id && c.alive && !c.barred) keep.push_back(c);
            break;
        default:
            return {};   // an unread gender law: no correction (the caller keeps the cache)
    }
    std::stable_sort(keep.begin(), keep.end(), [](const Child& a, const Child& b) { return a.age > b.age; });
    std::vector<uint32_t> out;
    for (const Child& c : keep) out.push_back(c.id);
    return out;
}

// cache: the title's cached line (position 1 first). kids: the holder's children (dead ones too:
// a dead child's line is recognised through slot0_parents). slot0_parents: father and mother of the
// cache's first entry (0 when unread). gender: the title's effective gender law.
inline Result check(const std::vector<uint32_t>& cache, const std::vector<Child>& kids,
                    uint32_t slot0_father, uint32_t slot0_mother, uint8_t gender) {
    Result r;
    r.cache_primary = cache.empty() ? 0 : cache[0];
    r.admitted = admitted_children(kids, gender);
    auto in = [](const std::vector<uint32_t>& v, uint32_t id) { return std::find(v.begin(), v.end(), id) != v.end(); };
    for (uint32_t id : r.admitted)
        if (!in(cache, id)) r.missing.push_back(id);
    if (r.admitted.empty()) { r.line = cache; return r; }
    bool head_ok = in(r.admitted, r.cache_primary);
    if (!head_ok && r.cache_primary) {
        for (const Child& c : kids) {
            if (!c.id || c.alive) continue;
            if (c.id == slot0_father || c.id == slot0_mother) { head_ok = true; break; }  // a dead child's line
        }
    }
    if (head_ok) {
        r.line = cache;
        // an admitted child the cache never held goes after the cached children (younger: born after it
        // was built); the head stands
        for (uint32_t id : r.missing) r.line.push_back(id);
        r.stale = !r.missing.empty();
        return r;
    }
    r.stale = true;
    r.line = r.admitted;
    for (uint32_t id : cache)
        if (!in(r.line, id)) r.line.push_back(id);
    return r;
}

}  // namespace bridge::heir_line
