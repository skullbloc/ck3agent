// test_fault_reply.cpp — ff3 N1 (witness #9 minor 1). NO GAME.
//
// THE CLAIM: a read that faulted on a path the reader thread serves inline is
// answered, never passed off as a 0. safe_read answers T{} on a fault; before
// this fix the swept handlers never looked at the fault flag, so an unreadable
// player id read as player 0 (the player guard then guarded nothing, and
// set_controlled_character would take the player), get_player_character
// answered player_character_id 0, and get_bridge_state said nothing.
//
// Links the REAL dispatch_router.cpp (the test_release_handler scaffolding:
// weak stubs for the handlers other TUs define). The fault is real: the page
// holding PLAYER_CHAR_ID in a synthetic game image is PROT_NONE, so
// process_vm_readv answers EFAULT exactly as it does for a freed page, while
// the range table still lists it (is_mapped says yes, as for storage freed
// since the last refresh).
//
//   [1] the reply: answer_memory_access_fault() answers the one string the
//       reader's sigsetjmp recovery answers;
//   [2] FaultScope: a reply left by a handler whose read faulted says
//       degraded (and one with no fault, or that already says so, is untouched);
//       [2b] it nests: the caller's fault record survives it (L14 S4);
//   [3] the handlers: get_player_character, get_controlled_roster and
//       set_controlled_character answer memory_access_fault on an unreadable
//       player id (and the take does not happen); get_bridge_state says degraded.
//   [4] a write handler (ff4 P1, witness #10's REJECT): a read that faults
//       BEFORE the store refuses (memory_access_fault) and the store is not
//       made, read back from the byte; a read that faults AFTER the store (the
//       read-back) answers success with degraded, the fault count and what was
//       written (verified:false), never a refusal -- set_ai_personality,
//       toggle_all_ai, toggle_ai_subsystem. "After" is a write-only page: the
//       CPU store lands and process_vm_readv answers EFAULT (no VM_READ), the
//       read-back faulting while the write did not. The toggles store an image
//       global no read decides, so they have no "before".
//   [5] the reply the dispatch wrapper makes for a handler that faulted and
//       left none says degraded (witness #10 R1 shape (a)), and
//       get_offset_validation's own verdict is offsets_degraded, so the fault
//       marker is not suppressed by it (shape (b)).
//   [6] release_character's degraded reply says whether the character's native
//       AI was handed back (witness #10 minor 8).
//   [7] declare_war's verdict after its dispatch: a war count that could not be
//       read is war_unverified, never dispatched_but_no_war (minor 9).
//   [8] the dynasty name: NAME (+0x80) only (L14 S4; the custom-name branch
//       deleted in L14 FB M-2: +0x110 overlaps HOUSES_ARRAY).
//   [9] the house (NAME +0x248), culture (+0x260/+0x1C8) and faith
//       (FAITH_DB keyed at +0x08; DISPLAY_NAME/FAITH_KEY) readers, and
//       emit_char_names through them (L14 FB M-1).
//
// CONTROLS (run by hand, recorded in the commits): this source against the
// pre-fix dispatch_router.cpp (blob of cleanup/final-fix 720e5757) fails [3];
// against the 5bf5cc7b personality.cpp and dispatch_router.cpp it fails [4]
// and [5].
#include "action_dispatch_internal.h"
#include "session.h"
#include "pipeline.h"
#include "intent_queue.h"
#include "name_tables.h"
#include "snapshot.h"
#include "episode_policy.h"
#include "personality_locks.h"
#include "declare_war_outcome.h"

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

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
void clock_write(ClockWrite, ClockOrigin, int) {}
const char* session_state_string() { return "test"; }
std::string get_cached_character_name(uint32_t) { return "(test)"; }
std::string enumerate_interactions() { return "[]"; }
std::string enumerate_decisions()    { return "[]"; }
bool get_client_lock_info(ClientLockInfo*) { return false; }
bool is_game_state_query(const char*) { return false; }
const char* offset_validation_group_name(int) { return ""; }
// [5]: where validate_offsets reads (readable unless a section points it at
// the PROT_NONE page).
uintptr_t g_validate_reads = 0;
OffsetValidationResult validate_offsets() {
    if (g_validate_reads) (void)safe_read<uint32_t>(g_validate_reads);
    return OffsetValidationResult{};
}
// personality.cpp's engine-bound neighbours (hooks, the pipeline, the reader).
void* install_hook(uintptr_t, void*) { return nullptr; }
void pipeline_note_episode_open(const char*) {}
std::vector<std::string> ipc_reconcile_declared_world(const WorldIdentity&, uint64_t) { return {}; }
// [5]: a handler bracketed by the query meta-router's FaultScope that reads a
// faulting address and returns false with no reply (query_council's shape when
// the character does not resolve).
uintptr_t g_query_reads = 0;
bool do_query_character(const char*, std::string*) {
    (void)safe_read<uint32_t>(g_query_reads);
    return false;
}
// Defined in dispatch_router.cpp, named only by its dispatch table.
bool do_get_player_character(const char* params_json, std::string* out);
bool do_get_controlled_roster(const char* params_json, std::string* out);
}  // namespace bridge

using namespace bridge;

namespace {

int g_failures = 0;
void check(bool ok, const std::string& what) {
    std::cout << (ok ? "    ok   " : "    FAIL ") << what << "\n";
    if (!ok) ++g_failures;
}

constexpr size_t    kImageSize = 0x0D000000;   // covers CHARACTER_DB @ ~0x0BECF088
constexpr uintptr_t kTableOff  = 0x0C000000;
constexpr uintptr_t kDbOff     = 0x0C800000;
constexpr uintptr_t kSentinel  = 0x0C900000;
constexpr uintptr_t kCharOff   = 0x0CA00000;
constexpr uintptr_t kCoreOff   = 0x0CB00000;   // character 301's core_data
constexpr uintptr_t kPersOff   = 0x0CC00000;   // its personality bytes (own page)
constexpr uintptr_t kNoneOff   = 0x0CD00000;   // [5]: a page no section makes readable
constexpr uintptr_t kDynOff    = 0x0CE00000;   // [8]: a stand-in dynasty object
constexpr uintptr_t kNameOff   = 0x0CF00000;   // [9]: stand-in house/culture/faith DBs and objects
constexpr uint32_t  kCapacity  = 4096;
constexpr uint32_t  kChar      = 301;

template <typename T> void poke(uintptr_t addr, T v) { std::memcpy(reinterpret_cast<void*>(addr), &v, sizeof v); }

uintptr_t page_of(uintptr_t a) {
    const uintptr_t pg = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    return a & ~(pg - 1);
}

// One character (kChar) in a stand-in CHARACTER_DB; PLAYER_CHAR_ID's page PROT_NONE.
uintptr_t build_image() {
    void* p = mmap(nullptr, kImageSize, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { std::cout << "SETUP FAILED: mmap\n"; std::exit(2); }
    const uintptr_t img = reinterpret_cast<uintptr_t>(p);
    g_base_addr = img;
    {
        std::unique_lock<std::shared_mutex> lk(g_ranges_mutex);
        g_valid_ranges.assign(1, MemRange{img, img + kImageSize});
    }
    poke<uintptr_t>(img + ck3::globals::CHARACTER_DB, img + kDbOff);
    poke<uintptr_t>(img + ck3::globals::CHAR_DB_SENTINEL, img + kSentinel);
    poke<uint32_t>(img + kDbOff + ck3::hash_map::CAPACITY, kCapacity);
    poke<uintptr_t>(img + kDbOff + ck3::hash_map::TABLE_PTR, img + kTableOff);
    const uintptr_t c = img + kCharOff;
    poke<uint32_t>(c + ck3::character::ID, kChar);
    poke<uintptr_t>(img + kTableOff + ck3::hash_map::ENTRY_HEADER_SIZE
                        + static_cast<uintptr_t>(kChar) * ck3::hash_map::ENTRY_STRIDE, c);
    const uintptr_t pid_page = page_of(img + ck3::globals::PLAYER_CHAR_ID);
    if (page_of(img + ck3::globals::CHARACTER_DB) == pid_page ||
        page_of(img + ck3::globals::CHAR_DB_SENTINEL) == pid_page) {
        std::cout << "SETUP FAILED: the character DB globals share PLAYER_CHAR_ID's page\n";
        std::exit(2);
    }
    if (mprotect(reinterpret_cast<void*>(pid_page), static_cast<size_t>(sysconf(_SC_PAGESIZE)),
                 PROT_NONE) != 0) {
        std::cout << "SETUP FAILED: mprotect\n"; std::exit(2);
    }
    return img;
}

bool host_char_is_alive(void*) { return true; }

void protect(uintptr_t addr, int prot) {
    const size_t pg = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    if (mprotect(reinterpret_cast<void*>(page_of(addr)), pg, prot) != 0) {
        std::cout << "SETUP FAILED: mprotect\n"; std::exit(2);
    }
}

// The byte at addr, read with the page made readable for the read (the test's
// own witness of the store, not the handler's).
int8_t byte_at(uintptr_t addr, int prot_after) {
    protect(addr, PROT_READ | PROT_WRITE);
    int8_t v;
    std::memcpy(&v, reinterpret_cast<void*>(addr), 1);
    protect(addr, prot_after);
    return v;
}

// A write-only page: the CPU store lands, process_vm_readv answers EFAULT.
// Checked, not assumed (a kernel that reads it would make "after" vacuous).
void write_only(uintptr_t addr) {
    protect(addr, PROT_WRITE);
    *reinterpret_cast<volatile int8_t*>(addr) = *reinterpret_cast<volatile int8_t*>(addr);
    safe_read_clear();
    (void)safe_read<uint8_t>(addr);
    if (!safe_read_had_fault()) {
        std::cout << "SETUP FAILED: a PROT_WRITE page reads through process_vm_readv\n";
        std::exit(2);
    }
    safe_read_clear();
}

int axis_index(const char* name) {
    for (int i = 0; i < PERSONALITY_AXIS_COUNT; ++i)
        if (std::strcmp(PERSONALITY_AXIS_NAMES[i], name) == 0) return i;
    std::cout << "SETUP FAILED: no axis " << name << "\n"; std::exit(2);
}

bool has(const std::string& s, const char* needle) { return s.find(needle) != std::string::npos; }

// A handler-shaped function bracketed by FaultScope (section [2]).
bool scoped_reply(uintptr_t read_at, const char* reply, std::string* out) {
    FaultScope fault_scope("test_fault_reply", out);
    (void)safe_read<uint32_t>(read_at);
    *out = reply;
    return true;
}

}  // namespace

int main() {
    std::cout << "=== ff3 N1: a read that faulted is answered, never a silent 0 ===\n";
    const uintptr_t img = build_image();
    g_char_is_alive = &host_char_is_alive;
    const uintptr_t unreadable = img + ck3::globals::PLAYER_CHAR_ID;
    const uintptr_t readable   = img + kCharOff;

    std::cout << "[1] the reply\n";
    {
        std::string out;
        const bool r = answer_memory_access_fault("test", &out);
        check(!r && out == "{\"error\":\"memory_access_fault\"}",
              "answer_memory_access_fault answers the reader's recovery string: " + out);
    }

    std::cout << "[2] FaultScope\n";
    {
        std::string out;
        scoped_reply(unreadable, "{\"success\":true,\"x\":0}", &out);
        check(out == "{\"success\":true,\"x\":0,\"degraded\":true,\"fault_count\":1}",
              "a reply after a faulted read says degraded: " + out);
        scoped_reply(readable, "{\"success\":true,\"x\":0}", &out);
        check(out == "{\"success\":true,\"x\":0}", "a reply with no fault is untouched: " + out);
        scoped_reply(unreadable, "{}", &out);
        check(out == "{\"degraded\":true,\"fault_count\":1}", "an empty object gains the keys: " + out);
        scoped_reply(unreadable, "{\"degraded\":false}", &out);
        check(out == "{\"degraded\":false}", "a reply that already carries degraded is untouched: " + out);
        scoped_reply(unreadable, MEMORY_ACCESS_FAULT_REPLY, &out);
        check(out == MEMORY_ACCESS_FAULT_REPLY, "the memory_access_fault reply is untouched: " + out);
    }

    std::cout << "[2b] FaultScope nests (L14 S4, audit 3(iii)): a handler another calls\n"
                 "     leaves its caller's fault record as it found it\n";
    {
        std::string inner;
        safe_read_clear();                              // the caller's own bracket
        (void)safe_read<uint32_t>(unreadable);          // the caller's read faults
        scoped_reply(readable, "{}", &inner);           // a clean nested handler
        check(safe_read_had_fault() && safe_read_fault_count() == 1,
              "the caller's fault survives a nested FaultScope (count " +
              std::to_string(safe_read_fault_count()) + ")");
        safe_read_clear();
        scoped_reply(unreadable, "{}", &inner);         // the nested handler's read faults
        check(safe_read_fault_count() == 1 && has(inner, "\"degraded\":true"),
              "the nested handler's fault is said in its own reply and stays on the record "
              "(the dispatch wrapper reads it, [5]): " + inner);
        safe_read_clear();
        (void)safe_read<uint32_t>(unreadable);
        scoped_reply(unreadable, "{}", &inner);
        check(safe_read_fault_count() == 2, "the caller's fault and the nested one, both on the record (count " +
              std::to_string(safe_read_fault_count()) + ")");
    }

    std::cout << "[3] the handlers, PLAYER_CHAR_ID unreadable\n";
    {
        std::string out;
        do_get_player_character("{}", &out);
        check(out == MEMORY_ACCESS_FAULT_REPLY, "get_player_character: " + out);

        out.clear();
        do_get_controlled_roster("{}", &out);
        check(out == MEMORY_ACCESS_FAULT_REPLY, "get_controlled_roster: " + out);

        out.clear();
        const bool took = do_set_controlled_character("{\"character_id\":301}", &out);
        check(!took && out == MEMORY_ACCESS_FAULT_REPLY,
              "set_controlled_character refuses (the player guard read nothing): " + out);
        {
            std::lock_guard<std::mutex> lk(g_controlled_mutex);
            check(g_controlled_chars.empty() && g_controlled_primary.load() == 0,
                  "and nothing was taken (roster empty, primary 0)");
        }

        out.clear();
        do_get_bridge_state("{}", &out);
        check(out.find("\"degraded\":true") != std::string::npos,
              "get_bridge_state says degraded (player_character_id read nothing)");
    }

    std::cout << "[4] a write handler: a fault before the store refuses, after it is said\n";
    {
        char lock_path[] = "/tmp/test_fault_reply_locks_XXXXXX";
        const int fd = mkstemp(lock_path);
        if (fd < 0) { std::cout << "SETUP FAILED: mkstemp\n"; return 2; }
        close(fd);
        set_personality_locks_path(lock_path);
        const uintptr_t c = img + kCharOff, core = img + kCoreOff, pers = img + kPersOff;
        poke<uintptr_t>(c + ck3::character::REALM_DATA, core);
        poke<uintptr_t>(core + ck3::core_data::PERSONALITY_PTR, pers);
        poke<int64_t>(core + ck3::core_data::GOLD, 12300000);
        for (int i = 0; i < 9; ++i) poke<int8_t>(pers + i, 7);
        const int bold = axis_index("boldness");
        auto locked = [](uint32_t cid) {
            std::shared_lock<std::shared_mutex> lk(g_personality_lock_mutex);
            return g_personality_locks.count(cid) != 0;
        };

        // (a) BEFORE, the player id (the player guard's read) unreadable.
        std::string out;
        bool ok = do_set_ai_personality("{\"character_id\":301,\"boldness\":42}", &out);
        check(!ok && out == MEMORY_ACCESS_FAULT_REPLY, "set_ai_personality, player id unreadable: " + out);
        check(byte_at(pers + bold, PROT_READ | PROT_WRITE) == 7 && !locked(kChar),
              "and nothing was written (the axis byte still 7, no lock taken)");

        // (b) BEFORE, the personality pointer (the address the store goes to) unreadable.
        protect(img + ck3::globals::PLAYER_CHAR_ID, PROT_READ | PROT_WRITE);
        poke<int32_t>(img + ck3::globals::PLAYER_CHAR_ID, 0xFFFFFF);   // observer: no human player
        protect(core, PROT_NONE);
        out.clear();
        ok = do_set_ai_personality("{\"character_id\":301,\"boldness\":42}", &out);
        check(!ok && out == MEMORY_ACCESS_FAULT_REPLY, "set_ai_personality, personality pointer unreadable: " + out);
        protect(core, PROT_READ | PROT_WRITE);
        check(byte_at(pers + bold, PROT_READ | PROT_WRITE) == 7 && !locked(kChar),
              "and nothing was written (the axis byte still 7, no lock taken)");

        // (c) AFTER: the read-back of the axes faults; the store has landed.
        write_only(pers);
        out.clear();
        ok = do_set_ai_personality("{\"character_id\":301,\"boldness\":42}", &out);
        check(ok && has(out, "\"success\":true") && has(out, "\"degraded\":true")
                 && has(out, "\"fault_count\":9") && has(out, "\"personality\":null")
                 && has(out, "\"verified\":false") && has(out, "\"written\":{\"boldness\":42}")
                 && !has(out, "memory_access_fault"),
              "set_ai_personality, read-back unreadable: success, degraded, what was written: " + out);
        check(byte_at(pers + bold, PROT_READ | PROT_WRITE) == 42 && locked(kChar),
              "and the store landed (the axis byte 42, the lock taken)");
        out.clear();
        do_set_ai_personality("{\"character_id\":301,\"boldness\":43}", &out);
        check(has(out, "\"personality\":{") && has(out, "\"boldness\":43") && !has(out, "degraded")
                  && !has(out, "verified"),
              "control, nothing unreadable: the axes as read back, no degraded: " + out);

        // toggle_all_ai and toggle_ai_subsystem, the read-back unreadable.
        Session* sess = g_session_registry.create("ff4");
        ClientContext ctx{sess, nullptr, nullptr};
        ClientContextGuard guard(&ctx);
        const uintptr_t all_ai = img + ck3::globals::AI_GLOBAL_TOGGLE;
        poke<uint8_t>(all_ai, 1);
        write_only(all_ai);
        out.clear();
        ok = dispatch_action_with_response("toggle_all_ai", "{\"enabled\":false}", &out);
        check(ok && has(out, "\"success\":true") && has(out, "\"byte_after\":null")
                 && has(out, "\"byte_written\":0") && has(out, "\"verified\":false")
                 && has(out, "\"degraded\":true") && !has(out, "memory_access_fault"),
              "toggle_all_ai, read-back unreadable: success, degraded, the byte written: " + out);
        check(byte_at(all_ai, PROT_READ | PROT_WRITE) == 0, "and the store landed (the byte 0)");
        out.clear();
        dispatch_action_with_response("toggle_all_ai", "{\"enabled\":true}", &out);
        check(has(out, "\"byte_after\":1") && !has(out, "degraded"),
              "control, readable: byte_after read back, no degraded: " + out);

        const uintptr_t decisions = img + ck3::globals::AI_TOGGLE_BASE;   // "Decisions", the first toggle
        poke<uint8_t>(decisions, 1);
        write_only(decisions);
        out.clear();
        ok = dispatch_action_with_response("toggle_ai_subsystem",
                                           "{\"subsystem\":\"Decisions\",\"enabled\":false}", &out);
        check(ok && has(out, "\"success\":true") && has(out, "\"byte_after\":null")
                 && has(out, "\"byte_written\":0") && has(out, "\"verified\":false")
                 && has(out, "\"degraded\":true") && !has(out, "memory_access_fault"),
              "toggle_ai_subsystem, read-back unreadable: success, degraded, the byte written: " + out);
        check(byte_at(decisions, PROT_READ | PROT_WRITE) == 0, "and the store landed (the byte 0)");
        out.clear();
        dispatch_action_with_response("toggle_ai_subsystem", "{\"subsystem\":\"Decisions\",\"enabled\":true}",
                                      &out);
        check(has(out, "\"byte_after\":1") && !has(out, "degraded"),
              "control, readable: byte_after read back, no degraded: " + out);

        std::cout << "[5] a fault the reply would not say\n";
        const uintptr_t none = img + kNoneOff;
        protect(none, PROT_NONE);
        g_query_reads = none;
        out.clear();
        ok = dispatch_action_with_response("query", "{\"type\":\"character\"}", &out);
        check(!ok && has(out, "\"success\":false") && has(out, "\"degraded\":true"),
              "a handler that faulted and left no reply: the wrapper's reply says degraded: " + out);
        g_query_reads = readable;
        out.clear();
        dispatch_action_with_response("query", "{\"type\":\"character\"}", &out);
        check(has(out, "\"success\":false") && !has(out, "degraded"),
              "control, nothing unreadable: the wrapper's reply says nothing of a fault: " + out);
        g_validate_reads = none;
        out.clear();
        dispatch_action_with_response("get_offset_validation", "{}", &out);
        check(has(out, "\"offsets_degraded\":false") && has(out, "\"degraded\":true"),
              "get_offset_validation: its verdict is offsets_degraded, and a faulted read says degraded: " + out);
        g_validate_reads = 0;
        out.clear();
        dispatch_action_with_response("get_offset_validation", "{}", &out);
        check(has(out, "\"offsets_degraded\":false") && !has(out, "\"degraded\""),
              "control, nothing unreadable: no degraded key: " + out);
        set_personality_locks_path(nullptr);
        std::remove(lock_path);
    }

    std::cout << "[6] release_character after a read that faulted\n";
    {
        const uintptr_t c = img + kCharOff;
        poke<uintptr_t>(c + ck3::character::REALM_DATA, img + kNoneOff);   // its controller read faults
        {
            std::lock_guard<std::mutex> lk(g_controlled_mutex);
            g_controlled_chars.assign(1, kChar);
        }
        std::string out;
        bool ok = do_release_character("{\"character_id\":301}", &out);
        check(ok && has(out, "\"success\":true") && has(out, "\"native_ai_reenabled\":false")
                 && has(out, "\"degraded\":true"),
              "released, degraded, and the native AI not handed back is said: " + out);
        poke<uintptr_t>(c + ck3::character::REALM_DATA, 0);   // unlanded: nothing to read
        {
            std::lock_guard<std::mutex> lk(g_controlled_mutex);
            g_controlled_chars.assign(1, kChar);
        }
        out.clear();
        ok = do_release_character("{\"character_id\":301}", &out);
        check(ok && !has(out, "degraded") && !has(out, "native_ai_reenabled"),
              "control, nothing unreadable: released, no degraded: " + out);
    }

    std::cout << "[7] declare_war's verdict after its dispatch\n";
    {
        auto is = [](DeclareWarOutcome o, bool success, const char* status) {
            return o.success == success && std::strcmp(o.status, status) == 0;
        };
        check(is(declare_war_outcome(true, true, false, true), false, "war_unverified"),
              "the pipeline ran and the count could not be read: war_unverified");
        check(is(declare_war_outcome(true, true, false, false), false, "dispatched_but_no_war"),
              "the pipeline ran and the count read unchanged: dispatched_but_no_war");
        check(is(declare_war_outcome(true, true, true, false), true, "war_created"), "a war: war_created");
        check(is(declare_war_outcome(false, false, false, true), false, "timeout"), "no result: timeout");
        check(is(declare_war_outcome(true, false, false, true), false, "rejected"), "rejected: rejected");
    }

    std::cout << "[8] the dynasty name (L14 S4, FB M-2): NAME, whatever +0x110/+0x130 hold\n";
    {
        // A stand-in dynasty object inside the image (its range is mapped), with
        // libstdc++ string control blocks {data ptr, size} pointing at text in it.
        // +0x110 holds a string block and +0x130 is set: the reader must not
        // look (in the ivar867 dump +0x110 is an image pointer and +0x118 the
        // houses array in all 12,025 dynasties).
        const uintptr_t d = img + kDynOff, txt = img + kDynOff + 0x400;
        std::memcpy(reinterpret_cast<void*>(txt), "Karling", 8);
        std::memcpy(reinterpret_cast<void*>(txt + 0x40), "Carolingian", 12);
        poke<uintptr_t>(d + ::dynasty_obj::NAME, txt);
        poke<uint64_t>(d + ::dynasty_obj::NAME + 8, 7);
        poke<uintptr_t>(d + 0x110, txt + 0x40);
        poke<uint64_t>(d + 0x118, 11);
        poke<uint8_t>(d + 0x130, 1);
        check(dynasty_name(d) == "Karling",
              "+0x130 set, +0x110 holds text: still NAME: " + dynasty_name(d));
        poke<uint64_t>(d + ::dynasty_obj::NAME + 8, 0);
        check(dynasty_name(d).empty(), "NAME empty: \"\" (no stand-in word, no other offset)");
        check(dynasty_name(0).empty(), "no dynasty object: \"\"");
    }

    std::cout << "[9] house, culture and faith names (L14 FB M-1): the readers' offsets and lookups\n";
    {
        // Stand-in HOUSE_DB / CULTURE_INDEXED_DB / FAITH_DB, one object each,
        // shaped as the ivar867 dump has them: a house and a culture carry their
        // id at +0x10 (ENTRY_KEY_REF), a faith at +0x08 with a pointer at +0x10.
        const uintptr_t B = img + kNameOff;
        auto str = [&](uintptr_t at, uintptr_t text, const char* v) {
            std::memcpy(reinterpret_cast<void*>(text), v, std::strlen(v) + 1);
            poke<uintptr_t>(at, text);
            poke<uint64_t>(at + 8, std::strlen(v));
        };
        auto db = [&](uintptr_t global, uintptr_t dbs, uintptr_t table, uint32_t id, uintptr_t obj) {
            poke<uintptr_t>(img + global, dbs);
            poke<uintptr_t>(dbs + ck3::hash_map::TABLE_PTR, table);
            poke<uint32_t>(dbs + ck3::hash_map::CAPACITY, 64);
            poke<uintptr_t>(table + ck3::hash_map::ENTRY_HEADER_SIZE
                                + static_cast<uintptr_t>(id) * ck3::hash_map::ENTRY_STRIDE, obj);
        };
        const uint32_t kHouse = 5, kCulture = 9, kFaith = 7;
        const uintptr_t house = B + 0x10000, cult = B + 0x11000, fth = B + 0x12000, ch = B + 0x13000;
        db(ck3::globals::HOUSE_DB, B + 0x000, B + 0x1000, kHouse, house);
        db(ck3::globals::CULTURE_INDEXED_DB, B + 0x100, B + 0x2000, kCulture, cult);
        db(ck3::globals::FAITH_DB, B + 0x200, B + 0x3000, kFaith, fth);
        poke<uint32_t>(house + ck3::hash_map::ENTRY_KEY_REF, kHouse);
        str(house + ::dynasty_house_obj::NAME, B + 0x20000, "Ivaring");
        poke<uint32_t>(cult + ck3::hash_map::ENTRY_KEY_REF, kCulture);
        str(cult + ::culture::NAME, B + 0x20100, "Norse");
        str(cult + ::culture::SCRIPT_KEY, B + 0x20200, "norse");
        poke<uint32_t>(fth + ::faith::FAITH_ID, kFaith);
        poke<uintptr_t>(fth + 0x10, img + 0x55aa50);          // the dump's image pointer
        str(fth + ::faith::FAITH_KEY, B + 0x20300, "norse_pagan");
        str(fth + ::faith::DISPLAY_NAME, B + 0x20400, "\xc3\x81satr\xc3\xba");
        poke<uint32_t>(ch + ck3::character::ID, 14859);
        poke<uint32_t>(ch + ck3::character::HOUSE_ID_DIRECT, kHouse);
        poke<uint32_t>(ch + ck3::character::CULTURE_ID_DIRECT, kCulture);
        poke<uint32_t>(ch + ck3::character::FAITH_ID_DIRECT, kFaith);

        check(house_name(house) == "Ivaring", "house_name reads NAME (+0x248): " + house_name(house));
        check(char_house_name(ch) == "Ivaring", "char_house_name: char -> HOUSE_DB -> NAME");
        check(house_name(0).empty(), "no house object: \"\"");
        const NamePair cn = culture_names(kCulture);
        check(cn.name == "Norse" && cn.key == "norse",
              "culture_names: NAME (+0x260) / SCRIPT_KEY (+0x1C8): " + cn.name + "/" + cn.key);
        check(ad_db_lookup(ck3::globals::FAITH_DB, kFaith) == 0,
              "control: ad_db_lookup (key at +0x10) does not find a faith");
        check(faith_lookup(kFaith) == fth, "faith_lookup finds it (key at +0x08)");
        check(faith_lookup(kFaith + 1) == 0, "faith_lookup: an absent id is 0");
        const NamePair fn = faith_names(kFaith);
        check(fn.name == "\xc3\x81satr\xc3\xba" && fn.key == "norse_pagan",
              "faith_names: DISPLAY_NAME (+0xC0) / FAITH_KEY (+0x98): " + fn.name + "/" + fn.key);
        ResponseBuilder r;
        r.begin_object();
        emit_char_names(r, ch);
        r.end_object();
        const std::string row = r.take();
        for (const char* want : {"\"house_name\":\"Ivaring\"", "\"culture\":\"Norse\"",
                                 "\"culture_key\":\"norse\"", "\"faith_key\":\"norse_pagan\"",
                                 "\"faith\":\""}) {
            check(row.find(want) != std::string::npos,
                  std::string("emit_char_names carries ") + want + ": " + row);
        }
    }

    std::cout << (g_failures ? "FAILED: " : "PASSED: ") << g_failures << " failure(s)\n";
    return g_failures ? 1 : 0;
}
