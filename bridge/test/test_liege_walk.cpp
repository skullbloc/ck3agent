// test_liege_walk.cpp — a liege walk that cannot finish is 0, never the
// character's own id (L15 A2; bridge/include/liege_walk.h).
//
// THE DEFECT. get_top_realm_ruler_id returned the id it had reached when a
// liege did not look up: from the first hop that is the character's own id, so
// a vassal read as independent (query_character top_liege_id == id passes the
// shield's "independent" clause; query_conquest_targets' sub-vassal gate read
// such a holder as an independent top).
#include "liege_walk.h"

#include <cstdio>
#include <map>

static int g_fail = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); g_fail++; } } while (0)

// A stand-in world: handle = id (0 = none); liege[id] = its liege id; an id
// missing from `exists` does not look up.
struct World {
    std::map<uint32_t, uint32_t> liege;
    std::map<uint32_t, bool> exists;
    uint32_t top(uint32_t self) const {
        return liege_walk::top_liege(
            self, self,
            [this](uint32_t h) { auto it = liege.find(h); return it == liege.end() ? 0u : it->second; },
            [this](uint32_t id) { return exists.count(id) ? id : 0u; });
    }
};

int main() {
    World w;
    w.exists = {{10, true}, {20, true}, {30, true}};
    w.liege = {{30, 20}, {20, 10}};            // 30 -> 20 -> 10 (independent)
    CHECK(w.top(10) == 10, "an independent ruler is its own top liege");
    CHECK(w.top(20) == 10, "a vassal's top liege is the head of the chain");
    CHECK(w.top(30) == 10, "a sub-vassal walks two hops");

    w.liege[40] = 99;                          // 99 does not look up
    CHECK(w.top(40) == liege_walk::kUnresolved,
          "a vassal whose first liege does not resolve is UNRESOLVED, not independent (RED before L15 A2: 40)");
    w.exists[50] = true; w.liege[50] = 40;     // 50 -> 40 -> (99 missing)
    CHECK(w.top(50) == liege_walk::kUnresolved,
          "a later unresolved hop is UNRESOLVED, not the intermediate liege (RED before: 40)");

    w.exists[60] = true; w.exists[61] = true;
    w.liege[60] = 61; w.liege[61] = 60;        // a liege loop
    CHECK(w.top(60) == liege_walk::kUnresolved, "a liege loop is UNRESOLVED (RED before: 61)");

    World deep;                                // 31 hops, past the cap
    for (uint32_t i = 1; i <= 32; i++) { deep.exists[i] = true; if (i < 32) deep.liege[i] = i + 1; }
    CHECK(deep.top(1) == liege_walk::kUnresolved, "a chain past the cap is UNRESOLVED");
    CHECK(deep.top(10) == 32, "a chain within the cap reaches its head");

    w.liege[70] = 70; w.exists[70] = true;     // a self-liege reads as no liege
    CHECK(w.top(70) == 70, "a self liege is the top");

    if (g_fail) { std::fprintf(stderr, "%d failure(s)\n", g_fail); return 1; }
    std::printf("test_liege_walk: all passed\n");
    return 0;
}
