// CK3 LLM Agent Bridge DLL
// Injected into CK3 process via LD_PRELOAD to intercept AI tick loop
// and relay game state to an external LLM agent process.
#pragma once

#include "world_identity.h"

#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

#include "intent_queue.h"
#include "session.h"

namespace bridge {

// =============================================================================
// Runtime Address Resolution
// =============================================================================

// ASLR base address of the CK3 binary, resolved at init time.
// All addresses in ck3_offsets.h are added to this base.
extern uintptr_t g_base_addr;
// ck-65lv: one past the CK3 image's last PT_LOAD byte (0 = unknown; the fault
// formatters then assume base + 512 MB and SAY so).
extern uintptr_t g_image_end;

// Resolve a compile-time offset to a runtime address
inline void* resolve(uintptr_t offset) {
    return reinterpret_cast<void*>(g_base_addr + offset);
}

// Typed read from a resolved absolute address
template<typename T>
inline T read(uintptr_t addr) {
    return *reinterpret_cast<T*>(addr);
}

// Typed read with base-relative offset
template<typename T>
inline T read_offset(uintptr_t offset) {
    return *reinterpret_cast<T*>(g_base_addr + offset);
}

// =============================================================================
// Pointer Validation — /proc/self/maps based
// =============================================================================

// Cached valid memory ranges from /proc/self/maps.
// Updated once per tick by refresh_memory_map().
struct MemRange { uintptr_t start, end; };
extern std::vector<MemRange> g_valid_ranges;
extern std::shared_mutex g_ranges_mutex;

// Rebuild the valid memory ranges table from /proc/self/maps.
// Called once per tick from on_tick(). Thread-safe.
void refresh_memory_map();

// Check if an address falls within a mapped memory region.
// Uses binary search on sorted ranges + shared_lock for concurrent readers.
inline bool is_mapped(uintptr_t addr) {
    if (addr < 0x10000 || addr > 0x7FFFFFFFFFFF) return false;
    std::shared_lock<std::shared_mutex> rdlock(g_ranges_mutex);
    auto it = std::upper_bound(g_valid_ranges.begin(), g_valid_ranges.end(), addr,
        [](uintptr_t val, const MemRange& range) { return val < range.start; });
    if (it == g_valid_ranges.begin()) return false;
    --it;
    return addr < it->end;
}

// Check if a pointer-sized region starting at addr is fully mapped.
// Uses binary search on sorted ranges + shared_lock for concurrent readers.
inline bool is_mapped_range(uintptr_t addr, size_t len) {
    if (addr < 0x10000 || addr > 0x7FFFFFFFFFFF) return false;
    uintptr_t end_addr = addr + len;
    if (end_addr < addr) return false;  // overflow
    std::shared_lock<std::shared_mutex> rdlock(g_ranges_mutex);
    auto it = std::upper_bound(g_valid_ranges.begin(), g_valid_ranges.end(), addr,
        [](uintptr_t val, const MemRange& range) { return val < range.start; });
    if (it == g_valid_ranges.begin()) return false;
    --it;
    return addr >= it->start && end_addr <= it->end;
}

// =============================================================================
// safe_read fault flag (ck-udrre, Option 3 from ck-bv2ge)
//
// safe_read<T> returns T{} on failure with no error channel. Callers that
// need to distinguish "value is 0" from "memory was unreadable" can opt in
// to a thread-local fault flag: call safe_read_clear() at handler entry,
// run the reads, then check safe_read_had_fault() / safe_read_last_fault().
//
// The flag is bumped from inside safe_read<T> whenever process_vm_readv
// fails (covers both unmapped addresses and partial reads). Existing call
// sites that don't opt in continue to receive T{} with no behavior change.
// Per-thread so query handlers on the IPC reader thread don't race with
// observation hooks on the game thread.
// =============================================================================
inline thread_local int       g_safe_read_fault_count    = 0;
inline thread_local uintptr_t g_safe_read_last_fault_addr = 0;

inline void safe_read_clear() {
    g_safe_read_fault_count    = 0;
    g_safe_read_last_fault_addr = 0;
}
inline bool safe_read_had_fault() {
    return g_safe_read_fault_count > 0;
}
inline int safe_read_fault_count() {
    return g_safe_read_fault_count;
}
inline uintptr_t safe_read_last_fault() {
    return g_safe_read_last_fault_addr;
}

// Safe read using process_vm_readv — returns default T{} on unmapped memory.
// Never crashes, even on completely invalid addresses.
// Use for one-off reads where performance is not critical.
//
// Failure path also bumps g_safe_read_fault_count and records addr in
// g_safe_read_last_fault_addr (see safe_read_clear / safe_read_had_fault).
template<typename T>
inline T safe_read(uintptr_t addr) {
    T result{};
    struct iovec local = { &result, sizeof(T) };
    struct iovec remote = { reinterpret_cast<void*>(addr), sizeof(T) };
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != static_cast<ssize_t>(sizeof(T))) {
        g_safe_read_fault_count++;
        g_safe_read_last_fault_addr = addr;
        return T{};
    }
    return result;
}

// Safe read with base-relative offset — safe_read equivalent of read_offset.
template<typename T>
inline T safe_read_offset(uintptr_t offset) {
    return safe_read<T>(g_base_addr + offset);
}

// Safe read into caller-provided buffer. Returns true on success.
// Also bumps the safe_read fault flag on failure for parity with safe_read<T>.
inline bool safe_read_bytes(uintptr_t addr, void* out, size_t len) {
    struct iovec local = { out, len };
    struct iovec remote = { reinterpret_cast<void*>(addr), len };
    if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0) != static_cast<ssize_t>(len)) {
        g_safe_read_fault_count++;
        g_safe_read_last_fault_addr = addr;
        return false;
    }
    return true;
}

// =============================================================================
// Canonical guarded std::string reader (crash 20260625)
//
// Reads a CK3 std::__cxx11::basic_string<char> from GAME memory safely.
// Layout: [_M_p(8B ptr)][_M_string_length(8B size)][16B SSO buf / capacity].
// _M_p points at the data (inline SSO buffer at str_addr+0x10 for len<=15, or a
// heap buffer for len>15).
//
// The 2026-06-25 SIGSEGV was heap corruption: under heavy war/title churn one
// per-tick reader grabbed a BAD length from a transient/dangling game object and
// over-read past a heap chunk (the dump caught malloc walking a free-list whose
// fd-pointer had been clobbered with loc-key text "CK:CULTU…"). The hot fix is to
// make EVERY per-tick reader bound the copy to memory that is *actually*
// readable, and to NEVER construct std::string(ptr,size) by dereferencing game
// memory directly (which faults between the map-check and the copy).
//
// This guard:
//   1. rejects absurd sizes (0 or > a sane cap) — a garbage length never reaches
//      an allocation,
//   2. validates the data pointer is plausible and the FULL [ptr, ptr+size)
//      range is mapped,
//   3. copies through process_vm_readv (EFAULT-safe; returns false, never
//      faults, if the page vanished after the map-check),
//   4. returns false on ANY failure so the caller emits an EMPTY/skipped string.
//
// Cheap enough for the per-tick hot path: one length read, one mapped-range
// binary-search, one bounded process_vm_readv. Returns true on success with the
// (null-trimmed) string in `out`; false (out cleared) on any anomaly.
inline constexpr size_t kMaxGameStringLen = 4096;  // sane cap; real keys/names << this

inline bool safe_read_game_string(uintptr_t str_addr, std::string& out) {
    out.clear();
    // Plausible std::string control block?
    if (str_addr < 0x10000 || str_addr > 0x7FFFFFFFFFFF) return false;

    // Read the length field FIRST, via process_vm_readv (never dereference).
    uint64_t size = 0;
    {
        struct iovec local  = { &size, sizeof(size) };
        struct iovec remote = { reinterpret_cast<void*>(str_addr + 8), sizeof(size) };
        if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0)
                != static_cast<ssize_t>(sizeof(size))) {
            return false;
        }
    }
    // Reject garbage lengths: 0 (nothing to read) or anything beyond the cap
    // (a negative int that became a huge unsigned, or a clobbered field).
    if (size == 0 || size > kMaxGameStringLen) return false;

    // Read the data pointer (_M_p), again without dereferencing.
    uintptr_t data_ptr = 0;
    {
        struct iovec local  = { &data_ptr, sizeof(data_ptr) };
        struct iovec remote = { reinterpret_cast<void*>(str_addr), sizeof(data_ptr) };
        if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0)
                != static_cast<ssize_t>(sizeof(data_ptr))) {
            return false;
        }
    }
    if (data_ptr < 0x10000 || data_ptr > 0x7FFFFFFFFFFF) return false;

    // The FULL data range must be mapped per our /proc/self/maps snapshot…
    if (!is_mapped_range(data_ptr, size)) return false;

    // …and copy it through process_vm_readv, which is EFAULT-safe even if the
    // page was freed between the (per-tick stale) map snapshot and now. We NEVER
    // construct std::string directly from the unvalidated game pointer.
    char buf[kMaxGameStringLen];
    {
        struct iovec local  = { buf, size };
        struct iovec remote = { reinterpret_cast<void*>(data_ptr), size };
        if (process_vm_readv(getpid(), &local, 1, &remote, 1, 0)
                != static_cast<ssize_t>(size)) {
            return false;
        }
    }
    // Trim at first embedded NUL (CK3 strings are sized, but be defensive).
    size_t actual = 0;
    while (actual < size && buf[actual] != '\0') actual++;
    out.assign(buf, actual);
    return true;
}

// Convenience wrapper: returns the string by value (empty on any failure).
inline std::string safe_read_game_string(uintptr_t str_addr) {
    std::string s;
    safe_read_game_string(str_addr, s);
    return s;
}

// =============================================================================
// RAII mprotect wrapper — makes a memory region writable on construction,
// restores ORIGINAL permissions on destruction.
//
// Previous bug: destructor hardcoded R+X, which is wrong for .data.rel.ro
// pages (RELRO) that should be R-only. Now queries /proc/self/maps for the
// actual original permissions and restores them exactly.
// =============================================================================
class ScopedWritable {
    void* page_;
    size_t len_;
    int orig_prot_;  // original permissions to restore

    // Query /proc/self/maps for the actual permissions of a page
    static int query_page_prot(void* page_addr) {
        int prot = PROT_READ | PROT_EXEC;  // safe fallback for .text
        FILE* f = fopen("/proc/self/maps", "r");
        if (!f) return prot;
        uintptr_t target = reinterpret_cast<uintptr_t>(page_addr);
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            uintptr_t start, end;
            char perms[8];
            if (sscanf(line, "%lx-%lx %4s", &start, &end, perms) == 3) {
                if (target >= start && target < end) {
                    prot = 0;
                    if (perms[0] == 'r') prot |= PROT_READ;
                    if (perms[1] == 'w') prot |= PROT_WRITE;
                    if (perms[2] == 'x') prot |= PROT_EXEC;
                    break;
                }
            }
        }
        fclose(f);
        return prot;
    }

public:
    ScopedWritable(void* addr, size_t len) {
        long ps = sysconf(_SC_PAGESIZE);
        uintptr_t start = reinterpret_cast<uintptr_t>(addr) & ~(ps - 1);
        uintptr_t end = (reinterpret_cast<uintptr_t>(addr) + len + ps - 1) & ~(ps - 1);
        page_ = reinterpret_cast<void*>(start);
        len_ = end - start;
        orig_prot_ = query_page_prot(page_);
        mprotect(page_, len_, PROT_READ | PROT_WRITE | PROT_EXEC);
    }
    ~ScopedWritable() {
        mprotect(page_, len_, orig_prot_);
    }
    ScopedWritable(const ScopedWritable&) = delete;
    ScopedWritable& operator=(const ScopedWritable&) = delete;
};

// =============================================================================
// Permission-free memory write via /proc/self/mem
//
// Writes to any address in the current process without changing page permissions.
// Works on read-only pages (including RELRO vtable pages) because /proc/self/mem
// bypasses mprotect restrictions. This is the preferred method for vtable hooks
// since it leaves page permissions completely untouched.
//
// Returns true on success, false on failure.
// =============================================================================
inline bool write_to_readonly(void* addr, const void* data, size_t len) {
    int fd = open("/proc/self/mem", O_RDWR);
    if (fd < 0) return false;
    ssize_t written = pwrite(fd, data, len, static_cast<off_t>(reinterpret_cast<uintptr_t>(addr)));
    close(fd);
    return written == static_cast<ssize_t>(len);
}

// =============================================================================
// Function Pointer Types (for calling game internals)
// =============================================================================

// void FUN_04e1f190(long param_1, byte param_2)
// Sets AI enabled flag for a character's AI controller
using fn_ai_char_toggle = void(*)(void* ai_controller, uint8_t enabled);

// bool FUN_0674b2c0(long character)
// Returns true if character is alive (death_date ptr == null)
using fn_char_is_alive = bool(*)(void* character);

// long FUN_06780040(long character)
// Returns AI controller for a character (via realm data chain)
using fn_get_ai_controller = void*(*)(void* character);

// bool FUN_0664adf0(long character)
// Returns true if character is imprisoned (ruler_data->court->is_free check), 34 bytes
using fn_is_imprisoned = bool(*)(void* character);

// Council config accessor: long FUN_0677baf0(char_ptr) -> ptr to inline council config
using fn_get_council_config = void*(*)(void* character);

// Council instance accessor: long FUN_0677bdf0(char_ptr) -> council instance (nullable)
using fn_get_council_instance = void*(*)(void* character);

// Compute a battle score from raw war memory (safe — pure memory reads).
// Reads battle array at war_ptr+0x238/0x244, each battle entry has:
//   +0x40: casualty score (int64, fixed-point / 100000)
//   +0x48: side flag (byte, 0=defender casualties, 1=attacker casualties)
// Returns (attacker_casualties - defender_casualties) / 100000.0
// Positive = attacker winning, negative = defender winning.
// Also sets battle_count_out if non-null.
inline double compute_battle_score(uintptr_t war_ptr, int* battle_count_out = nullptr) {
    int32_t battle_count = safe_read<int32_t>(war_ptr + 0x244);
    if (battle_count_out) *battle_count_out = (battle_count > 0 && battle_count < 1000) ? battle_count : 0;
    if (battle_count <= 0 || battle_count >= 1000) return 0.0;

    uintptr_t battle_arr = safe_read<uintptr_t>(war_ptr + 0x238);
    if (!battle_arr) return 0.0;

    int64_t attacker_casualties = 0;
    int64_t defender_casualties = 0;
    for (int32_t i = 0; i < battle_count && i < 200; i++) {
        uintptr_t battle_ptr = safe_read<uintptr_t>(battle_arr + i * 8);
        if (!battle_ptr) continue;
        int64_t casualty_score = safe_read<int64_t>(battle_ptr + 0x40);
        uint8_t side_flag = safe_read<uint8_t>(battle_ptr + 0x48);
        if (side_flag == 1) {
            attacker_casualties += casualty_score;
        } else {
            defender_casualties += casualty_score;
        }
    }
    return static_cast<double>(attacker_casualties - defender_casualties) / 100000.0;
}

// Compute ticking war score from raw war memory (safe — pure memory reads).
// Reads attacker/defender ticking score accumulators (int64 fixed-point / 100000).
// Returns net ticking score: (attacker - defender) / 100000.
// Positive = attacker gaining ticking score, negative = defender gaining.
inline int compute_ticking_score(uintptr_t war_ptr) {
    int64_t atk_tick = 0, def_tick = 0;
    uint8_t atk_enabled = safe_read<uint8_t>(war_ptr + 0x48);
    uint8_t def_enabled = safe_read<uint8_t>(war_ptr + 0x78);
    if (atk_enabled) atk_tick = safe_read<int64_t>(war_ptr + 0x40);
    if (def_enabled) def_tick = safe_read<int64_t>(war_ptr + 0x70);
    return static_cast<int>((atk_tick - def_tick) / 100000);
}

// =============================================================================
// Hook Management
// =============================================================================

// Log file for diagnostics (shared across all bridge modules)
extern FILE* g_logfile;

// Process-lifetime trace gate (CK3_BRIDGE_TRACE=1). Default OFF. Gates the
// high-frequency per-query BT_TRACE diagnostics from the ck-xm08 investigation.
extern bool g_trace;

// Install an inline hook at the given address.
// Returns the trampoline address (original function entry for calling through).
void* install_hook(uintptr_t target_addr, void* hook_fn);

// Remove a previously installed hook
bool remove_hook(uintptr_t target_addr);

// Allocate a trampoline slot from the RWX pool near g_base_addr.
// Returns a writable+executable pointer, or nullptr on failure.
// Used by CALL-site hooking to place a redirect stub within ±2GB of .text.
uint8_t* alloc_trampoline_slot();

// =============================================================================
// Game State Reading
// =============================================================================

struct CharacterInfo {
    uint32_t id;
    bool alive;
    bool ai_controlled;
    bool has_realm;       // has landed title
    void* raw_ptr;        // raw character pointer for further reads
    void* ai_controller;  // null if no AI controller
};

// Enumerate all AI-controlled living characters
// Callback receives character info; return false to stop iteration
void enumerate_ai_characters(std::function<bool(const CharacterInfo&)> callback);

// Cache the AI manager pointer (called from tick hook)
void set_ai_manager(void* mgr);
void* get_ai_manager();

// Generate RNG matching native AI pattern
uint32_t generate_rng();

// =============================================================================
// SIGALRM Watchdog Timer
// =============================================================================

// Initialize the per-thread watchdog timer. Must be called from the game thread
// (on_tick context). Uses SIGEV_THREAD_ID to guarantee SIGALRM delivery to
// the calling thread, preventing the wrong-thread-delivery hang bug.
void watchdog_init();

// Arm the watchdog timer for N seconds. SIGALRM will be delivered to the game
// thread (set during watchdog_init). Falls back to alarm() if init failed.
void watchdog_arm(unsigned secs);

// Disarm the watchdog timer. No-op if already disarmed.
void watchdog_disarm();

// =============================================================================
// IPC Server
// =============================================================================

// Start the Unix domain socket server on a background thread.
// ck-qn03: nullptr (the default) means "this instance's socket" —
// bridge::instance_socket_path(), which is the legacy /tmp/ck3-agent.sock on
// the default instance. Never hardcode the literal at a call site again.
bool start_ipc_server(const char* socket_path = nullptr);

// Stop the IPC server
void stop_ipc_server();

// ck-6kvq.5: a declared save identity can REFUTE a session's recorded world, in
// which case SessionRegistry::reconcile_declared_world erases that session and
// releases its claims.  The IPC layer holds a RAW pointer to every session from
// its ClientSlot, so the erase has to be paired with an eviction of any slot
// still bound to it — otherwise the next line that client sends dereferences a
// freed Session.  This does both, in that order, under g_client_mutex, and is
// the ONLY supported way to run a reconciliation from outside ipc_server.cpp.
//
// Returns the ids of the sessions that were erased (empty is the normal case).
std::vector<std::string> ipc_reconcile_declared_world(const WorldIdentity& declared,
                                                      uint64_t current_epoch);

// Check if an agent is connected
bool agent_connected();

// Snapshot of the single-client IPC slot — populated from per-accept state and
// SO_PEERCRED. `peer_alive` is a live kill(pid, 0) probe (ESRCH → dead).
// Used by the `who_holds_the_lock` query and by the accept-loop's dead-peer
// release path (ck-ngoh1).
struct ClientLockInfo {
    bool connected;                  // true iff >=1 client slot is live
    int  fd;                         // representative (first) live slot's fd, -1 when none
    int  peer_pid;                   // -1 if SO_PEERCRED was unavailable
    int  peer_uid;                   // -1 if SO_PEERCRED was unavailable
    int  peer_gid;                   // -1 if SO_PEERCRED was unavailable
    uint64_t connection_age_seconds; // monotonic seconds since the holder's accept()
    bool peer_alive;                 // true iff kill(peer_pid, 0) succeeded
    int  client_count;               // multi-client: number of live client slots
};
bool get_client_lock_info(ClientLockInfo* out);

// Send observation data to the connected agent
// Returns true if sent successfully
bool send_observation(const char* json_data, size_t len);

//: ck-h4pn: AN OBSERVATION FOR WHOEVER IS LISTENING, never a reply to whoever asked.
//:
//: send_observation routes by a reader-thread thread-local, so a frame emitted from
//: inside the inline-query window unicasts to the CALLING client — correct for a
//: reply and wrong for a record. emit_management_ledger_record ran there and its
//: rows went to the agent instead of the observer the ARM relay drains: zero of
//: ~386 management calls in ep-02, with the frame itself well-formed throughout.
//:
//: Use this for anything whose audience is "whoever is listening", regardless of
//: which thread emits it. It is a separate function rather than a flag on
//: send_observation so the intent is visible at the CALL SITE, which is where the
//: old bug was invisible.
bool broadcast_observation(const char* json_data, size_t len);
// ck-n7n0: route a drained reply to the session that asked for it. Returns
// false when that session has no live slot, so the caller falls back to the
// broadcast rather than dropping the reply.
bool send_to_session_id(const char* session_id, const char* json_data, size_t len);

// Dequeue a message from the background reader's thread-safe queue.
// timeout_ms: 0 = non-blocking (drain mode), >0 = wait up to timeout_ms ms.
// Returns allocated buffer (caller frees) or nullptr if empty/timeout.
char* dequeue_action(int timeout_ms = 0);

// ck-6kvq.9: push a pre-built {"session_id":..,"actions":[..]} envelope onto the
// SAME game-thread queue the reader thread uses, so the pipeline conductor's
// DrainTick step hands its batch to the EXISTING drain
// (bridge_init.cpp on_tick) rather than opening a second execution path.
// Returns false only when the envelope could not be queued at all — a FAILED
// ticket, never a lost one.
bool enqueue_action_envelope(const std::string& envelope);

// Send a JSON message string to the connected agent (NDJSON format).
// Convenience wrapper around send_observation for std::string.
bool send_ipc_message(const std::string& json_msg);

// =============================================================================
// Event Interception
// =============================================================================

// Event data sent to the agent when an event fires for the controlled character
struct EventInfo {
    char event_id[128];        // event type key (e.g. "feast_event.0001")
    uint32_t target_char_id;   // character this event targets
    int option_count;          // number of available options
    char title[256];           // event title (loc key or extracted text)
    char description[512];     // event description (truncated)
    char option_texts[8][256]; // up to 8 option descriptions
};

// Initialize the event interception system (called from bridge_init)
void init_event_handler();

// Shutdown the event handler
void shutdown_event_handler();

// Send an event to the agent over IPC and wait for the agent's option choice.
// Pauses the game while waiting. Returns chosen option index (0 on timeout).
int send_event_and_wait(const EventInfo& event, int timeout_ms = 30000);

// Pause/unpause the game (writes to CJominiIdler singleton)
void game_pause();
void game_unpause();
void game_unpause_delayed(int delay_ms);  // Unpause after delay (via on_tick countdown)
void check_delayed_unpause();             // Called from on_tick

// Check if the game is paused
bool is_paused();

// Auto-respond to pending player events (selects option 0, clears event-pause).
// Called from on_tick in passthrough mode when g_auto_respond_events is true.
// Returns the number of events dismissed.
int auto_respond_pending_events();

// Poll pending events and send non-blocking notifications to the connected agent.
// Called from on_tick when an agent is connected. Returns number of new events sent.
int poll_and_notify_events();

// Select an option for a pending event by instance_id.
// Clears event-pause and unpauses the game. Returns true on success.
bool select_event_option(uint32_t instance_id, int option_index);

// Clear a sent event from the tracking set (called after agent responds).
void clear_sent_event(uint32_t instance_id);

// Clear all sent events (e.g., on agent disconnect).
void clear_all_sent_events();

// Set/get game speed (1-5, via game state root)
void game_set_speed(int speed);
int game_get_speed();

// =============================================================================
// Deferred-drain pause-transparency coordinator (deferred_drain.cpp)
// =============================================================================
//
// A DEFERRED call (CB validity, acceptance, trigger/named-trigger eval, decision
// eval, conquest-targets) is serviced on the GAME thread inside
// drain_and_execute_intents(), which only runs while the sim ticks. Called while
// the game is PAUSED, the call's bounded poll never sees a drain and either
// returns a graceful timeout or (for the unclassified handlers) sat in the
// g_msg_queue forever → client HANG.
//
// run_deferred_pause_transparent() makes every deferred call work from EITHER
// pause state, transparently for all callers (agent, raw IPC, harness, suite):
// it records the live {paused,speed}, and — only if paused — pulses the game
// unpaused just long enough for on_tick → drain_and_execute_intents() to service
// the request, then restores {paused,speed} EXACTLY on every exit path. Bounded
// throughout (the I15 frozen-thread guard): a never-draining call returns a typed
// status instead of hanging. No-op (just runs `service`) when already unpaused —
// the common agent-loop hot path is untouched.
//
// `service` is the existing submit+poll closure; its return string is passed
// through verbatim on success. On the pulse-failure paths the coordinator
// synthesizes a typed JSON status into `out` (see DeferredDrainStatus).
enum class DeferredDrainStatus {
    Ok,                // pulse (if any) started a tick; service() ran — `out` is service()'s result
    DrainUnavailable,  // pulse-start budget elapsed with NO tick (idler null / load/menu) — sim not live
    BusyDeferred,      // the single-flight lock was held past the lock budget
};

// Run `service` under the record→pulse→drain→restore critical section.
// On return:
//   - Ok               : *out holds whatever `service` wrote (its own typed result,
//                        including its own graceful drain_timeout if it timed out).
//   - DrainUnavailable : *out holds a synthesized {status:"drain_unavailable",...}
//                        and `service` was NOT run (the world isn't ticking).
//   - BusyDeferred     : *out holds a synthesized {status:"busy_deferred",...}
//                        and `service` was NOT run.
// The function NEVER blocks longer than (lock budget + pulse-start budget +
// service's own bounded poll). Crash-proof: pulse/restore engine calls are the
// fault-guarded event_handler.cpp primitives; the lock/refcount are pure C++.
DeferredDrainStatus run_deferred_pause_transparent(
    const std::function<void(std::string*)>& service,
    std::string* out);

// Spin-read g_tick_id against a captured baseline; return true once it advances
// past `baseline_tick` within `budget_ms`, false otherwise. Used by the
// coordinator's pulse-start budget (mirror of the gateway's _await_tick_advance).
bool await_tick_advance(uint64_t baseline_tick, int budget_ms);

// ck-p2sb. HOW MANY TIMES EACH TYPED REFUSAL FIRED, so that its ABSENCE can be
// read as evidence. Before this, `busy_deferred` existed ONLY in the reply frame
// it was written into — and on this socket every queued response is broadcast
// with request_id as the only discriminator (ipc_server.cpp:1507), so a wedged
// drain left no trace in the log or any artefact. "The drain never wedged this
// episode" was a sentence nobody could say.
//
// THREE STATES, as everywhere else: a bridge too old to report these keys says
// NOTHING (the keys are absent); 0 means the bridge looked and neither refusal
// fired; N is N refusals. Reported by get_bridge_state. Instrumentation only —
// reading them has no behavioural effect, and they are never reset.
extern std::atomic<uint64_t> g_busy_deferred_count;
extern std::atomic<uint64_t> g_drain_unavailable_count;

// True for actions whose real work is serviced on the game thread post-tick and
// therefore need the deferred-drain coordinator's pulse when called paused.
// Defined in deferred_drain.cpp.
bool is_deferred_drain_action(const char* action_type);

// =============================================================================
// Offset Validation (offset_validator.cpp)
// =============================================================================

// Maximum number of offset groups (must match offset_validator.cpp)
static constexpr int MAX_OFFSET_GROUPS = 16;

struct OffsetValidationResult {
    int total_groups;
    int groups_passed;
    int groups_failed;
    int total_checks;
    int checks_passed;
    int checks_failed;
    float failure_pct;                    // group failure percentage
    bool degraded;                        // true if >30% groups failed
    bool group_passed[MAX_OFFSET_GROUPS]; // per-group pass/fail
    int group_checks_passed[MAX_OFFSET_GROUPS];
    int group_checks_failed[MAX_OFFSET_GROUPS];
    int group_checks_total[MAX_OFFSET_GROUPS];
};

// Run all offset validation checks. Returns summary with per-group results.
// Should be called after g_base_addr is resolved and memory map is available.
OffsetValidationResult validate_offsets();

// Query group metadata
int offset_validation_group_count();
const char* offset_validation_group_name(int index);

// =============================================================================
// Bridge Lifecycle
// =============================================================================

// Called during DLL load (constructor attribute)
void bridge_init();

// Called during DLL unload (destructor attribute)
void bridge_shutdown();

// Main tick hook — called each game tick instead of the original AI tick
void on_tick(void* ai_manager);

// Initialize action dispatch function pointers (called during bridge_init)
void init_action_dispatch();

// Per-character subsystem mask API
bool set_character_subsystems(uint32_t char_id, const char* params_json);
bool get_character_subsystems(uint32_t char_id, std::string* response_out);

// =============================================================================
// War Score Breakdown Cache (war_score_hook.cpp)
// =============================================================================

struct WarScoreBreakdown {
    int imprisonment_atk;   // ImprisonmentScore(war, 0, 0) — fixed-point /1000
    int imprisonment_def;   // ImprisonmentScore(war, 1, 0)
    int battle_atk;         // BattleScore(war, 0, 0)
    int battle_def;         // BattleScore(war, 1, 0)
    int occupation_atk;     // OccupationScore(war, 0, 0)
    int occupation_def;     // OccupationScore(war, 1, 0)
    int ticking_atk;        // TickingWarScore(war, 0, 0)
    int ticking_def;        // TickingWarScore(war, 1, 0)
    int landless_atk;       // LandlessArmiesLost(war, 0, 0)
    int landless_def;       // LandlessArmiesLost(war, 1, 0)
    int total;              // clamped sum as percentage [-100, +100]
    uint64_t cached_tick;
};

// Why on_war_score_update last declined to refresh a war's breakdown (ck-xwlg).
enum class WarScoreRejectReason : uint8_t {
    None = 0,
    FnUnresolved,
    CbSubobj,
    BattleList,
    AtkParticipants,
    DefParticipants,
    OccupationChain,
    LandlessRealm,
};

// Cache entries older than this many bridge ticks (≈ game days) are fossils:
// the hook has been rejecting every refresh since. See war_score_hook.cpp.
constexpr uint64_t WAR_SCORE_FRESH_TICKS = 100;

// Initialize war score post-hook (called during first-tick deferred init)
void init_war_score_hook();

// ck-sd4d: hooks WAR_END_CORE so an ENDED war (outcome + end date) is
// observable at all — the engine destroys the CWar in the same call.
void init_war_history_hook();

// Initialize war declaration observer (called during first-tick deferred init)
void init_war_declaration_hook();
// Check WAR_DB for new wars (called every tick from on_tick)
void check_war_db_tick();

// Initialize scheme initiation observer hook (called during first-tick deferred init)
void init_scheme_init_hook();

// Initialize AI CB selector observer hook (called during first-tick deferred init)
// Post-hook observes AI_CB_SELECTOR output; gated by thread-local
// g_in_bridge_war_intent (set in intent_dispatch.cpp around war_post_decl).
void init_ai_cb_selector_hook();
void init_war_decl_handler_hook();

// Initialize marriage observation hook (called during first-tick deferred init)
void init_marriage_observation_hook();

// Initialize council task assignment observer hook (called during first-tick deferred init)
void init_council_task_hook();

// Initialize per-ruler council-AI suppression hook (L20 stickiness; first-tick deferred init)
void init_council_suppress_hook();

// Initialize per-character off-goal routine-war veto hook (re_offgoal_veto_FINAL.md;
// first-tick deferred init). Ships INERT behind g_offgoal_veto_enabled (default false).
void init_offgoal_war_veto_hook();

// Per-ruler council-lock release (council_suppress_hook.cpp). Declared here too so
// the on_tick dead-ruler handoff path (bridge_init.cpp) can clear a stale lock when
// a controlled ruler dies — not only on explicit release_character.
void council_lock_remove(uint32_t char_id);

// Query cached breakdown for a single war (thread-safe, read lock)
bool query_war_score_breakdown(uint32_t war_id, WarScoreBreakdown* out);
// Freshness-aware variant: stale entries (see WAR_SCORE_FRESH_TICKS) report as
// a miss; reject_out (optional) receives the last validator that starved the
// refresh, for surfacing in query responses.
bool query_war_score_breakdown_fresh(uint32_t war_id, WarScoreBreakdown* out,
                                     WarScoreRejectReason* reject_out);
const char* war_score_reject_name(WarScoreRejectReason r);

// Query all cached breakdowns (thread-safe, read lock)
bool query_all_war_score_breakdowns(std::unordered_map<uint32_t, WarScoreBreakdown>* out);

// Dispatch a single action command from the agent.
// Returns true on success. If response_out is non-null, fills it with
// JSON response data (used by query handlers to return game state).
bool dispatch_action(const char* action_type, const char* params_json,
                     std::string* response_out = nullptr);

bool dispatch_action_with_response(const char* action_type, const char* params_json,
                                    std::string* response_out);

// D1-C / FIX 3.1: true iff `action_type` has a dispatch case. Generated from
// the SAME table (action_registry.inc) as the dispatch chain, so registration
// and dispatch cannot drift. Used by the IPC reader to fast-fail unknown
// action names inline instead of letting them queue to g_msg_queue (where a
// paused caller would hang forever — the D1 query_titles class). NOT a pause
// classifier; see snapshot.h for those.
bool is_registered_action(const char* action_type);

// rc2 B2: true iff `verb` is a debug verb this build compiled out (always false
// with CK3_BRIDGE_DEBUG_VERBS). Such a verb is not registered; the IPC reader
// answers it with error=unavailable_in_release rather than unknown_action.
bool debug_verb_compiled_out(const char* verb);

// Bridge-management verbs: tempo, AI toggles, observer/automation switches and
// the diagnostic reads. Not game mutations even though action_is_mutation()
// (which is simply !readonly) says otherwise — which is why the pipeline's
// polarity authority needs it (pipeline.cpp classify_polarity step 3/4).
bool is_bridge_management_verb(const char* action_type);

// Look up an interaction object by type name
void* lookup_interaction(const char* interaction_type);

// Look up a character by ID (returns nullptr if not found)
void* lookup_character(uint32_t char_id);

// Hash a string using the game's CRC-like hash function
uint32_t game_string_hash(const char* data, uint32_t len);

// =============================================================================
// AI Interaction Evaluation Hook (Intent Queue)
// =============================================================================

// Discover TLS offsets needed by execute_direct_dispatch.
// Called once during deferred initialization from the first on_tick.
void init_tls_offsets();

// Drain the intent queue and execute all pending intents on the game thread.
// Called from on_tick() in the serial section BEFORE parallel AI workers launch.
// This is the safe replacement for the old hooked_ai_interaction_decision approach.
void drain_and_execute_intents();

// Queue an intent for the AI evaluation hook to process during the natural tick.
// intent_type defaults to INTERACTION. For WAR_DECLARE, the intent is dispatched
// via execute_war_intent (AI war pipeline) instead of the interaction dispatch path;
// interaction_type may be any label string (used for logging only, hash not required).
bool queue_intent(uint32_t actor_id, uint32_t target_id,
                  const char* interaction_type, uint32_t max_age_ticks = 100,
                  const char* request_id = nullptr, bool force_bypass = false,
                  uint32_t claimant_id = 0, int cb_index = -1,
                  uint32_t secondary_actor_id = 0, uint32_t secondary_recipient_id = 0,
                  int title_id = -1,
                  bool debug_all_paths = false,
                  IntentType intent_type = IntentType::INTERACTION);

// Get count of pending intents
size_t pending_intent_count();

// Clear all pending intents
void clear_pending_intents();

// =============================================================================
// Intent Queue (new architecture — replaces deferred dispatch)
// =============================================================================

// Global intent queue — intents enqueued from IPC, consumed during AI tick
extern IntentQueue g_intent_queue;

// Execute intents from within the AI tick context (safe to call engine functions)
bool execute_war_intent(Intent* intent);
bool execute_decision_intent(Intent* intent);

// =============================================================================
// Session Lifecycle
// =============================================================================

enum class SessionState : int {
    DORMANT   = 0,  // Game menu, no save loaded
    ACTIVE    = 1,  // Save loaded, tick running
    RELOADING = 2,  // Transitioning between saves
};

// Get the current session state (thread-safe)
SessionState get_session_state();

// String representation for JSON output
const char* session_state_string();

// =============================================================================
// State
// =============================================================================

extern std::atomic<bool> g_initialized;
extern std::atomic<bool> g_agent_mode;   // true = agent controls AI, false = passthrough
extern std::atomic<bool> g_auto_respond_events;  // true = auto-dismiss pending events in passthrough mode
extern std::atomic<uint64_t> g_tick_id;

// #123: live "tick":N,"game_date":"YYYY.MM.DD" JSON fragment (no surrounding
// commas) for stamping the MCP envelope clock into every response. Defined in
// ipc_server.cpp; reused by the game-thread drain path in bridge_init.cpp.
std::string build_envelope_clock_fragment();

// Strict mode (CK3_BRIDGE_STRICT=1 at process start). Default = false: SIGSEGV
// inside a sigsetjmp-protected region is swallowed after logging, preserving
// resilience for normal play. When true: the catch path calls
// bridge_strict_check_abort(site), which logs and abort()s — turning every
// previously-silent pointer-validation miss into a crash signal during test
// runs. See ck-nr51.4 / ADR-0001.
extern std::atomic<bool> g_strict_mode;

// Called from each sigsetjmp catch path. No-op unless g_strict_mode is set, in
// which case it flushes the bridge log and abort()s. Safe to call from any
// thread (not from a signal handler — the signal handler must siglongjmp back
// to the catch path first so the abort runs on a clean stack).
void bridge_strict_check_abort(const char* site);

// Controlled character selection (Phase A)
// g_controlled_primary: 0 = use engine's player character, nonzero = specific char ID
extern std::atomic<uint32_t> g_controlled_primary;
// g_controlled_heir: last successor id resolved by the on_tick dead-ruler handoff
// (primary-title current-holder). 0 = none resolved. Surfaced in get_tick_info as
// controlled_heir_id so the agent/monitor can see who control was handed off to
// (or, in expose-only fallback, who to one-call set_controlled_character on).
extern std::atomic<uint32_t> g_controlled_heir;
// Multi-character roster: all characters currently under agent control
// Protected by g_controlled_mutex — acquire before reading or modifying.
extern std::vector<uint32_t> g_controlled_chars;
extern std::mutex g_controlled_mutex;

// Personality lock: re-apply overridden personality values after engine tick
void apply_personality_locks();
void clear_personality_lock(uint32_t char_id);

// Personality getter hooks: intercept engine reads of personality values
void install_personality_hooks();

} // namespace bridge
