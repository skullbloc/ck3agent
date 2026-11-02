// test_char_is_alive.cpp — cleanup final fix (witness #8 minor 8). NO GAME.
//
// THE CLAIM: a character whose death date cannot be read is not reported as
// alive. safe_read answers 0 on a fault and 0 is the "alive" value, so before
// the fix enumerate_ai_characters handed such a character to its callback as a
// living AI ruler. The fixture is real memory: three characters in a stand-in
// CHARACTER_DB, one of them placed so its DEATH_DATE field is the first byte of
// a PROT_NONE page (every field below it readable), one alive, one dead.
//
// game_state.cpp is compiled into this test (its helpers are static); the two
// dispatch_router helpers it calls and the globals bridge.h declares are the
// stand-ins below.

#include "../src/game_state.cpp"

#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>
#include <vector>

namespace bridge {
uintptr_t g_base_addr = 0;
std::vector<MemRange> g_valid_ranges;
std::shared_mutex g_ranges_mutex;
FILE* g_logfile = nullptr;
uint32_t char_get_id(void* c) {
    return safe_read<uint32_t>(reinterpret_cast<uintptr_t>(c) + ck3::character::ID);
}
void* char_get_realm(void* c) {
    return safe_read<void*>(reinterpret_cast<uintptr_t>(c) + ck3::character::REALM_DATA);
}
}  // namespace bridge

using namespace bridge;

static int g_checks = 0, g_fails = 0;
static void check(bool ok, const char* what) {
    g_checks++;
    if (!ok) g_fails++;
    printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

template <typename T> static void put(uint8_t* base, size_t off, T v) {
    memcpy(base + off, &v, sizeof v);
}

int main() {
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    // AI controller (AI_ENABLED=1) and realm data (REALM_AI_CTRL -> it), shared.
    static uint8_t ai_ctrl[0x100] = {};
    ai_ctrl[ck3::ai_ctrl::AI_ENABLED] = 1;
    static uint8_t realm[0x400] = {};
    put<uintptr_t>(realm, ck3::character::REALM_AI_CTRL, reinterpret_cast<uintptr_t>(ai_ctrl));
    static uint8_t title_realm[0x10] = {};

    auto fill = [&](uint8_t* c, uint32_t id) {
        put<uint32_t>(c, ck3::character::ID, id);
        put<uintptr_t>(c, ck3::character::REALM_DATA, reinterpret_cast<uintptr_t>(realm));
        put<uintptr_t>(c, ck3::character::TITLE_REALM_DATA, reinterpret_cast<uintptr_t>(title_realm));
    };

    // Character 101: DEATH_DATE is the first byte of a PROT_NONE page.
    auto* two = static_cast<uint8_t*>(mmap(nullptr, 2 * page, PROT_READ | PROT_WRITE,
                                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (two == MAP_FAILED) { printf("FAIL mmap\n"); return 1; }
    uint8_t* unreadable = two + page - ck3::character::DEATH_DATE;
    fill(unreadable, 101);
    mprotect(two + page, page, PROT_NONE);
    // Character 102: alive (DEATH_DATE 0). Character 103: dead.
    static uint8_t alive[0x400] = {}, dead[0x400] = {};
    fill(alive, 102);
    fill(dead, 103);
    put<uintptr_t>(dead, ck3::character::DEATH_DATE, 0x1234);

    // The stand-in CHARACTER_DB: three buckets.
    static uint8_t table[ck3::hash_map::ENTRY_HEADER_SIZE + 3 * ck3::hash_map::ENTRY_STRIDE] = {};
    uint8_t* chars[3] = {unreadable, alive, dead};
    for (int i = 0; i < 3; i++)
        put<uintptr_t>(table, ck3::hash_map::ENTRY_HEADER_SIZE + i * ck3::hash_map::ENTRY_STRIDE,
                       reinterpret_cast<uintptr_t>(chars[i]));
    static uint8_t db[0x40] = {};
    put<uintptr_t>(db, ck3::hash_map::TABLE_PTR, reinterpret_cast<uintptr_t>(table));
    put<uint32_t>(db, ck3::hash_map::CAPACITY, 3);
    // The image globals: CHARACTER_DB -> db, CHAR_DB_SENTINEL -> 0.
    static uintptr_t image[2] = {reinterpret_cast<uintptr_t>(db), 0};
    static_assert(ck3::globals::CHAR_DB_SENTINEL == ck3::globals::CHARACTER_DB + 8, "fixture layout");
    g_base_addr = reinterpret_cast<uintptr_t>(image) - ck3::globals::CHARACTER_DB;

    std::vector<uint32_t> reported;
    enumerate_ai_characters([&](const CharacterInfo& ci) { reported.push_back(ci.id); return true; });

    bool has101 = false, has102 = false, has103 = false;
    for (uint32_t id : reported) { has101 |= id == 101; has102 |= id == 102; has103 |= id == 103; }
    check(has102, "a readable living AI character is reported (the fixture reaches the callback)");
    check(!has103, "a character with a death date is not reported");
    check(!has101, "a character whose death date is unreadable is not reported as alive");
    check(safe_read_fault_count() >= 1, "the unreadable death date was a counted fault, not a 0");

    printf("%d checks, %d failed\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
