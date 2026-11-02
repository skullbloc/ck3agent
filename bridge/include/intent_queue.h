#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

namespace bridge {

enum class IntentType : uint8_t {
    INTERACTION = 0,
    WAR_DECLARE = 1,
    DECISION = 2,
    // 3 (console execute) and 4 (scripted effect) were deleted in cleanup
    // 2026.11 (D5/D6); the values are not reused.
    // ck-eyfb: direct WAR_CREATE_CORE invocation, bypassing CStartWarEffect
    // and the scripted-effect dispatcher. Mirrors WAR_DECLARE_SPECIAL_HANDLER
    // (FUN_065db970): resolves CB by name, builds minimal CB-params struct
    // with a single-title array, and calls WAR_CREATE_CORE. cb_name in
    // interaction_name; title_id, claimant_id, actor_id, target_id reused.
    WAR_CREATE_CORE_DIRECT = 5,
    // faction write (factions.cpp, re_factions.md): console_command carries the
    // action ("create" / "join" / "leave" / "press_demand"); create takes the
    // faction-type key in interaction_name and the liege in target_id (0: the
    // actor's own), the others the faction id in title_id. Drained like
    // WAR_CREATE_CORE_DIRECT (RNG scope + crash-net).
    FACTION = 6,
    // 7 and 8 were RAISE_ARMIES and RAISE_RAIDERS (deleted, cleanup 2026.11).
    // #111 (re_create_title_action.md): LEGITIMATE de-jure create-title. Build a
    // CCreateTitleCommand (ruler@+0x20 = AI ruler full handle, title_id@+0x24 =
    // target de-jure duchy/kingdom title id), run its vt[7] CanExecute pre-gate
    // (the engine's de-jure-control + not-extant + gold-cost check), and on PASS
    // dispatch it via GAME_COMMAND_DISPATCH(0xe) onto the player-command queue —
    // the engine applies the gold cost + creates the title natively on its tick.
    // Unlike RAISE_*/decision, the ruler is carried IN the command struct, so NO
    // g_PlayerCharacterID global-swap is needed. Drained on the game thread under
    // watchdog + sigsetjmp + fake-RNG (cost eval may advance RNG). actor_id = the
    // AI ruler creating the title; title_id = the de-jure title to create.
    CREATE_TITLE = 9,
    // #132 (docs/notes/hostile-intrigue-research.md): LEGITIMATE start-a-scheme
    // (murder / fabricate_hook / claim_throne / …). execute_scheme_intent
    // resolves the owner's scheme manager (GET_SCHEME_MANAGER) + the scheme type
    // by key from SCHEME_TYPE_DB, builds the target_scope (target_type|target_ref),
    // and calls CREATE_ACTIVE_SCHEME(mgr, type, owner, target_scope, tpl5, tpl6).
    // The ENGINE'S OWN GATES live INSIDE CreateActiveScheme (type is_shown/is_valid
    // predicates, ValidateSchemeTarget, owner+target alive, HasSchemeOnTarget
    // cooldown) — on any failure it returns the SCHEME_NULL sentinel, so calling it
    // forward is self-gating (no rule bypass). The 5th/6th args (a subtype-scope
    // object + a scope object, both dereferenced inside InitializeScheme) are NOT
    // nullable; they are captured at runtime by the CREATE_ACTIVE_SCHEME observer
    // hook (scheme_init_hook.cpp) from real engine-initiated schemes and reused
    // per scheme-type. actor_id = owner (must be AI); target_id = victim character;
    // interaction_name = scheme key. Game-thread-only (drained here) under
    // watchdog + sigsetjmp + fake-RNG, mirroring execute_faction_intent.
    SCHEME = 10,
    // 11 (break alliance) was deleted in cleanup 2026.11 (D6); not reused.
    // RESERVED: MOVE_ARMY (=12) removed per design (agent does not drive army micro).
    // Value left as a gap so existing intent serialization stays stable; do not reuse.
    // 13 was WAR_FORCE_OFFENSIVE_STANCE (deleted, cleanup 2026.11: the engine's own
    // coordinator already prosecutes a steered war; re_steered_war_offensive_root.md §9).
};

// The queue seq of the intent drain_and_execute_intents is executing on this
// thread, or 0 when idle. Set after claim(), cleared after finish(); read by the
// drain's crash recovery (recover_drain_crash) to attribute a crash to THAT
// intent. L13 FF4 (witness m10): by seq, not request_id, which a caller may
// repeat, so a duplicate queued ahead of the one in flight is never rejected
// in its place. Thread-local: only the game thread drains.
extern thread_local uint64_t g_inflight_intent_seq;

struct ClientContext;

struct Intent {
    IntentType type;
    uint32_t actor_id;
    uint32_t target_id;          // 0 = let AI choose
    uint32_t interaction_hash;   // hash from list_interactions
    std::string interaction_name;
    int title_id;                // -1 = none
    uint32_t claimant_id;        // 0 = self (actor is claimant), for claim wars
    int cb_index;                // -1 = auto-select first valid CB
    uint32_t secondary_actor_id;     // marriage: bride/groom from actor's court
    uint32_t secondary_recipient_id; // marriage: bride/groom from target's court
    // The war an interaction is about, written as the interaction's target scope
    // (ctx+0x2F0, scope type 0x10 = war): call_ally_interaction and
    // join_war_interaction take the war there (re/findings/re_war.md). War 0 is a
    // real war, so presence is its own flag.
    bool has_target_war = false;
    uint32_t target_war_id = 0;
    uint64_t queued_tick;
    uint32_t ttl_ticks;          // default 10 (dispatch hooks), 100 (queue_intent)
    bool force_bypass;           // enable debug bypass flag for this dispatch
    bool force_accept;           // enable yesmen (force AI acceptance) for this dispatch
    bool consumed;
    bool executed;
    bool rejected;
    char rejection_reason[128];
    std::string request_id;
    std::string session_id;      // M1: owning session for game-thread context
    std::string console_command; // per-type discriminator (faction create/join/leave, scheme start/cancel)
    std::string console_output;  // per-type result text the executor reports back
    // Lane l4: the interaction window's send options (flag names, comma-separated, e.g.
    // "matrilineal,hook") the caller asked for, and the selection the engine held after
    // they were applied (options that start enabled included).
    std::string send_options;
    std::string send_options_applied;
    // L13 FF3 (witness X3): the queue's own bookkeeping, written under its lock.
    // seq names the intent inside the queue (request_id may be empty or
    // repeated); in_flight is true from claim() to finish(): the drain is
    // executing its own copy and nothing in the queued record is written.
    uint64_t seq = 0;
    bool in_flight = false;
};

// L14 (surface-eval finding 3): a READ the drain runs on the game thread — the
// interaction check (query_interaction_check) and the scheme check
// (scheme_action action=check, the schc_ ids discover(scheme_targets) sends).
// It sends nothing. Its verdict reaches its own asker through the lookups
// (find_completed_by_request_id, lookup_dispatch) and never the completed ring
// that query_intents / query_intent_results / inspect(pending) list: a read
// listed there read as a finished write ("befriend: success" for a scheme never
// started), and took a slot from a real write's poll_token. The one definition
// both check handlers' intents are classified by: their console_command starts
// "check" (bare, "check:<ids>" or "check:<max>:<ids>").
inline bool intent_is_read(const Intent& i) {
    return (i.type == IntentType::INTERACTION || i.type == IntentType::SCHEME) &&
           i.console_command.compare(0, 5, "check") == 0;
}

// L13 FF2 (witness R3): the status query_dispatch_result answers, decided in
// one place. The order is the point: the completed ring first, then an intent
// the drain has consumed but not yet stored — RUNNING, never not_found (the
// server treats not_found as a refusal, so the gap between consume and store
// read as "the write was refused" for a write that ran) — then a pending one.
// `completed` is the completed-ring record when found_completed.
enum class DispatchLookup { Completed, Running, Pending, NotFound };
inline DispatchLookup dispatch_lookup(bool found_completed, bool consumed_unstored,
                                      bool pending) {
    if (found_completed) return DispatchLookup::Completed;
    if (consumed_unstored) return DispatchLookup::Running;
    if (pending) return DispatchLookup::Pending;
    return DispatchLookup::NotFound;
}

class IntentQueue {
    std::mutex mutex_;
    std::vector<Intent> intents_;
    std::atomic<size_t> pending_count_atomic_{0};  // lock-free check for hot path
public:
    // Lock-free pending count — safe to call from parallel AI eval threads
    // without taking the mutex. Updated by enqueue/clear/tick_maintenance.
    size_t pending_count_fast() const { return pending_count_atomic_.load(std::memory_order_relaxed); }

    // Enqueue from IPC thread
    void enqueue(Intent intent);

    // L13 FF3 (witness X3): the drain's two calls. No pointer into the queue
    // ever leaves it, so an enqueue that grows the vector, a cancel or a clear
    // cannot pull an intent out from under the drain, and no reader copies a
    // record the drain is writing.
    //   claim(seq, out): under the lock, an unconsumed intent with that seq is
    //     marked consumed + in_flight and copied into *out (the drain executes
    //     on that copy). False if it is gone or already consumed (retired,
    //     cancelled, expired, or claimed).
    //   finish(done): under the lock, the queued record becomes the drain's
    //     copy, consumed, in_flight cleared. A copy with neither executed nor
    //     rejected (an executor that returned without a verdict) is recorded as
    //     rejected "executor_returned_no_verdict", never as a silent nothing
    //     (witness r4 m9). If the record was cleared meanwhile, the verdict goes
    //     straight to the completed ring.
    bool claim(uint64_t seq, Intent* out);
    void finish(const Intent& done);

    // Return a snapshot of all unconsumed intents (for query_intents IPC)
    std::vector<Intent> snapshot();

    // The count of unconsumed WRITES: pending_writes().size(). L14 FB2 (witness
    // N-10): one meaning wherever a pending count is reported (query_intents'
    // pending_count, send_interaction's pending_count, status' pending_intents,
    // clear_intents); it had counted queued checks too, which no list shows.
    // The drain's own gate is pending_count_fast (every unconsumed intent).
    size_t pending_count();

    // L14 FB m-6: the unconsumed WRITES (snapshot without intent_is_read). A
    // queued check is a game-thread read, delivered to its asker and never
    // listed: query_intents' pending[] and pending_count are this.
    std::vector<Intent> pending_writes();

    // Expire old intents; move consumed + expired ones into the completed ring
    // (under the queue's lock) and also return them. Called by the drain.
    void tick_maintenance(uint64_t current_tick,
                          std::vector<Intent>& completed_out);

    // Cancel a specific intent by request_id (for cancel_intent IPC)
    bool cancel_by_request_id(const std::string& request_id);

    // lane-scheme 2026-10-18: retire a still-pending intent IN PLACE (consumed +
    // rejected with `reason`) instead of erasing it, so its verdict reaches the
    // completed ring. Only an UNCLAIMED intent is retired: one the drain has
    // claimed (L13 FF3) is consumed, so this returns false and the caller reads
    // the drain's verdict instead (never "cancelled" for a send that ran).
    // tick_maintenance moves it to completed.
    // Returns true if a pending intent with that id was found and marked.
    bool retire_pending_by_request_id(const std::string& request_id,
                                      const char* reason);

    void clear();

    // Store a completed record directly (the drain's Mechanism-B write verdicts;
    // consumed intents reach the ring through tick_maintenance). A read
    // (intent_is_read) goes to the reads ring instead.
    void store_completed(const Intent& intent);

    // Return the last N completed WRITES (the completed ring; reads never enter it)
    std::vector<Intent> completed_snapshot(size_t max_count = 10);

    // ck-jsis0: O(n) lookup of a completed intent by request_id. Returns true
    // and copies the intent into *out if found; false otherwise. Used by
    // query_dispatch_result to disambiguate "completed-and-evicted" from
    // "still pending" without forcing callers to walk the whole snapshot.
    bool find_completed_by_request_id(const std::string& request_id, Intent* out);


    // L13 FF2 (witness R3): where an intent stands, under ONE lock over both the
    // live queue and the completed ring (tick_maintenance moves consumed intents
    // into the ring under the same lock), so a reader can never fall between
    // them. An intent the drain has claimed and not finished is Running (nothing
    // is copied); otherwise *out receives the pending or completed record, whose
    // every field is written under this lock (L13 FF3).
    DispatchLookup lookup_dispatch(const std::string& request_id, Intent* out);

    // The drain's crash recovery: the in-flight intent with this seq (and only
    // an in-flight one) becomes a structured rejection; the drain's copy and
    // its finish() were jumped past. True if it was found.
    bool mark_rejected_in_flight(uint64_t seq, const char* reason);
    // ck-jsis0: bumped from 16 → 128. The destructive harness fires ~15 intents
    // back-to-back before each per-intent verdict check; a 16-slot buffer let
    // late intents push earlier ones out of view before the harness queried,
    // producing ambiguous "intent not in query_intent_results" verdicts. 128
    // covers a full --deep destructive suite without forcing harness writers
    // to interleave queries between every send_interaction.
    // Public so a test can tell an eviction from a lookup gap.
    static constexpr size_t MAX_COMPLETED = 128;
    // L14: the verdicts of the last MAX_READS game-thread reads (intent_is_read),
    // kept only so the asker's poll finds its own verdict; never listed.
    static constexpr size_t MAX_READS = 32;
private:
    std::vector<Intent> completed_;   // ring buffer of last N completed writes
    std::vector<Intent> reads_;       // ring buffer of last N completed reads
    uint64_t next_seq_ = 1;           // under mutex_
    void store_completed_locked(const Intent& intent);
};

// The game thread drains g_intent_queue in drain_game_thread_work
// (drain_and_execute_intents), AFTER it has dispatched the IPC actions queued for
// it. A handler served on the game thread (is_game_thread_action: create_title,
// call_war_create_core) that enqueues an intent and then polls for its
// completion waits for a drain that cannot run until it returns: it stalls the
// game for its whole poll window and answers after the client's own 8 s deadline
// (live867 l5fx3-live-1, the deleted raise_armies: reason_code no_ack). These two
// calls let such a handler see that and answer at once; the intent then drains
// later in the same drain and the caller confirms by readback. L13: the game
// thread is the session thread when the session-pass hook is live
// (bridge_init.cpp claim_drain_thread), else the AI-tick worker running on_tick.
// The drain's crash recovery, after the outer siglongjmp out of
// drain_and_execute_intents (bridge_init.cpp). The jump skipped the
// per-intent ClientContextGuard's destructor, so g_current_client_context
// still points at that iteration's dead stack object: it is put back to
// `saved_ctx`, the value it had when the drain was entered (witness m11). The
// in-flight intent (g_inflight_intent_seq) is rejected with `reason`. Returns
// its seq, or 0 when nothing was in flight. Engine-free: a host test drives it.
uint64_t recover_drain_crash(IntentQueue& q, ClientContext* saved_ctx, const char* reason);

void note_game_thread();      // claim_drain_thread: this thread runs the drain
bool on_game_thread();        // true only on the thread that runs the drain

} // namespace bridge
