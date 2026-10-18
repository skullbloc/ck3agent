// test_release_handler.cpp — ck-sozz.
//
// THE GAP THIS CLOSES
// -------------------
// release_character failed 6/6 in the first live N=6 ownership run
// (2026-09-10): a character that had just been claimed came back
//   {"success":false,"reason":"not_in_roster","controlled_primary":0,
//    "roster_size":0,"hint":"...nothing to release"}
// while the ownership claim had in fact ALREADY BEEN DROPPED, and the early
// return skipped the whole cleanup half — personality lock, council lock, and
// re-enabling the engine's own per-character AI (ai_ctrl+0x28).
//
// The offline suite could not see it, for exactly the ck-xqol reason:
// test_session_registry drives SessionRegistry DIRECTLY, and the registry half
// is correct — release_character() returns true and erases the entry. Only the
// COMPOSITION was broken: the M1 ownership release had been inserted ABOVE
// ck-60bz's roster guard, a guard whose own contract is "validate roster
// membership BEFORE mutating anything". The guard was inspecting a state its
// own predecessor created.
//
// So this test drives the HANDLER — the real bridge::do_release_character out
// of the real dispatch_router.cpp — against a synthetic game image. The
// handler's ordering is the thing under test, and nothing below ever calls
// SessionRegistry::release_character itself.
//
// THE SYNTHETIC GAME IMAGE
// ------------------------
// lookup_character() walks g_base_addr -> CHARACTER_DB -> hash map -> entry,
// and the AI re-enable walks char+0x1A8 -> core_data+AI_CONTROLLER -> +0x28.
// All of it is process_vm_readv / plain loads against OUR OWN address space, so
// a single anonymous mapping with those structures laid out at the real offsets
// is a faithful stand-in. That is what lets the test assert the real thing the
// live bug destroyed: that the AI byte goes back to 1.
//
// NEGATIVE CONTROL: test_release_handler_negative_control is this same source
// linked against dispatch_router.cpp built with -DCK3_RELEASE_LEGACY_ORDER,
// which restores the pre-fix ordering. It MUST fail, and it must fail by
// reproducing the live symptom verbatim.

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
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

// =============================================================================
// Host stand-ins for the library globals dispatch_router.cpp expects.
// (Same pattern as test_session_registry.cpp / test_client_slot.cpp.)
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

// g_personality_lock_mutex / g_personality_locks are NOT defined here: since
// ck-hxh7 they live in personality_world_reset.cpp, which SESSION_HOST_SOURCES
// links. The real definitions are the ones the handler must clear.

IntentQueue              g_intent_queue;
size_t IntentQueue::pending_count() { return 0; }

// The council lock is a bridge-side map in another TU; here we just record that
// the handler asked for the clear, which is half of "the cleanup ran".
static std::vector<uint32_t> g_council_lock_removals;
void council_lock_remove(uint32_t char_id) { g_council_lock_removals.push_back(char_id); }

bool pipeline_owns_clock()       { return false; }
bool is_clock_verb(const char*)  { return false; }
// pipeline.cpp is engine-bound; the handlers under test never read a flush id.
uint64_t pipeline_current_flush_id() { return 0; }
// ck-1ek9: dispatch_router.cpp's observe-mode gate now asks the mode policy
// whether a null-mode observer may issue a clock verb, and the ACCESSOR lives in
// engine-bound pipeline.cpp (the pure POLICY is pipeline_mode.cpp, which this
// target does link). Legacy is G11's default and the right stub: it is the mode
// in which the gate behaves exactly as it did before that bead, so the release
// handlers under test see the pre-existing world and this stub cannot quietly
// grant them a carve-out.
PipelineMode pipeline_mode() { return PipelineMode::Legacy; }

void game_pause()   {}
void game_unpause() {}

// (require_dispatch_context / action_is_mutation / validate_actor_ownership
//  come from the real src/session.cpp, which this test links.)
const char* session_state_string() { return "test"; }

std::string get_cached_character_name(uint32_t) { return "(test)"; }
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
// Synthetic game image
// =============================================================================
namespace {

constexpr size_t   kImageSize   = 0x0D000000;   // covers CHARACTER_DB @ ~0x0BECF088
constexpr uintptr_t kTableOff   = 0x0C000000;   // hash-map bucket table
constexpr uintptr_t kDbOff      = 0x0C800000;   // the hash-map object itself
constexpr uintptr_t kSentinel   = 0x0C900000;   // CHAR_DB_SENTINEL target
constexpr uintptr_t kCharsOff   = 0x0CA00000;   // character entries
constexpr uint32_t  kCapacity   = 65536;

struct FakeChar {
    uintptr_t addr;        // CCharacter*
    uintptr_t core_data;   // char + 0x1A8
    uintptr_t ai_ctrl;     // core_data + AI_CONTROLLER
};

uintptr_t g_image = 0;

// Setup precondition.  NOT assert(): assert() vanishes under NDEBUG, and some of
// these wrap a handler call whose side effect is the point.
void must(bool cond, const char* what) {
    if (!cond) { std::cout << "    SETUP FAILED: " << what << "\n"; std::exit(2); }
}

template <typename T>
void poke(uintptr_t addr, T v) { *reinterpret_cast<T*>(addr) = v; }

// Lay out a character DB with `ids` in it, plus one core_data + ai_ctrl each.
std::unordered_map<uint32_t, FakeChar> build_image(const std::vector<uint32_t>& ids) {
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

    // Globals the lookup path reads out of the "binary".
    poke<uintptr_t>(g_image + ck3::globals::CHARACTER_DB,      g_image + kDbOff);
    poke<uintptr_t>(g_image + ck3::globals::CHAR_DB_SENTINEL,  g_image + kSentinel);
    poke<uint32_t>(g_image + ck3::globals::PLAYER_CHAR_ID,     0xFFFFFFFFu);  // observer

    // The hash map.
    poke<uint32_t>(g_image + kDbOff + ck3::hash_map::CAPACITY, kCapacity);
    poke<uintptr_t>(g_image + kDbOff + ck3::hash_map::TABLE_PTR, g_image + kTableOff);

    std::unordered_map<uint32_t, FakeChar> out;
    uintptr_t cursor = g_image + kCharsOff;
    for (uint32_t id : ids) {
        must((id & 0xFFFFFF) == id && id < kCapacity, "character id fits the fake DB");
        FakeChar fc;
        fc.addr      = cursor;
        fc.core_data = cursor + 0x1000;
        fc.ai_ctrl   = cursor + 0x2000;
        cursor += 0x3000;

        poke<uint32_t>(fc.addr + ck3::character::ID, id);
        poke<uintptr_t>(fc.addr + 0x1A8, fc.core_data);      // CCharacter -> core_data
        poke<uintptr_t>(fc.core_data + ck3::core_data::AI_CONTROLLER, fc.ai_ctrl);
        poke<uint8_t>(fc.ai_ctrl + ck3::ai_ctrl::AI_ENABLED, 1);   // native AI live

        poke<uintptr_t>(g_image + kTableOff + ck3::hash_map::ENTRY_HEADER_SIZE
                            + static_cast<uintptr_t>(id) * ck3::hash_map::ENTRY_STRIDE,
                        fc.addr);
        out[id] = fc;
    }
    return out;
}

uint8_t ai_enabled(const FakeChar& fc) {
    return *reinterpret_cast<uint8_t*>(fc.ai_ctrl + ck3::ai_ctrl::AI_ENABLED);
}

// ---------------------------------------------------------------------------
// Engine entry points do_set_controlled_character calls through. These are the
// library's own function pointers, so we aim them at host functions rather than
// at (unmapped) addresses inside the synthetic image.
// ---------------------------------------------------------------------------
bool  host_char_is_alive(void*)             { return true; }
void* host_get_ai_ctrl(void* character) {
    uintptr_t core = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(character) + 0x1A8);
    return reinterpret_cast<void*>(*reinterpret_cast<uintptr_t*>(core + ck3::core_data::AI_CONTROLLER));
}
void  host_ai_char_toggle(void* ai_ctrl, uint8_t on) {
    *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(ai_ctrl) + ck3::ai_ctrl::AI_ENABLED) =
        static_cast<uint8_t>(on ? 1 : 0);
}

// ---------------------------------------------------------------------------
// Tiny JSON field readers — enough to assert on the handler's typed responses.
// ---------------------------------------------------------------------------
bool json_flag(const std::string& j, const char* key, bool* value) {
    std::string k = std::string("\"") + key + "\":";
    size_t i = j.find(k);
    if (i == std::string::npos) return false;
    *value = j.compare(i + k.size(), 4, "true") == 0;
    return true;
}
std::string json_str(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\":\"";
    size_t i = j.find(k);
    if (i == std::string::npos) return "";
    size_t s = i + k.size();
    return j.substr(s, j.find('"', s) - s);
}
long json_num(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\":";
    size_t i = j.find(k);
    if (i == std::string::npos) return -1;
    return strtol(j.c_str() + i + k.size(), nullptr, 10);
}
bool ok(const std::string& j) { bool v = false; return json_flag(j, "success", &v) && v; }

// ---------------------------------------------------------------------------
// Dispatch scaffolding: install a session the way the IPC choke point does.
// ---------------------------------------------------------------------------
struct Scoped {
    ClientContext ctx;
    ClientContextGuard guard;
    explicit Scoped(Session* s) : ctx{s, nullptr, nullptr}, guard(&ctx) {}
};

std::string params(uint32_t id, const char* extra = "") {
    char buf[192];
    snprintf(buf, sizeof(buf), "{\"character_id\":%u%s}", id, extra);
    return buf;
}

int g_failures = 0;
void check(bool cond, const std::string& what) {
    if (cond) {
        std::cout << "    ok   " << what << "\n";
    } else {
        std::cout << "    FAIL " << what << "\n";
        ++g_failures;
    }
}

// ck-6kvq.10's claim pin. The gate moved from pipeline.cpp's
// pipeline_gate_pins_claims() to episode_policy()'s experiment mode (ck-ijfa),
// so ARM THE REAL ONE — a stub for a symbol the bridge no longer calls would
// leave §2.7 untested while reading as tested.
void arm_claim_pin(bool on) {
    EpisodePolicy p{};
    p.mode = on ? EpisodeMode::Experiment : EpisodeMode::FreePlay;
    episode_policy().set(p);
    must(episode_pins_claims() == on, "claim pin armed as asked");
}

void reset_world() {
    g_council_lock_removals.clear();
    g_personality_locks.clear();
    {
        std::lock_guard<std::mutex> lk(g_controlled_mutex);
        g_controlled_chars.clear();
    }
    g_controlled_primary.store(0);
    g_controlled_heir.store(0);
    g_agent_mode.store(false);
    arm_claim_pin(false);
    for (const auto& id : g_session_registry.ids()) g_session_registry.destroy(id);
}

void lock_personality(uint32_t id) {
    std::unique_lock<std::shared_mutex> lk(g_personality_lock_mutex);
    PersonalityLock pl{};
    pl.axes[0] = 90;
    pl.locked[0] = true;
    g_personality_locks[id] = pl;
}

}  // namespace

// =============================================================================
// Tests
// =============================================================================

// The live bug, reduced: claim one character through the handler, release it
// through the handler, and demand that the release actually SUCCEED and that
// every consequence of a release is visible afterwards.
static void test_owned_character_releases(std::unordered_map<uint32_t, FakeChar>& chars) {
    std::cout << "[1] a session-owned character releases, and the cleanup runs\n";
    reset_world();
    const uint32_t kId = 30744;   // the character from the live N=6 run
    FakeChar& fc = chars[kId];

    Session* s = g_session_registry.create("agent-a");
    Scoped scope(s);

    // ck-cwal: A SECOND CLAIM, so the release under test is not the seat's LAST.
    //
    // This test is about the CLEANUP half of a release, and under the progenitor's
    // ruling a control session's last-character release is REFUSED — so with one
    // claim the handler never reaches the cleanup and the test could no longer
    // observe what it was written for. The second claim restores its subject. The
    // refusal itself is tested immediately below, on the same seat, which is also
    // the bead's "with two -> allowed, with one -> refused" pair.
    const uint32_t kKeep = 30745;
    std::string take;
    bool took = do_set_controlled_character(params(kId, ",\"disable_native_ai\":true").c_str(), &take);
    check(took && ok(take), "set_controlled_character succeeded: " + take);
    check(json_num(take, "roster_size") == 1, "roster holds the character");
    // ck-cwal: the SECOND claim goes through the REGISTRY, not through
    // set_controlled_character — a second take on a seat that already holds one is
    // a SWITCH (swap_character, net zero under one lock), so it would leave the
    // seat with one claim and the release below would still be the last.
    {
        std::string why;
        check(g_session_registry.claim_character(s, kKeep, &why),
              "setup: the seat holds a second claim: " + why);
    }
    check(ai_enabled(fc) == 0, "native AI was disabled by the claim (ai_ctrl+0x28 == 0)");

    lock_personality(kId);
    check(g_personality_locks.count(kId) == 1, "a personality lock is in place before release");

    std::string rel;
    bool released = do_release_character(params(kId).c_str(), &rel);

    // The live symptom, asserted directly so the negative control reproduces it
    // by name rather than by some downstream side effect.
    check(released && ok(rel),
          "release_character succeeded (live bug returned success:false): " + rel);
    check(json_str(rel, "reason") != "not_in_roster",
          "release was NOT refused not_in_roster");

    // PER-CHARACTER cleanup, which is what this test is for.
    check(g_session_registry.owner_of(kId) == nullptr, "ownership entry is gone");
    check(g_personality_locks.count(kId) == 0, "personality lock was cleared");
    check(g_council_lock_removals.size() == 1 && g_council_lock_removals[0] == kId,
          "council lock was cleared");
    check(ai_enabled(fc) == 1, "native AI was RE-ENABLED (ai_ctrl+0x28 back to 1)");
    // SEAT-LEVEL state is NOT torn down, because the seat still holds kKeep. These
    // three used to assert the opposite, and that was correct when a seat could
    // empty itself. They are kept rather than deleted: a release that silently
    // cleared the primary or dropped agent_mode while a claim remained would be a
    // real defect, and nothing else asserts it.
    check(g_controlled_primary.load() == kKeep,
          "controlled primary moved to the remaining claim, not to 0");
    check(g_agent_mode.load() == true, "agent_mode stays on while a claim remains");
    {
        std::lock_guard<std::mutex> lk(g_controlled_mutex);
        check(g_controlled_chars.size() == 1, "roster holds only the remaining claim");
    }

    // ── ck-cwal: AND NOW IT IS THE LAST, SO IT IS REFUSED ────────────────────
    // Same seat, one claim left. The bead's required pair, on one world, so the
    // two answers cannot come from two different setups.
    const size_t councils_before = g_council_lock_removals.size();
    std::string rel2;
    const bool released2 = do_release_character(params(kKeep).c_str(), &rel2);
    check(!released2 && !ok(rel2), "the LAST claim's release is refused: " + rel2);
    check(json_str(rel2, "reason") == "last_claim_of_control_session",
          "typed reason is last_claim_of_control_session");
    check(rel2.find("you steer") != std::string::npos,
          "the refusal carries its explanation, not just a code");
    // NOTHING MUTATED — asserted on state, like the unowned-character refusal.
    check(g_session_registry.owner_of(kKeep) == s, "the seat still owns it");
    check(g_controlled_primary.load() == kKeep, "primary untouched by the refusal");
    check(g_agent_mode.load() == true, "agent_mode untouched by the refusal");
    check(g_council_lock_removals.size() == councils_before,
          "no council lock was cleared by a refused release");
    {
        std::lock_guard<std::mutex> lk(g_controlled_mutex);
        check(g_controlled_chars.size() == 1, "roster untouched by the refusal");
    }
}

// The other half of atomicity: a refusal must leave the world bit-for-bit as it
// was. Asserted on STATE, not on the return value — the live bug returned the
// right-looking refusal while having already mutated.
static void test_unowned_character_refused_without_mutating(
        std::unordered_map<uint32_t, FakeChar>& chars) {
    std::cout << "[2] a character this session does not own is refused, nothing mutated\n";
    reset_world();
    const uint32_t kMine = 301;
    const uint32_t kTheirs = 302;
    FakeChar& theirs = chars[kTheirs];

    Session* a = g_session_registry.create("agent-a");
    Session* b = g_session_registry.create("agent-b");

    { Scoped sa(a); std::string j; must(do_set_controlled_character(params(kMine).c_str(), &j), "setup take"); }
    {
        Scoped sb(b);
        std::string j;
        // ck-cwal: b holds a SECOND character, so its later release of kTheirs is
        // not its last and is therefore permitted. Without this the "true owner can
        // still release afterwards" assertion tests the new refusal instead of the
        // cross-session rule it was written for.
        must(do_set_controlled_character(params(kTheirs, ",\"disable_native_ai\":true").c_str(), &j), "setup take");
        //: ck-cwal: via the REGISTRY, so b holds two and its later release of
        //: kTheirs is not its last. A second take would SWITCH, not add.
        std::string why;
        must(g_session_registry.claim_character(b, 303u, &why), "setup: b holds a second");
    }
    lock_personality(kTheirs);
    check(ai_enabled(theirs) == 0, "b's character has native AI disabled");

    // ck-cwal: the size BEFORE the refused call, so the row below asserts the
    // invariant ("the refusal mutated nothing") rather than a literal that became a
    // statement about the setup once a third claim was added.
    size_t roster_before = 0;
    {
        std::lock_guard<std::mutex> lk(g_controlled_mutex);
        roster_before = g_controlled_chars.size();
    }

    // a tries to release b's character.
    std::string rel;
    bool released;
    {
        Scoped sa(a);
        released = do_release_character(params(kTheirs).c_str(), &rel);
    }
    check(!released && !ok(rel), "cross-session release refused: " + rel);
    check(json_str(rel, "reason") == "not_owned_by_session", "typed reason is not_owned_by_session");

    // NOTHING MUTATED — the part the live bug got wrong.
    check(g_session_registry.owner_of(kTheirs) == b, "b still owns the character");
    check(g_personality_locks.count(kTheirs) == 1, "b's personality lock still in place");
    check(g_council_lock_removals.empty(), "no council lock was cleared");
    check(ai_enabled(theirs) == 0, "b's native AI was NOT re-enabled behind its back");
    {
        std::lock_guard<std::mutex> lk(g_controlled_mutex);
        bool present = std::find(g_controlled_chars.begin(), g_controlled_chars.end(), kTheirs)
                       != g_controlled_chars.end();
        check(present, "b's character is still in the roster");
        check(g_controlled_chars.size() == roster_before,
              "roster size unchanged by the refusal (" + std::to_string(roster_before)
              + ")");
    }

    // ...and b can still release it, which is the practical cost of the bug:
    // after a "failed" release the claim used to be gone for good.
    {
        Scoped sb(b);
        std::string j;
        const bool r = do_release_character(params(kTheirs).c_str(), &j);
        check(r && ok(j), "the true owner can still release afterwards: " + j);
    }
    check(ai_enabled(theirs) == 1, "native AI re-enabled on the real release");
}

// A character nobody has ever claimed: ck-60bz's original case. Must still be a
// typed refusal, and must not touch the one character that IS claimed.
static void test_never_claimed_character_refused(std::unordered_map<uint32_t, FakeChar>& chars) {
    std::cout << "[3] a never-claimed character is refused; the claimed one is untouched\n";
    reset_world();
    const uint32_t kMine = 401;
    FakeChar& mine = chars[kMine];

    Session* s = g_session_registry.create("agent-a");
    Scoped scope(s);
    { std::string j; must(do_set_controlled_character(params(kMine, ",\"disable_native_ai\":true").c_str(), &j), "setup take"); }
    lock_personality(kMine);

    std::string rel;
    bool released = do_release_character(params(999999).c_str(), &rel);
    check(!released && !ok(rel), "unclaimed release refused: " + rel);

    check(g_session_registry.owner_of(kMine) == s, "the claimed character is still owned");
    check(g_personality_locks.count(kMine) == 1, "its personality lock is intact");
    check(ai_enabled(mine) == 0, "its native AI is still disabled");
    check(g_council_lock_removals.empty(), "no council lock was cleared");
    {
        std::lock_guard<std::mutex> lk(g_controlled_mutex);
        check(g_controlled_chars.size() == 1, "roster unchanged");
    }
    check(g_controlled_primary.load() == kMine, "primary unchanged");
}

// The ck-60bz roster-less-primary tolerance: a character that is the global
// primary but absent from the roster must still be releasable on the LEGACY
// (no session) path. Keeps the fix from narrowing an existing contract.
static void test_rosterless_primary_still_releasable(std::unordered_map<uint32_t, FakeChar>& chars) {
    std::cout << "[4] legacy path: a roster-less primary is still releasable\n";
    reset_world();
    const uint32_t kId = 501;
    FakeChar& fc = chars[kId];
    host_ai_char_toggle(reinterpret_cast<void*>(fc.ai_ctrl), 0);

    // No ClientContext installed => no session => legacy N=1 path.
    g_controlled_primary.store(kId);
    lock_personality(kId);
    {
        std::lock_guard<std::mutex> lk(g_controlled_mutex);
        g_controlled_chars.clear();          // primary present, roster empty
    }

    std::string rel;
    const bool r = do_release_character(params(kId).c_str(), &rel);
    check(r && ok(rel), "roster-less primary released: " + rel);
    check(g_personality_locks.count(kId) == 0, "personality lock cleared");
    check(ai_enabled(fc) == 1, "native AI re-enabled");
    check(g_controlled_primary.load() == 0, "primary cleared");
}

// ck-6kvq.10 §2.7 is DELIBERATE and must survive the reordering: under an armed
// checkpoint gate the claim is dropped but ai_ctrl+0x28 stays disabled.
static void test_armed_gate_keeps_ai_disabled(std::unordered_map<uint32_t, FakeChar>& chars) {
    std::cout << "[5] ck-6kvq.10: an ARMED checkpoint gate keeps native AI off\n";
    reset_world();
    const uint32_t kId = 601;
    FakeChar& fc = chars[kId];

    Session* s = g_session_registry.create("agent-a");
    Scoped scope(s);
    {
        std::string j;
        // ck-cwal: a SECOND claim, so the release below is not the seat's last.
        // This test is about what an ARMED checkpoint gate does to the native-AI
        // re-enable on release; a refused release would never reach that code.
        must(do_set_controlled_character(params(kId, ",\"disable_native_ai\":true").c_str(), &j), "setup take");
        //: ck-cwal: via the REGISTRY — a second take would SWITCH, not add.
        std::string why;
        must(g_session_registry.claim_character(s, 602u, &why), "setup: a second claim");
    }
    lock_personality(kId);
    arm_claim_pin(true);

    std::string rel;
    const bool r = do_release_character(params(kId).c_str(), &rel);
    check(r && ok(rel), "release succeeds under an armed gate: " + rel);
    check(g_session_registry.owner_of(kId) == nullptr, "the claim was dropped");
    check(g_personality_locks.count(kId) == 0, "personality lock still cleared");
    check(ai_enabled(fc) == 0, "ai_ctrl+0x28 deliberately left DISABLED (R3, §2.7)");
}

// The sibling handler. A take that is going to be REFUSED must not have already
// disabled the engine's AI for the character (ck-sozz, same defect class).
static void test_set_controlled_refusal_does_not_touch_ai(
        std::unordered_map<uint32_t, FakeChar>& chars) {
    std::cout << "[6] do_set_controlled_character: a refused take leaves the AI alone\n";

    // (a) cross-session refusal — the claim itself is refused, so nothing
    //     downstream of it ever runs.
    reset_world();
    const uint32_t kTheirs = 702;
    FakeChar& theirs = chars[kTheirs];
    Session* a = g_session_registry.create("agent-a");
    Session* b = g_session_registry.create("agent-b");
    { Scoped sb(b); std::string j; must(do_set_controlled_character(params(kTheirs).c_str(), &j), "setup take"); }
    check(ai_enabled(theirs) == 1, "b took it without disabling the AI");
    // (rebuild_legacy_mirrors sets agent_mode from "is anything claimed", so
    //  snapshot it rather than assuming false.)
    const bool agent_mode_before = g_agent_mode.load();
    std::string take;
    {
        Scoped sa(a);
        const bool took = do_set_controlled_character(
            params(kTheirs, ",\"disable_native_ai\":true").c_str(), &take);
        check(!took && !ok(take), "cross-session take refused: " + take);
    }
    check(json_str(take, "reason") == "owned_by_other", "typed reason is owned_by_other");
    check(ai_enabled(theirs) == 1, "the refused take did NOT disable the engine's AI");
    check(g_agent_mode.load() == agent_mode_before, "the refused take did NOT flip agent_mode");
    check(g_session_registry.owner_of(kTheirs) == b, "ownership unchanged");

    // (b) THE DISCRIMINATING CASE: a take that gets past the claim and is then
    //     refused by the roster check.  The legacy (no-session) path refuses
    //     with watchlist_full once g_controlled_chars is at MAX_CONTROLLED_WATCHLIST.
    //     Pre-fix, the global primary had already been rewritten and the
    //     engine's AI for the character already switched OFF by the time that
    //     refusal was written — a take that "failed" left an inert character
    //     behind.  Post-fix both happen only after the roster check passes.
    reset_world();
    const uint32_t kMine = 701;
    FakeChar& mine = chars[kMine];
    {
        std::lock_guard<std::mutex> lk(g_controlled_mutex);
        for (uint32_t i = 0; i < MAX_CONTROLLED_WATCHLIST; ++i)
            g_controlled_chars.push_back(900 + i);
    }
    g_controlled_primary.store(900);

    std::string full;
    // No ClientContext installed => legacy path.
    const bool took_full = do_set_controlled_character(
        params(kMine, ",\"disable_native_ai\":true").c_str(), &full);
    check(!took_full && !ok(full), "take over a full watchlist refused: " + full);
    check(json_str(full, "reason") == "watchlist_full", "typed reason is watchlist_full");
    check(ai_enabled(mine) == 1,
          "the refused take did NOT disable the engine's AI (pre-fix left it OFF forever)");
    check(g_controlled_primary.load() == 900,
          "the refused take did NOT rewrite the global primary");
}

// ck-cwal: AN OBSERVE SESSION IS UNCHANGED BY THE LAST-CLAIM REFUSAL.
//
// The bead names this case, and it is not redundant with the observe gate already
// at INSPECT 1: the point is that the new refusal did not become the answer for
// observe sessions. An observe seat must still be told `observe_mode`, because
// "you cannot release, you are observing" and "you cannot release your last
// character" are different facts with different fixes — and an observe session
// holds no claims, so a guard keyed on the claim set could plausibly have reached
// it first.
static void test_observe_session_unchanged(
        std::unordered_map<uint32_t, FakeChar>& chars) {
    std::cout << "[7] ck-cwal: an observe session still gets observe_mode, not the "
                 "last-claim refusal\n";
    reset_world();
    (void)chars;
    Session* o = g_session_registry.create("agent-observer", Session::Mode::OBSERVE);
    Scoped scope(o);
    std::string rel;
    const bool released = do_release_character(params(401u).c_str(), &rel);
    check(!released && !ok(rel), "observe release refused: " + rel);
    check(json_str(rel, "reason") == "observe_mode",
          "typed reason is observe_mode, NOT last_claim_of_control_session");
    check(rel.find("last_claim_of_control_session") == std::string::npos,
          "the last-claim refusal did not displace the observe gate");
}

int main() {
#ifdef CK3_RELEASE_LEGACY_ORDER
    std::cout << "=== ck-sozz NEGATIVE CONTROL "
                 "(dispatch_router.cpp built with -DCK3_RELEASE_LEGACY_ORDER) ===\n"
                 "Expect [1] to fail exactly as the live bridge did on 2026-09-10:\n"
                 "the ownership release runs first, the roster guard then reports the\n"
                 "state its predecessor created, and the cleanup half never runs.\n\n";
#else
    std::cout << "=== ck-sozz release_character handler ordering ===\n\n";
#endif

    // ck-cwal added 30745, 303 and 602: a control seat's LAST-character release is
    // now refused, so three tests that verify the CLEANUP half of a release need a
    // second claim to keep the release they are about from being the last one. The
    // fake world is built from this list, so a character not named here is
    // character_not_found however it is referenced later.
    std::vector<uint32_t> ids = {30744, 30745, 301, 302, 303, 401, 501, 601, 602,
                                 701, 702};
    auto chars = build_image(ids);

    // The library's engine entry points, aimed at host stand-ins.
    g_char_is_alive  = &host_char_is_alive;
    g_get_ai_ctrl    = &host_get_ai_ctrl;
    g_ai_char_toggle = &host_ai_char_toggle;

    test_owned_character_releases(chars);
    test_unowned_character_refused_without_mutating(chars);
    test_observe_session_unchanged(chars);
    test_never_claimed_character_refused(chars);
    test_rosterless_primary_still_releasable(chars);
    test_armed_gate_keeps_ai_disabled(chars);
    test_set_controlled_refusal_does_not_touch_ai(chars);

    std::cout << "\n";
    if (g_failures) {
#ifdef CK3_RELEASE_LEGACY_ORDER
        std::cout << "NEGATIVE CONTROL REPRODUCED THE BUG: " << g_failures
                  << " assertion(s) failed, as required.\n";
#else
        std::cout << "FAILED: " << g_failures << " assertion(s).\n";
#endif
        return 1;
    }
#ifdef CK3_RELEASE_LEGACY_ORDER
    std::cout << "NEGATIVE CONTROL DID NOT REPRODUCE THE BUG — the test is not "
                 "measuring the ordering it claims to measure.\n";
    return 1;
#else
    std::cout << "All release-handler ordering assertions passed.\n";
    return 0;
#endif
}
