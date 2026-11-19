// test_realm_tally.cpp —. NO GAME. realm_tally.h against the
// per-ruler loop it replaced.
//
// THE CLAIM: for every ruler R, Tally::total(R, demesne(R)) and
// Tally::unresolved_for(R) answer exactly what count_realm_counties(R) answered
// when it walked the character table itself (the loop below is that function's
// body at d1e28164, bridge/src/query_handlers.cpp, with the bridge's reads
// replaced by a fake population), and a Tally is ONE walk however many rulers
// it answers. The population mixes independent holders (top == self), vassals
// of several tops, unwalkable chains (top 0), holders of no county, dead and
// unlanded characters, and repeated ids.

#include "realm_tally.h"

#include <cstdio>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

static int g_checks = 0;
static int g_fails  = 0;

static void check(bool ok, const char* what, const std::string& detail = "") {
    g_checks++;
    if (!ok) g_fails++;
    printf("  %-4s %s%s%s\n", ok ? "ok" : "FAIL", what, detail.empty() ? "" : " — ", detail.c_str());
}

struct Ch {
    uint32_t id;
    bool alive, has_realm;
    int demesne;
    uint32_t top;
};

static int g_walks = 0;

// The enumeration the bridge's walk makes (enumerate_ai_characters calls back
// alive AI characters; dead ones are kept here so the alive guard is exercised).
template <class Cb>
static void each_of(const std::vector<Ch>& pop, Cb cb) {
    g_walks++;
    for (size_t i = 0; i < pop.size(); i++) cb(pop[i].id, pop[i].alive, pop[i].has_realm, i);
}

// count_realm_counties at d1e28164, its reads replaced by the fake population.
static int reference(const std::vector<Ch>& pop, uint32_t ruler_id, int ruler_demesne, int* unresolved) {
    ruler_id &= 0xFFFFFF;
    int total = ruler_demesne;
    each_of(pop, [&](uint32_t id, bool alive, bool has_realm, size_t i) {
        if (!alive || !has_realm) return;
        uint32_t vid = id & 0xFFFFFF;
        if (vid == ruler_id) return;
        int vc = pop[i].demesne;
        if (vc == 0) return;
        uint32_t top = pop[i].top;
        if (top == ruler_id)
            total += vc;
        else if (top == 0 && unresolved)
            (*unresolved)++;
    });
    return total;
}

int main() {
    printf("test_realm_tally\n");
    std::mt19937 rng(20261117);
    int rounds_ok = 0, compared = 0, one_walk = 0;
    for (int round = 0; round < 40; round++) {
        std::vector<Ch> pop;
        const int n = 200 + int(rng() % 400);
        std::vector<uint32_t> tops = {0};
        for (int k = 0; k < 6; k++) tops.push_back(1 + rng() % uint32_t(n));
        for (int i = 0; i < n; i++) {
            Ch c;
            c.id = 1 + uint32_t(i);
            if (rng() % 50 == 0 && !pop.empty()) c.id = pop[rng() % pop.size()].id;   // a repeated id
            c.alive = rng() % 10 != 0;
            c.has_realm = rng() % 6 != 0;
            c.demesne = (rng() % 3 == 0) ? 0 : int(rng() % 6);
            switch (rng() % 4) {
                case 0: c.top = c.id; break;                                  // independent
                case 1: c.top = 0; break;                                     // walk not made
                default: c.top = tops[1 + rng() % (tops.size() - 1)]; break;  // a vassal of a top
            }
            pop.push_back(c);
        }
        g_walks = 0;
        realm_tally::Tally t = realm_tally::build(
            [&](auto cb) { each_of(pop, cb); },
            [&](size_t i) { return pop[i].demesne; },
            [&](size_t i) { return pop[i].top; });
        const int walks_for_tally = g_walks;
        bool all = true;
        // Every id in the population, every top, and an id nobody holds.
        std::vector<uint32_t> rulers = tops;
        for (const Ch& c : pop) rulers.push_back(c.id);
        rulers.push_back(uint32_t(n) + 77);
        for (uint32_t r : rulers) {
            if (r == 0) continue;
            const int dem = int(r % 5);
            int ref_unres = 0;
            const int ref = reference(pop, r, dem, &ref_unres);
            const int got = t.total(r, dem);
            const int got_unres = t.unresolved_for(r);
            compared++;
            if (ref != got || ref_unres != got_unres) {
                all = false;
                char buf[160];
                snprintf(buf, sizeof buf, "round %d ruler %u: reference %d/%d, tally %d/%d",
                         round, r, ref, ref_unres, got, got_unres);
                check(false, "a ruler's total and unresolved equal the per-ruler loop's", buf);
                break;
            }
        }
        if (walks_for_tally == 1 && t.walks == 1) one_walk++;
        if (all) rounds_ok++;
    }
    check(one_walk == 40, "every Tally of 40 was one walk of the table, however many rulers it answered");
    check(rounds_ok == 40, "every ruler of 40 populations answers as the per-ruler loop did",
          std::to_string(compared) + " rulers compared");
    // The cases named, one each.
    {
        std::vector<Ch> pop = {
            {10, true, true, 3, 10},   // the king himself, independent: his entry is his demesne
            {11, true, true, 2, 10},   // a vassal of the king
            {12, true, true, 4, 10},   // a sub-vassal: his top is the king, not his liege
            {13, true, true, 5, 0},    // a chain that cannot be walked
            {14, false, true, 9, 10},  // dead
            {15, true, false, 9, 10},  // unlanded
            {16, true, true, 0, 10},   // holds no county
            {20, true, true, 7, 20},   // another king
        };
        realm_tally::Tally t = realm_tally::build(
            [&](auto cb) { each_of(pop, cb); },
            [&](size_t i) { return pop[i].demesne; },
            [&](size_t i) { return pop[i].top; });
        check(t.total(10, 3) == 3 + 2 + 4, "the king: demesne plus every holder under him, once");
        check(t.total(11, 2) == 2, "a vassal: his demesne only (no holder walks to him)");
        check(t.unresolved_for(10) == 1 && t.unresolved_for(13) == 0,
              "the unwalkable holder is unresolved for everyone but himself");
        check(t.total(20, 7) == 7, "the other king: his own");
    }
    // The batched read's ids (parse_batch): the whole list or a typed refusal.
    {
        std::vector<uint32_t> ids;
        auto code = [&](const char* json) {
            const char* c = realm_tally::parse_batch(json, &ids);
            return std::string(c ? c : "ok");
        };
        check(code(R"({"action":"query_realm","params":{"character_ids":[37067,"41",7]},"request_id":"r"})") == "ok"
              && ids == std::vector<uint32_t>{37067, 41, 7}, "ids under params, in order (numeric strings too)");
        check(code(R"({"character_ids":[5]})") == "ok" && ids == std::vector<uint32_t>{5}, "ids at the top level");
        check(code(R"({"params":{"character_ids":[5],"character_id":5}})") == "ambiguous_character_ids",
              "character_id beside character_ids is refused");
        check(code(R"({"params":{"character_ids":[]}})") == "invalid_character_ids" && ids.empty(), "an empty list is refused");
        check(code(R"({"params":{"character_ids":7}})") == "invalid_character_ids", "a non-list is refused");
        check(code(R"({"params":{"character_ids":[3,0,4]}})") == "invalid_character_ids" && ids.empty(),
              "a list with a non-positive id is refused whole");
        check(code(R"({"params":{"character_ids":[3,"x"]}})") == "invalid_character_ids", "a non-numeric id is refused");
        check(code(R"({"params":{"character_ids":[16777216]}})") == "invalid_character_ids", "an id over 24 bits is refused");
        std::string many = R"({"params":{"character_ids":[)";
        for (int i = 1; i <= realm_tally::kBatchMax + 1; i++) many += (i > 1 ? "," : "") + std::to_string(i);
        many += "]}}";
        check(code(many.c_str()) == "too_many_character_ids" && ids.empty(), "a list over kBatchMax is refused whole");
        std::string full = R"({"params":{"character_ids":[)";
        for (int i = 1; i <= realm_tally::kBatchMax; i++) full += (i > 1 ? "," : "") + std::to_string(i);
        full += "]}}";
        check(code(full.c_str()) == "ok" && int(ids.size()) == realm_tally::kBatchMax, "kBatchMax ids are read");
    }
    printf("%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
