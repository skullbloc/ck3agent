// test_partition_heirs.cpp — the heirs of a partition succession (partition_heirs.h).
//
// Rows 1-3 are the three recorded cases of re/findings/re_succession_survival.md P3, with the line
// the simulation read (the heirs it named, in its order) and the family the run's reads show:
//   1 (1071-1082): Murchad 31506; line head Culdub 66780 (a dead son's son), then 15819
//     Duinsech (Culdub's sister), 37528 Donnchad (a son); the game gave Donnchad Athlone and
//     Leinster at 1082.01.17. The old split named 15819.
//   2 (1071-1076, another game): line head Domnall 36124 (a son), then his own children, then the sons
//     37111 / 37528, then the brother 32805; the old split named 32805 (the line's 5th entry).
//   3 (1072, a third game): as 2, the brother 32805 named for a year; the game gave 37111 his share (1078).
// Rows 4-7 the gender law and the no-child case.
#include <algorithm>
#include <cstdio>
#include <set>
#include "partition_heirs.h"

namespace ph = bridge::partition_heirs;

static int g_fail = 0;
#define ROW(name, cond)                                                     \
    do {                                                                    \
        if (!(cond)) { ++g_fail; std::printf("FAIL %s\n", name); }          \
        else std::printf("ok   %s\n", name);                                \
    } while (0)

static bool eq(const std::vector<uint32_t>& a, std::vector<uint32_t> b) { return a == b; }

int main() {
    const std::set<uint32_t> murchad_children = {36124, 37111, 37528};
    const std::set<uint32_t> women = {15819, 70001};
    auto child = [&](uint32_t id) { return murchad_children.count(id) > 0; };
    auto female = [&](uint32_t id) { return women.count(id) > 0; };

    // 1: Culdub, then his sister, then Donnchad
    auto a = ph::select({66780, 15819, 37528}, child, female, ph::MALE_PREFERENCE);
    ROW("1 game A heirs = Culdub, Donnchad", eq(a.heirs, {66780, 37528}));
    ROW("1 game A the granddaughter takes nothing", eq(a.excluded, {15819}));
    ROW("1 game A basis children_in_line", a.basis == ph::Basis::children_in_line);

    // 2: Domnall, his son 70412, the sons 37111 and 37528, the brother 32805
    auto b = ph::select({36124, 70412, 37111, 37528, 32805}, child, female, ph::MALE_PREFERENCE);
    ROW("2 game B heirs = the three sons", eq(b.heirs, {36124, 37111, 37528}));
    ROW("2 game B the brother takes nothing",
        std::find(b.heirs.begin(), b.heirs.end(), 32805u) == b.heirs.end());

    // 3 (1072): Domnall, the brother before 37111 in a line read
    auto c = ph::select({36124, 32805, 37111}, child, female, ph::MALE_PREFERENCE);
    ROW("3 run C heirs = Domnall, Enna 37111", eq(c.heirs, {36124, 37111}));

    // 4 male preference: a daughter shares only when no son does
    const std::set<uint32_t> kids2 = {501, 502, 70001};
    auto child2 = [&](uint32_t id) { return kids2.count(id) > 0 || id == 500; };
    auto d = ph::select({500, 70001, 502}, child2, female, ph::MALE_PREFERENCE);
    ROW("4 male preference drops the daughter", eq(d.heirs, {500, 502}) && eq(d.excluded, {70001}));
    auto e = ph::select({500, 70001}, child2, female, ph::MALE_PREFERENCE);
    ROW("5 male preference keeps a daughter with no son", eq(e.heirs, {500, 70001}));
    auto f = ph::select({500, 70001, 502}, child2, female, ph::EQUAL);
    ROW("6 equal keeps both in line order", eq(f.heirs, {500, 70001, 502}));

    // 7 a ruler with no child in the line (1084: Culdub, childless; line = sister, uncle)
    auto none = [](uint32_t) { return false; };
    auto g = ph::select({15819, 37528}, none, female, ph::MALE_PREFERENCE);
    ROW("7 no child in line: the head alone, basis says so",
        eq(g.heirs, {15819}) && g.basis == ph::Basis::no_child_in_line && eq(g.excluded, {37528}));
    ROW("8 empty line", ph::select({}, none, female, ph::EQUAL).heirs.empty());

    std::printf("%s\n", g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
