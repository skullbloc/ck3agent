// test_war_scope.cpp — L20-A, smoke17 row A3: query_war_stance's row rule on a
// fixture read from the 867 core dump, through the REAL json_get_int.
//
// Fixture: the core dump ck3_20260625_152901_ivar867 (CLAUDE.md, "Runtime probing") (867.01.01,
// BuildID e61ff264...), WAR_DB = *(base+0x0bed8888), table *(db+0x20), capacity
// *(db+0x2c) = 1024; every slot whose war+0x08 low-24 equals the slot and whose
// war+0x2F8 (IS_ENDED) is 0 — the ten active wars — with war+0x228
// (PRIMARY_ATK_REF) low-24. Read 2026-11-04 with re/coredump/core_reader.py.
// The dump's controlled ruler 14859 is primary attacker in wars 3 and 4.
//
// Built twice: plain (must pass) and with -DWAR_SCOPE_PREFIX_CONTROL, the
// pre-fix scope read, registered WILL_FAIL (it lists no war for 14859, which is
// the A3 reply: `wars []` with records matched).
#include <cstdio>
#include <cstdint>
#include <vector>
#include "war_scope.h"

namespace bridge { int json_get_int(const char* json, const char* key); }

struct DumpWar { uint32_t slot, stored_id, primary_attacker; };
static const DumpWar kWars[] = {
    {0, 0x0, 12618}, {1, 0x1, 14147}, {2, 0x2, 13356}, {3, 0x3, 14859}, {4, 0x4, 14859},
    {5, 0x5, 12491}, {6, 0x6, 15111}, {7, 0x7, 12131}, {8, 0x8, 12114}, {9, 0x9, 11657},
};

static std::vector<uint32_t> rows(const char* params, uint32_t character_id) {
    using namespace bridge::war_scope;
    const uint32_t scope = from_param(bridge::json_get_int(params, "war_id"));
    std::vector<uint32_t> out;
    for (const auto& w : kWars)
        if (stance_row_selected(scope, w.slot, w.primary_attacker, character_id))
            out.push_back(w.stored_id);
    return out;
}

static int failures = 0;
static void expect(const char* name, const std::vector<uint32_t>& got,
                   const std::vector<uint32_t>& want) {
    const bool ok = got == want;
    printf("%s %-44s got %zu row(s)", ok ? "PASS" : "FAIL", name, got.size());
    for (uint32_t g : got) printf(" %u", g);
    printf("\n");
    if (!ok) failures++;
}

int main() {
    // A3's call shape: character_id only, no war_id.
    expect("no war_id: 14859's wars", rows("{\"character_id\":14859}", 14859), {3, 4});
    expect("no war_id, params-wrapped", rows("{\"params\":{\"character_id\":14859}}", 14859), {3, 4});
    expect("war_id 0: every war (server default)", rows("{\"character_id\":14859,\"war_id\":0}", 14859), {3, 4});
    expect("war_id -1 (none-ref): every war", rows("{\"character_id\":14859,\"war_id\":-1}", 14859), {3, 4});
    expect("war_id 4: that war only", rows("{\"character_id\":14859,\"war_id\":4}", 14859), {4});
    // A full handle: the generation byte is ignored, the slot selects.
    expect("war_id gen-tagged 0x0B000003", rows("{\"character_id\":14859,\"war_id\":184549379}", 14859), {3});
    expect("war_id 5: not 14859's war", rows("{\"character_id\":14859,\"war_id\":5}", 14859), {});
    expect("defender only (13903): no row", rows("{\"character_id\":13903}", 13903), {});
    expect("absent war_id, other attacker 12618", rows("{\"character_id\":12618}", 12618), {0});
    // query_war_score_breakdown's documented `{}`: every cached war (witness r1 item 8)
    auto breakdown_all = [](const char* params) {
        return bridge::war_scope::all_wars(
            bridge::war_scope::from_param(bridge::json_get_int(params, "war_id")));
    };
    auto expect_bool = [](const char* name, bool got, bool want) {
        printf("%s %-44s got %s\n", got == want ? "PASS" : "FAIL", name, got ? "all" : "one");
        if (got != want) failures++;
    };
    expect_bool("breakdown {}: every cached war", breakdown_all("{}"), true);
    expect_bool("breakdown war_id 0: every cached war", breakdown_all("{\"war_id\":0}"), true);
    expect_bool("breakdown war_id 4: that war", breakdown_all("{\"war_id\":4}"), false);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
