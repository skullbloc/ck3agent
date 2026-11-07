// test_war_stance_record.cpp — L22, smoke19 row A3 (stance half): which
// coordinator record answers a query_war_stance row, on a fixture read from the
// 1097 core dump.
//
// Fixture: ck3_20260421_195552.core (1097, BuildID e61ff264..., the PROTECTED
// evidence dump; the 867 dump has no records yet, ai_root+0x54 = 0). CAIManager
// at the CAIMAN_VTABLE vptr (one hit, 0x7fb52e069c40), records
// *(mgr+0x48)[i] for i < *(mgr+0x54) = 174, active rec+0x14 != 0, war key
// rec+0x18 validated against WAR_DB (war+8 == rec+0x18), side rec+0x68 & 4,
// stance = rec+0x70 reverse-mapped against STANCE_DB_GLOBAL's 7 slots; war
// sides from war+0x28 / war+0x58 participant arrays and war+0x228 / +0x22C.
// Read 2026-11-06 with re/coredump/core_reader.py. All 87 wars carry exactly
// one attacker and one defender record; six are below, in ARRAY order, three of
// them (0x10000000, 0x10000005, 0x10000009) with the defender's record last (the
// smoke19 shape: Charles, primary attacker, read `is_attacker false,
// defender_defensive`). Only the attacker rows can occur live: stance_row_selected
// emits primary-attacker rows only; the primary-defender and absent checks cover
// the helper's contract, not a handler path.
//
// Built twice: plain (must pass) and with -DWAR_STANCE_RECORD_PREFIX_CONTROL,
// the pre-fix last-record-wins map, registered WILL_FAIL.
#include <cstdio>
#include <cstdint>
#include <vector>
#include "war_stance_record.h"

// 0 attacker_offensive, 1 attacker_defensive, 2 defender_offensive,
// 3 defender_defensive, 4 defender_desperate, 5 ghw_attacker, 6 ghw_defender
struct Rec { int array_index; bool is_attacker; int stance_idx; };
struct DumpWar {
    uint32_t war_id, primary_attacker, primary_defender;
    std::vector<uint32_t> attackers, defenders;
    std::vector<Rec> recs;   // array order
};
static const DumpWar kWars[] = {
    {0x10000000, 36923, 38745, {36923}, {38745}, {{172, true, 0}, {173, false, 3}}},
    {0x12000001, 33411, 41730, {33411}, {41730, 36801, 67405}, {{144, false, 2}, {168, true, 1}}},
    {0x11000004, 36930, 31297, {36930, 36372}, {31297, 35866}, {{0, false, 3}, {89, true, 0}}},
    {0x10000005, 70392, 69068, {70392}, {69068, 38063, 49305}, {{27, true, 1}, {155, false, 2}}},
    {0x10000009, 81640, 57124, {81640}, {57124}, {{100, true, 0}, {120, false, 3}}},
    {0x1200000b, 73786, 55095, {73786, 64847, 45712}, {55095, 67423, 33349}, {{50, false, 6}, {79, true, 5}}},
};

static int failures = 0;

static bool on_attacker_side(const DumpWar& w, uint32_t id) {
    for (uint32_t a : w.attackers) if (a == id) return true;
    return false;
}

static void check(const DumpWar& w, uint32_t who) {
    using namespace bridge::war_stance_record;
    WarRecs recs;
    for (const Rec& r : w.recs) note(recs, r.is_attacker, r.stance_idx);
    const bool side = on_attacker_side(w, who);
    int want = -2;
    for (const Rec& r : w.recs) if (r.is_attacker == side) want = r.stance_idx;
    const bool got_side = row_is_attacker(recs, side);
    const int got = for_side(recs, side).stance_idx;
    const bool ok = got_side == side && got == want && for_side(recs, side).present;
    if (!ok) ++failures;
    std::printf("%s war 0x%08x char %u: side %s stance %d (want %s %d)\n", ok ? "PASS" : "FAIL",
                w.war_id, who, got_side ? "attacker" : "defender", got,
                side ? "attacker" : "defender", want);
}

int main() {
    for (const DumpWar& w : kWars) {
        check(w, w.primary_attacker);
        check(w, w.primary_defender);
    }
    // An absent side reads as not present (the row then says "unavailable").
    bridge::war_stance_record::WarRecs only_def;
    bridge::war_stance_record::note(only_def, false, 3);
    if (bridge::war_stance_record::for_side(only_def, true).present) {
        ++failures; std::printf("FAIL absent attacker record read as present\n");
    }
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
