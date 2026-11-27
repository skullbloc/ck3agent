// test_heir_line.cpp — the line of succession with the holder's children first (heir_line.h).
//
// Row 1 is the recorded case (inspect title_succession 768 at 1076.07.12 and 1079.05.02):
// the cache Ida 33804, Eustache 37465, Godefroy 38294, ... (her line); Gottfried 34471's children Virginia
// 45066 (daughter, 1074) and Goffredo 69510 (son, 1076); law male preference; the game gave every title
// to Goffredo at 1079.08.01. Rows 2-8: the cache stands when its head is an admitted child or a dead
// child's son; the gender laws; a barred son; an unread law.
#include <cstdio>
#include "heir_line.h"

namespace hl = bridge::heir_line;

static int g_fail = 0;
#define ROW(name, cond)                                                     \
    do {                                                                    \
        if (!(cond)) { ++g_fail; std::printf("FAIL %s\n", name); }          \
        else std::printf("ok   %s\n", name);                                \
    } while (0)

int main() {
    const std::vector<uint32_t> ida_line = {33804, 37465, 38294, 38764, 65833, 35421};
    hl::Child virginia{45066, true, true, 4, false}, goffredo{69510, true, false, 2, false};

    // 1 Lorraine 1079: the son first, then the cache; stale; both children missing
    auto a = hl::check(ida_line, {virginia, goffredo}, 0, 0, hl::MALE_PREFERENCE);
    ROW("1 lorraine primary = Goffredo", !a.line.empty() && a.line[0] == 69510);
    ROW("1 lorraine stale", a.stale);
    ROW("1 lorraine cache_primary = Ida", a.cache_primary == 33804);
    ROW("1 lorraine missing = Goffredo (the admitted child)", a.missing == std::vector<uint32_t>({69510}));
    ROW("1 lorraine the daughter is not admitted beside a son", a.admitted == std::vector<uint32_t>({69510}));
    ROW("1 lorraine Ida follows", a.line.size() == 1 + ida_line.size() && a.line[1] == 33804);

    // 1b Lorraine 1074.09 (Virginia only): the daughter first
    auto b = hl::check(ida_line, {virginia}, 0, 0, hl::MALE_PREFERENCE);
    ROW("1b lorraine 1074 primary = Virginia", b.line[0] == 45066 && b.stale);

    // 2 a cache whose head is the eldest son stands
    hl::Child s1{100, true, false, 30, false}, s2{101, true, false, 25, false};
    auto c = hl::check({100, 101, 33804}, {s1, s2}, 0, 0, hl::MALE_PREFERENCE);
    ROW("2 head is a son: unchanged, not stale", c.line == std::vector<uint32_t>({100, 101, 33804}) && !c.stale);

    // 3 a dead eldest son's son heads the cache: it stands
    hl::Child dead{99, false, false, 0, false};
    auto d = hl::check({500, 100, 101}, {dead, s1, s2}, 99, 0, hl::MALE_PREFERENCE);
    ROW("3 dead son's line stands", d.line[0] == 500 && !d.stale);

    // 4 a son born after the cache was built while a son heads it: appended, flagged stale, head stands
    hl::Child s3{102, true, false, 0, false};
    auto e = hl::check({100, 101}, {s1, s2, s3}, 0, 0, hl::MALE_PREFERENCE);
    ROW("4 newborn son appended, head stands", e.line == std::vector<uint32_t>({100, 101, 102}) && e.stale);

    // 5 male only: daughters never admitted; with only a daughter the cache stands
    auto f = hl::check({33804}, {virginia}, 0, 0, hl::MALE_ONLY);
    ROW("5 male only + daughter: cache stands", f.line == std::vector<uint32_t>({33804}) && !f.stale);

    // 6 equal: the eldest child of either sex
    auto g = hl::check(ida_line, {virginia, goffredo}, 0, 0, hl::EQUAL);
    ROW("6 equal: Virginia (elder) first", g.line[0] == 45066 && g.line[1] == 69510);

    // 7 a barred son (inheritance_blocker) is not admitted: the daughter heads under male preference
    hl::Child barred{69510, true, false, 2, true};
    auto h = hl::check(ida_line, {virginia, barred}, 0, 0, hl::MALE_PREFERENCE);
    ROW("7 barred son: daughter first", h.line[0] == 45066);

    // 8 an unread gender law: no correction
    auto i = hl::check(ida_line, {virginia, goffredo}, 0, 0, hl::UNSET);
    ROW("8 unset law: cache kept", i.line == ida_line && !i.stale);

    // 9 empty cache (the 1097 core: 23 duchy+ titles): the eldest admitted child
    auto j = hl::check({}, {s1, s2}, 0, 0, hl::MALE_PREFERENCE);
    ROW("9 empty cache: eldest son", j.line == std::vector<uint32_t>({100, 101}) && j.stale);

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
