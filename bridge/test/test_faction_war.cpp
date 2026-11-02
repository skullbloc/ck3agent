// A faction's war as the game reads it (faction_war.h; cleanup 2026.11 F16,
// witness w-l10-factions-5).
//
// Build: CMake target `test_faction_war` (BUILD_BRIDGE_TESTS=ON).
// Run:   ./test_faction_war   (exits non-zero on failure)
//
// The witness read faction 95 at war (war 51, the game's own sentence "Cannot press
// demands while the faction is in war") with at_war false: the bridge read
// faction+0xC1, which is not the war. And faction rows gave the war's id masked to
// 24 bits while every war read gives the war's full handle (run l10fx5-live-2: 19 on
// the faction row, 33554451 on the war read), so an id copied from a faction row
// into make_peace / call_ally / join_war named no war. The rule is the engine's
// (FUN_05f8ca90, FUN_05f8c910): the faction's WAR_REF indexes WAR_DB by its low 24
// bits, the slot's war must carry the whole handle, and the war it names is the
// faction's war; the id given out is that war's own WAR_ID.

#include <cstdint>
#include <cstdio>
#include <map>

#include "faction_war.h"

using bridge::FactionWar;
using bridge::faction_war_from;

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d): %s\n", #cond, __FILE__, __LINE__, msg); \
        failures++; \
    } \
} while (0)

// A WAR_DB stand-in: slot index -> war (an opaque address), war -> its WAR_ID.
struct FakeWarDb {
    std::map<uint32_t, uintptr_t> slot;
    std::map<uintptr_t, uint32_t> id;
    void add(uint32_t handle, uintptr_t war) { slot[handle & 0xFFFFFFu] = war; id[war] = handle; }
    FactionWar faction(uint32_t war_ref) const {
        return faction_war_from(
            war_ref,
            [this](uint32_t idx) -> uintptr_t { auto it = slot.find(idx); return it == slot.end() ? 0 : it->second; },
            [this](uintptr_t w) -> uint32_t { return id.at(w); });
    }
    // What a war read gives for the war in a slot: its WAR_ID (query_wars etc.).
    uint32_t war_read_id(uint32_t idx) const { return id.at(slot.at(idx)); }
};

// The witness's war: 33554451 = generation 2, slot 19.
static void test_faction_row_war_id_is_the_war_reads_id() {
    FakeWarDb db;
    const uint32_t handle = (2u << 24) | 19u;
    db.add(handle, 0x1000);
    FactionWar w = db.faction(handle);
    CHECK(w.at_war, "a faction whose WAR_REF names a live war is at war");
    CHECK(w.war_id == db.war_read_id(19), "the faction row's war_id is the id the war read gives");
    CHECK(w.war_id == 33554451u, "the full handle, generation byte included");
    CHECK(w.war_id != (handle & 0xFFFFFFu), "not the 24-bit slot index the faction rows used to give");
}

// War 0 is a real war (the engine numbers wars from 0): not "no war".
static void test_war_zero_is_a_war() {
    FakeWarDb db;
    db.add(0u, 0x2000);
    FactionWar w = db.faction(0u);
    CHECK(w.at_war && w.war_id == 0u, "war 0 is a war");
}

static void test_no_war() {
    FakeWarDb db;
    db.add(5u, 0x3000);
    CHECK(!db.faction(0xFFFFFFFFu).at_war, "WAR_REF 0xFFFFFFFF: no war");
    CHECK(!db.faction(7u).at_war, "an empty slot: the null war, not at war");
}

// A war that ended leaves the faction's WAR_REF until the next faction tick clears
// it (FactionTick sets WAR_INACTIVE_FLAG, the commit writes 0xFFFFFFFF); its slot
// may already hold another war. The whole handle decides.
static void test_reused_slot_is_another_war() {
    FakeWarDb db;
    db.add((3u << 24) | 19u, 0x4000);          // generation 3 now holds slot 19
    FactionWar w = db.faction((2u << 24) | 19u);
    CHECK(!w.at_war, "a stale handle whose slot holds another war: not at war");
    CHECK(w.war_id == 0u, "and no war id is given");
}

// CWar::IsValid is WAR_ID != 0xFFFFFFFF: a war whose WAR_ID was reset is no war.
static void test_invalidated_war() {
    FakeWarDb db;
    db.slot[19] = 0x5000;
    db.id[0x5000] = 0xFFFFFFFFu;
    CHECK(!db.faction((2u << 24) | 19u).at_war, "a slot war that is invalid: not at war");
}

int main() {
    test_faction_row_war_id_is_the_war_reads_id();
    test_war_zero_is_a_war();
    test_no_war();
    test_reused_slot_is_another_war();
    test_invalidated_war();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("test_faction_war: all passed\n");
    return 0;
}
