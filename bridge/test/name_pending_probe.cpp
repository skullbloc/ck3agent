// name_pending_probe.cpp — lane-namekey.
//
// THE DEFECT
// ----------
// ad_read_char_name answered a name-cache MISS with the localization KEY
// stored on CCharacter+0x20 (`E_thelred` for Æthelred). The reader thread
// cannot localize: it queues the id and the GAME thread resolves it in
// populate_name_cache() on the next tick. So the first reads of a session —
// the game paused before any tick — handed every client a key where a name
// belongs (lane-multi run 3's first snapshot, two campaign notes, whoami
// before time moved). Every client then had to guess what a name is.
//
// THE CONTRACT THIS PROBES
// ------------------------
// A miss is stated, never disguised: the reply carries NO `name`, and carries
// `name_key` (the key, as a diagnostic) plus `name_pending: true`; the id is
// still queued, and once the game thread has run populate_name_cache() the
// same read answers `name` and nothing else. A character whose key cannot
// even be read is a third state (no name, no key, not pending) — "we looked
// and could not see" is not "the game has not looked yet".
//
// This drives the REAL ad_read_char_name_full / emit_char_name /
// emit_char_names out of the real dispatch_router.cpp against the REAL
// name_tables.cpp cache; the engine's resolver is replaced through the
// documented test seam set_character_name_resolver(), which is the game
// thread here. The synthetic image is test_release_handler.cpp's: one
// anonymous mapping laid out at the real offsets, read through
// process_vm_readv against our own address space.
//
// NEGATIVE CONTROL: name_pending_probe_negative_control is this same source
// with dispatch_router.cpp built -DCK3_NAME_LEGACY_KEY_FALLBACK, which
// restores the pre-fix reader (the key AS the name). It MUST fail, on the
// first-read assertions, by reproducing the live symptom verbatim.

#include "action_dispatch_internal.h"
#include "session.h"
#include "pipeline.h"
#include "intent_queue.h"
#include "name_tables.h"
#include "snapshot.h"
#include "episode_policy.h"

#include <sys/mman.h>

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

// =============================================================================
// Host stand-ins for the library globals dispatch_router.cpp expects.
// (Same set as test_release_handler.cpp, minus get_cached_character_name:
// this probe links the REAL name_tables.cpp.)
// =============================================================================
namespace bridge {

uintptr_t                g_base_addr = 0;
FILE*                    g_logfile = nullptr;
std::vector<MemRange>    g_valid_ranges;
std::shared_mutex        g_ranges_mutex;

std::mutex               g_controlled_mutex;
std::vector<uint32_t>    g_controlled_chars;
std::atomic<uint32_t>    g_controlled_primary{0};
std::atomic<uint32_t>    g_controlled_heir{0};

std::atomic<bool>        g_agent_mode{false};
std::atomic<bool>        g_auto_respond_events{false};
std::atomic<uint64_t>    g_tick_id{0};
std::atomic<uint64_t>    g_cb_query_busy_count{0};
std::atomic<bool>        g_war_stance_record_walk_enabled{false};

IntentQueue              g_intent_queue;
size_t IntentQueue::pending_count() { return 0; }

void council_lock_remove(uint32_t) {}
bool pipeline_owns_clock()       { return false; }
bool is_clock_verb(const char*)  { return false; }
uint64_t pipeline_current_flush_id() { return 0; }
PipelineMode pipeline_mode() { return PipelineMode::Legacy; }
void game_pause()   {}
void game_unpause() {}
const char* session_state_string() { return "test"; }
std::string enumerate_interactions() { return "[]"; }
std::string enumerate_decisions()    { return "[]"; }
bool set_character_subsystems(uint32_t, const char*) { return false; }
bool get_character_subsystems(uint32_t, std::string*) { return false; }
bool get_client_lock_info(ClientLockInfo*) { return false; }
bool is_game_state_query(const char*) { return false; }
const char* offset_validation_group_name(int) { return ""; }
OffsetValidationResult validate_offsets() { return OffsetValidationResult{}; }

}  // namespace bridge

using namespace bridge;

// =============================================================================
// Synthetic game image (test_release_handler.cpp's layout)
// =============================================================================
namespace {

constexpr size_t   kImageSize   = 0x0D000000;   // covers CHARACTER_DB @ ~0x0BECF088
constexpr uintptr_t kTableOff   = 0x0C000000;   // hash-map bucket table
constexpr uintptr_t kDbOff      = 0x0C800000;   // the hash-map object itself
constexpr uintptr_t kCharsOff   = 0x0CA00000;   // character entries
constexpr uint32_t  kCapacity   = 65536;

uintptr_t g_image = 0;

void must(bool cond, const char* what) {
    if (!cond) { std::cout << "    SETUP FAILED: " << what << "\n"; std::exit(2); }
}

template <typename T>
void poke(uintptr_t addr, T v) { *reinterpret_cast<T*>(addr) = v; }

// A libstdc++ std::string at `at`, SSO layout: [_M_p][_M_string_length][buf 16].
// `key` must fit the local buffer (<= 15 bytes), which every real loc key does.
void poke_sso_string(uintptr_t at, const char* key) {
    size_t len = strlen(key);
    must(len <= 15, "fake key fits the SSO buffer");
    poke<uintptr_t>(at, at + 0x10);
    poke<uint64_t>(at + 8, len);
    memcpy(reinterpret_cast<void*>(at + 0x10), key, len);
    *reinterpret_cast<char*>(at + 0x10 + len) = '\0';
}

// Lay out a character DB with `ids` in it; each entry carries its loc key at
// FIRST_NAME (an empty key = the string is left zeroed), no override, alive.
std::unordered_map<uint32_t, uintptr_t> build_image(const std::vector<std::pair<uint32_t, const char*>>& chars) {
    void* p = mmap(nullptr, kImageSize, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    must(p != MAP_FAILED, "synthetic game image mmap");
    g_image = reinterpret_cast<uintptr_t>(p);
    g_base_addr = g_image;
    {
        std::unique_lock<std::shared_mutex> lk(g_ranges_mutex);
        g_valid_ranges.clear();
        g_valid_ranges.push_back(MemRange{g_image, g_image + kImageSize});
    }
    poke<uintptr_t>(g_image + ck3::globals::CHARACTER_DB, g_image + kDbOff);
    poke<uint32_t>(g_image + kDbOff + ck3::hash_map::CAPACITY, kCapacity);
    poke<uintptr_t>(g_image + kDbOff + ck3::hash_map::TABLE_PTR, g_image + kTableOff);

    std::unordered_map<uint32_t, uintptr_t> out;
    uintptr_t cursor = g_image + kCharsOff;
    for (const auto& [id, key] : chars) {
        must((id & 0xFFFFFF) == id && id < kCapacity, "character id fits the fake DB");
        uintptr_t c = cursor;
        cursor += 0x1000;
        poke<uint32_t>(c + ck3::character::ID, id);
        if (key && *key) poke_sso_string(c + ck3::character::FIRST_NAME, key);
        // OVERRIDE_FLAG / OVERRIDE_NAME zero: the first name is the one in use.
        // DEATH_DATE zero: alive (name_resolve_input_is_safe's liveness test).
        poke<uintptr_t>(g_image + kTableOff + ck3::hash_map::ENTRY_HEADER_SIZE
                            + static_cast<uintptr_t>(id) * ck3::hash_map::ENTRY_STRIDE,
                        c);
        out[id] = c;
    }
    return out;
}

// ---------------------------------------------------------------------------
// The game thread: the engine resolver, replaced through the test seam. It
// RECORDS every id it is asked for — that record is how the probe sees that a
// miss queued the id (populate_name_cache only asks for queued ids).
// ---------------------------------------------------------------------------
std::map<uint32_t, std::string> g_localized = {
    {13902, "\xC3\x86thelred"},   // Æthelred, key E_thelred (the live case)
    {12145, "Boris"},
};
std::vector<uint32_t> g_resolver_asked;

bool fake_resolver(uint32_t id, std::string& out) {
    g_resolver_asked.push_back(id);
    auto it = g_localized.find(id);
    if (it == g_localized.end()) return false;   // "skip this id" — stays unresolved
    out = it->second;
    return true;
}

int asked_count(uint32_t id) {
    int n = 0;
    for (uint32_t x : g_resolver_asked) if (x == id) ++n;
    return n;
}

// ---------------------------------------------------------------------------
// Assertions
// ---------------------------------------------------------------------------
int g_failures = 0;
void check(bool cond, const std::string& what) {
    std::cout << (cond ? "    ok   " : "    FAIL ") << what << "\n";
    if (!cond) ++g_failures;
}

bool has(const std::string& j, const char* frag) { return j.find(frag) != std::string::npos; }

std::string names_json(uintptr_t c) {
    ResponseBuilder r;
    r.begin_object();
    emit_char_names(r, c);
    r.end_object();
    return r.take();
}

std::string name_json(uintptr_t c) {
    ResponseBuilder r;
    r.begin_object();
    emit_char_name(r, c);
    r.end_object();
    return r.take();
}

// [1] The first read of a session: the game thread has not run. The key is
// stated as pending, never as the name — on every reader.
void test_first_read_is_pending_not_the_key(uintptr_t c) {
    std::cout << "[1] first read, no tick yet: pending, never the key\n";
    clear_name_cache();
    g_resolver_asked.clear();

    CharName n = ad_read_char_name_full(c);
    check(n.name.empty(),          "ad_read_char_name_full: name is EMPTY on a miss");
    check(n.name != "E_thelred",   "ad_read_char_name_full: the key is not the name");
    check(n.pending,               "ad_read_char_name_full: pending");
    check(n.key == "E_thelred",    "ad_read_char_name_full: key carries the loc key");

    std::string s = ad_read_char_name(c);
    check(s.empty() && s != "E_thelred", "ad_read_char_name: empty on a miss, never the key");

    std::string j = names_json(c);
    check(!has(j, "\"name\":"),                 "emit_char_names: no `name` field (" + j + ")");
    check(has(j, "\"name_key\":\"E_thelred\""), "emit_char_names: name_key = the loc key");
    check(has(j, "\"name_pending\":true"),      "emit_char_names: name_pending = true");
    check(!has(j, "\"name\":\"E_thelred\""),   "emit_char_names: the key is never under `name`");

    std::string k = name_json(c);
    check(!has(k, "\"name\":") && has(k, "\"name_key\":\"E_thelred\"") && has(k, "\"name_pending\":true"),
          "emit_char_name (listing rows): same pending shape (" + k + ")");

    check(g_resolver_asked.empty(), "no game-thread call happened on the reader's path");
}

// [2] The game thread ticks: the miss had QUEUED the id, populate_name_cache
// resolves exactly the queued ids, and the same read now answers the name.
void test_tick_resolves_the_queued_id(uintptr_t c) {
    std::cout << "[2] after one tick: the queued id resolves, the reply carries the name\n";
    populate_name_cache();   // the game thread
    check(asked_count(13902) == 1, "populate_name_cache asked the resolver for the id the miss queued");

    CharName n = ad_read_char_name_full(c);
    check(n.name == "\xC3\x86thelred", "ad_read_char_name_full: the localized name (\xC3\x86thelred)");
    check(!n.pending && n.key.empty(), "ad_read_char_name_full: not pending, no key once resolved");
    check(ad_read_char_name(c) == "\xC3\x86thelred", "ad_read_char_name: the localized name");

    std::string j = names_json(c);
    check(has(j, "\"name\":\"\xC3\x86thelred\""), "emit_char_names: name = \xC3\x86thelred (" + j + ")");
    check(!has(j, "name_key") && !has(j, "name_pending"), "emit_char_names: no pending fields once resolved");

    size_t before = g_resolver_asked.size();
    populate_name_cache();
    check(g_resolver_asked.size() == before, "a hit queues nothing: the next tick has nothing to resolve");
}

// [3] A character the resolver skips (name_tables' "unsafe input" path) stays
// pending on every read and is re-queued, never shown as its key.
void test_unresolvable_stays_pending(uintptr_t c) {
    std::cout << "[3] an id the game thread skips: pending on every read, re-queued each time\n";
    g_resolver_asked.clear();
    std::string j1 = names_json(c);
    check(!has(j1, "\"name\":") && has(j1, "\"name_key\":\"Bj_rn\"") && has(j1, "\"name_pending\":true"),
          "first read: pending with the key (" + j1 + ")");
    populate_name_cache();
    check(asked_count(30853) == 1, "tick 1 asked for it");
    std::string j2 = names_json(c);
    check(!has(j2, "\"name\":") && has(j2, "\"name_pending\":true"),
          "still pending after a tick that could not resolve it (" + j2 + ")");
    populate_name_cache();
    check(asked_count(30853) == 2, "tick 2 asked again: the read re-queued the id");
}

// [4] No key readable at all (zeroed string): the third state — no name, no
// key, NOT pending. Distinct from "the game has not looked yet".
void test_unreadable_key_is_not_pending(uintptr_t c) {
    std::cout << "[4] no readable key: no name, no key, not pending\n";
    CharName n = ad_read_char_name_full(c);
    check(n.name.empty() && n.key.empty() && !n.pending, "ad_read_char_name_full: all three empty/false");
    std::string j = names_json(c);
    check(!has(j, "\"name\":") && !has(j, "name_key") && !has(j, "name_pending"),
          "emit_char_names: no name fields at all (" + j + ")");
}

// [5] A null / unmapped pointer: nothing, and no crash.
void test_bad_pointer_is_silent() {
    std::cout << "[5] null / unmapped character pointer\n";
    CharName n = ad_read_char_name_full(0);
    check(n.name.empty() && n.key.empty() && !n.pending, "null: empty, not pending");
    std::string j = names_json(0);
    check(j == "{}", "emit_char_names(0) emits nothing (" + j + ")");
}

}  // namespace

int main() {
#ifdef CK3_NAME_LEGACY_KEY_FALLBACK
    std::cout << "=== lane-namekey NEGATIVE CONTROL "
                 "(dispatch_router.cpp built with -DCK3_NAME_LEGACY_KEY_FALLBACK) ===\n"
                 "Expect [1] to fail exactly as the live bridge did: the first read\n"
                 "answers `name: \"E_thelred\"` — the localization key as the name.\n\n";
#else
    std::cout << "=== lane-namekey: a name-cache miss is PENDING, never the key ===\n\n";
#endif

    auto chars = build_image({{13902, "E_thelred"},   // resolves to Æthelred on the tick
                              {30853, "Bj_rn"},       // the resolver skips it (unsafe input)
                              {40001, ""}});          // no readable key at all
    set_character_name_resolver(&fake_resolver);

    test_first_read_is_pending_not_the_key(chars[13902]);
    test_tick_resolves_the_queued_id(chars[13902]);
    test_unresolvable_stays_pending(chars[30853]);
    test_unreadable_key_is_not_pending(chars[40001]);
    test_bad_pointer_is_silent();

    set_character_name_resolver(nullptr);

    std::cout << "\n";
    if (g_failures) {
#ifdef CK3_NAME_LEGACY_KEY_FALLBACK
        std::cout << "NEGATIVE CONTROL REPRODUCED THE BUG: " << g_failures
                  << " assertion(s) failed, as required.\n";
#else
        std::cout << "FAILED: " << g_failures << " assertion(s).\n";
#endif
        return 1;
    }
#ifdef CK3_NAME_LEGACY_KEY_FALLBACK
    std::cout << "NEGATIVE CONTROL DID NOT REPRODUCE THE BUG — the probe is not "
                 "measuring the fallback it claims to measure.\n";
    return 1;
#else
    std::cout << "All name-pending assertions passed.\n";
    return 0;
#endif
}
