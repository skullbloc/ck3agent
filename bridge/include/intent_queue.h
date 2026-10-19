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
    CONSOLE_EXECUTE = 3,
    // ck-7ejs: invoke a user-defined `scripted_effect` template by name on a
    // character scope. effect_name in interaction_name; optional pre-rendered
    // PDXScript args blob in console_command (currently unused — args parsing
    // would require the script tokenizer; reserved for future expansion).
    SCRIPTED_EFFECT = 4,
    // ck-eyfb: direct WAR_CREATE_CORE invocation, bypassing CStartWarEffect
    // and the scripted-effect dispatcher. Mirrors WAR_DECLARE_SPECIAL_HANDLER
    // (FUN_065db970): resolves CB by name, builds minimal CB-params struct
    // with a single-title array, and calls WAR_CREATE_CORE. cb_name in
    // interaction_name; title_id, claimant_id, actor_id, target_id reused.
    WAR_CREATE_CORE_DIRECT = 5,
    // faction write (re_faction_join_create.md): create-or-join a political
    // faction against a liege via GET_OR_CREATE_AND_JOIN. faction-type key in
    // interaction_name; target_id = liege; faction_action ("create"/"join"/
    // "leave") stuffed in console_command; optional title_id/claimant_id for
    // claimant factions. Drained like WAR_CREATE_CORE_DIRECT (RNG scope +
    // crash-net).
    FACTION = 6,
    // A8.2 (re_raise_armies.md): raise a CHOSEN AI's levies. RaiseAllArmiesCore
    // (ARMY_RAISE_ALL) is hard-wired to the g_PlayerCharacterID global with no
    // character arg, so the executor temporarily swaps that global to the AI's
    // full 32-bit handle (char+0x18), calls raise_all(nullptr,0), and restores
    // the global on EVERY path. Game-thread-only (drained here), paused, under
    // watchdog+sigsetjmp+fake-RNG. actor_id = the AI whose levies to raise.
    // NEEDS-LIVE-HARNESS-VALIDATION — the global swap + indirect raise cannot be
    // exercised on a core dump; gated behind this intent/drain path.
    RAISE_ARMIES = 7,
    // #96 (re_raid_system.md): Norse/tribal RAIDING. Toggle the chosen AI's
    // raised armies into RAIDING mode. The dispatch path
    // (ARMY_TOGGLE_RAIDING_DISPATCH) is hard-wired to g_PlayerCharacterID (it
    // resolves the player's armies + builds a validator-gated CToggleRaidingCommand
    // dispatched onto the engine command queue), so the executor uses the SAME
    // guarded global-swap primitive as RAISE_ARMIES: swap the player global to the
    // AI's full 32-bit handle (char+0x18), call dispatch(intent=0), restore on
    // EVERY path. Game-thread-only (drained here), under watchdog+sigsetjmp+
    // fake-RNG. actor_id = the AI whose armies to set raiding. The engine flips
    // army+0x1b4 natively on its own command-apply tick (we never write it).
    RAISE_RAIDERS = 8,
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
    // break-alliance (re_break_alliance_FINAL.md, Path B). The literal
    // CBreakAllowAllianceEffect break via the gated break_alliance_cmd(actor*,
    // ally*, force=0): resolves BOTH characters to live CCharacter* (the same DB
    // walk FUN_05f72a60 does == lookup_character) and calls the engine's own
    // alive+relation-gated command, which runs relation_dissolve (clears the
    // CActiveRelation reasons vector, sets the state byte, fires on-break
    // notifications). Game-thread-only (drained here) under fake-RNG + watchdog +
    // sigsetjmp — the dissolve/notification path can draw engine RNG. actor_id =
    // the ruler breaking the alliance; target_id = the ally to break with.
    // DEFAULT-OFF behind g_enable_break_alliance; not agent-reachable until the
    // disposable-save smoke (FINAL §5) passes.
    BREAK_ALLIANCE = 11,
    // RESERVED: MOVE_ARMY (=12) removed per design (agent does not drive army micro).
    // Value left as a gap so existing intent serialization stays stable; do not reuse.
    // Fix B1a (re_caiwarcoordinator_FINAL.md / re_steered_war_offensive_root.md):
    // make a FORCE-DECLARED steered war get prosecuted OFFENSIVELY (so the AI marches
    // to SIEGE the war goal instead of sitting attacker_DEFENSIVE → occupation flat 0).
    // LEGITIMATE engine path — NOT a memory poke: walk the attacker realm's AI-manager
    // coordinator record array (ai_root+0x48 / count +0x54, the exact FUN_04dedd40
    // walk), find the record whose rec+0x18 war-id matches our war (FUN_04e60920
    // invariant war+8==rec+0x18), zero its re-eval cooldown (rec+0x94=0) and invoke the
    // engine's OWN per-coordinator stance tick COORD_STANCE_TICK(rec, &seed)
    // (FUN_04e66220) once. That tick runs the engine's real selector FUN_04e66550 →
    // real strength-condition FUN_04e843a0 and writes the engine's own chosen stance
    // into rec+0x70 — so if the attacker genuinely has the upper hand it lands
    // attacker_offensive legitimately; if not, defensive is the CORRECT engine verdict
    // (a useful diagnostic, never a forced fabrication). Game-thread-only (drained
    // here) under fake-RNG scope + watchdog + sigsetjmp (the tick rebuilds the war plan
    // / unit-stack assignments and draws engine RNG). DEFAULT-OFF behind
    // g_force_offensive_stance. actor_id = the AI attacker; title_id reused = war_id.
    // NEEDS-LIVE-VALIDATION (attempt-25): the engine tick has never been driven on a
    // live coordinator — gated entirely behind the flag + this intent/drain path.
    WAR_FORCE_OFFENSIVE_STANCE = 13,
};

// 2026-06-09: request_id of the intent currently being executed by
// drain_and_execute_intents on the game thread, or empty when idle. Set/cleared
// around each executor call; read by the outer on_tick recovery handler to
// attribute a crash to the in-flight intent. Thread-local: only the game thread
// drains.
extern thread_local std::string g_inflight_intent_rid;

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
    uint64_t queued_tick;
    uint32_t ttl_ticks;          // default 10 (dispatch hooks), 100 (queue_intent)
    bool force_bypass;           // enable debug bypass flag for this dispatch
    bool force_accept;           // enable yesmen (force AI acceptance) for this dispatch
    bool debug_all_paths;        // try every dispatch path independently, collect logs
    bool resolve_only;           // SCRIPTED_EFFECT: resolve template by name only, do NOT
                                 // call Execute (the scope_ctx frame is unpinned — Execute
                                 // faults). Default true; false is observer-save-only RE.
    bool consumed;
    bool executed;
    bool rejected;
    char rejection_reason[128];
    std::string request_id;
    std::string session_id;      // M1: owning session for game-thread context
    std::string debug_log;       // JSON array of per-path results (populated when debug_all_paths)
    std::string console_command; // CONSOLE_EXECUTE: full command string
    std::string console_output;  // CONSOLE_EXECUTE: result output from handler
};

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

    // Find unconsumed intent for character (called from game thread dispatch hooks)
    Intent* find_for_character(uint32_t char_id, IntentType type);

    // Find unconsumed intent matching character + interaction hash (for AI eval hook)
    Intent* find_for_character_interaction(uint32_t char_id, uint32_t interaction_hash);

    // Return a snapshot of all unconsumed intents (for query_intents IPC)
    std::vector<Intent> snapshot();

    // Return count of unconsumed intents
    size_t pending_count();

    // Expire old intents, return completed ones (called from on_tick)
    void tick_maintenance(uint64_t current_tick,
                          std::vector<Intent>& completed_out);

    // Cancel a specific intent by request_id (for cancel_intent IPC)
    bool cancel_by_request_id(const std::string& request_id);

    // lane-scheme 2026-10-18: retire a still-pending intent IN PLACE (consumed +
    // rejected with `reason`) instead of erasing it. The game-thread drain holds a
    // raw Intent* into intents_ while it executes (find_for_character), so an
    // erase from the IPC thread could pull the element out from under it; a mark
    // under the lock cannot. A drain that has not reached it yet skips it (it is
    // consumed); one already executing overwrites the verdict, so the completed
    // record still tells the truth. tick_maintenance moves it to completed.
    // Returns true if a pending intent with that id was found and marked.
    bool retire_pending_by_request_id(const std::string& request_id,
                                      const char* reason);

    void clear();

    // Store a completed intent result for diagnostic queries.
    // Called from tick_maintenance when an intent is consumed.
    void store_completed(const Intent& intent);

    // Return the last N completed intent results (for diagnostics)
    std::vector<Intent> completed_snapshot(size_t max_count = 10);

    // ck-jsis0: O(n) lookup of a completed intent by request_id. Returns true
    // and copies the intent into *out if found; false otherwise. Used by
    // query_dispatch_result to disambiguate "completed-and-evicted" from
    // "still pending" without forcing callers to walk the whole snapshot.
    bool find_completed_by_request_id(const std::string& request_id, Intent* out);

    // ck-jsis0: O(n) lookup of a pending (unconsumed) intent by request_id.
    // Returns true and copies the intent into *out if found.
    bool find_pending_by_request_id(const std::string& request_id, Intent* out);

    // 2026-06-09: mark a still-pending intent rejected+consumed by request_id.
    // Used by the outer on_tick recovery handler so a game-thread crash during
    // drain converts the in-flight intent into a structured rejection instead
    // of leaving it unconsumed (which the IPC poll sees as a 5s timeout).
    bool mark_rejected_by_request_id(const std::string& request_id, const char* reason);
private:
    std::vector<Intent> completed_;   // ring buffer of last N completed intents
    // ck-jsis0: bumped from 16 → 128. The destructive harness fires ~15 intents
    // back-to-back before each per-intent verdict check; a 16-slot buffer let
    // late intents push earlier ones out of view before the harness queried,
    // producing ambiguous "intent not in query_intent_results" verdicts. 128
    // covers a full --deep destructive suite without forcing harness writers
    // to interleave queries between every send_interaction.
    static constexpr size_t MAX_COMPLETED = 128;
};

} // namespace bridge
