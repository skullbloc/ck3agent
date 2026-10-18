// flush_scheduler.h — R1a: the buffered, polarity-driven flush loop (ck-6kvq.8).
//
// SHAPE (design of record: docs/design/event-pipeline-conductor.md §2.1-§2.3,
// §2.8; overseer ruling 2026-09-10, decision G2). This is NOT a window
// scheduler. There is no `window_open` and no `end_turn`. Calls are accepted
// and BUFFERED; a flush runs on a short wall-clock cadence (or early); the
// clock is paused ONLY if the batch contains pause-needing work, and only for
// as long as that work takes. The world's resting state stays RUNNING.
//
// PURITY. This translation unit has NO engine dependencies: it does not include
// bridge.h, does not touch game memory, does not know what an `on_tick` is and
// cannot pause anything by itself. It takes buffered call descriptors, session
// facts and a clock READING, and emits a PLAN. Applying that plan (flipping the
// clock, serving the reads, handing the tick batch to drain_and_execute_intents)
// is the wiring bead, ck-6kvq.9. That split is what makes this host-testable
// with no game and no CK3 binary.
//
// POLARITY IS AN INPUT. The bridge already computes an action's required pause
// polarity in three places (is_bridge_internal_query / is_game_state_query /
// is_deferred_drain_action + tier-3 mutations). Linking those classifiers here
// would drag the engine in. The submitter supplies the polarity; UNKNOWN
// resolves to PAUSED — the same safe-read default the MCP gateway already uses
// (mcp_server.py:166-177) — and the flush plan reports how many calls took that
// default so a newly-registered, unclassified action is loud, never silent.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "registered_speed.h"   // ck-on4u: the resting speed is DECLARED

namespace bridge {

// ============================================================================
// Polarity — the required clock state of one call.
// ============================================================================
enum class Polarity {
    ANY,        // is_bridge_internal_query: tempo, liveness, identity, rosters.
    PAUSED,     // is_game_state_query: live object-graph reads (ipc_server.cpp:583-588).
    TICK_READ,  // is_deferred_drain_action: needs a live tick for an ENGINE VERDICT.
    TICK_WRITE, // every tier-3 mutation: needs a live tick to take effect.
    UNKNOWN,    // not classified by the caller; resolves to PAUSED.

    // Compatibility alias. The first cut of this header (ck-6kvq.8) had ONE
    // tick class; §2.2 splits it because ordering a deferred-drain READ behind
    // the WRITES of the same flush answers it from a world that has already
    // moved past another agent's write. `TICK` keeps meaning "the write half",
    // which is what every existing use of it meant.
    TICK = TICK_WRITE,
};

// True for both halves of the tick class.
bool is_tick_polarity(Polarity p);

const char* polarity_name(Polarity p);

// UNKNOWN -> PAUSED. Matches the gateway's unclassified default exactly.
Polarity resolve_polarity(Polarity p);

// ============================================================================
// Submission
// ============================================================================
struct CallRequest {
    std::string session_id;
    std::string action;
    Polarity    polarity = Polarity::UNKNOWN;
};

// Which capacity bound refused a submission (observability only; the wire
// `reason` string is "buffer_full" either way, per §2.8).
enum class BackPressureLimit { None, Global, Session };

struct SubmitResult {
    bool               accepted       = false;
    uint64_t           ticket         = 0;              // 0 when refused
    Polarity           polarity       = Polarity::PAUSED;  // resolved
    std::string        reason;                          // "buffer_full" when refused
    uint64_t           retry_after_ms = 0;              // only when refused
    uint64_t           flush_eta_ms   = 0;              // only when accepted
    BackPressureLimit  limit          = BackPressureLimit::None;
};

// ============================================================================
// A buffered call
// ============================================================================
struct BufferedCall {
    uint64_t    ticket = 0;
    uint64_t    seq    = 0;   // global arrival order
    std::string session_id;
    std::string action;
    Polarity    polarity = Polarity::PAUSED;  // already resolved
    bool        polarity_was_unknown = false;
    uint64_t    enqueued_ms = 0;
};

// ============================================================================
// Clock
// ============================================================================
struct ClockReading {
    bool paused = false;
    int  speed  = 3;
};

// ============================================================================
// Flush triggers — §2.3 "Early flush"
// ============================================================================
enum class FlushTrigger {
    None,                  // not due
    Cadence,               // FLUSH_PERIOD_MS elapsed (also the empty-buffer heartbeat)
    AllAny,                // (a) every buffered call is ANY-polarity: serve now, no cycle
    AllSessionsSubmitted,  // (b) every registered session has spoken since the last flush
    ReadLaneEarly,         // (D', ck-h6nx) the EPISODE'S DECLARED OBSERVER spoke:
                           // serve the READ lane now; buffered WRITES stay for
                           // the cadence flush
    SizeBound,             // (c) the buffer reached its early-flush size

    // ck-tqx2: NOT A TRIGGER. The counter array's extent is derived from this,
    // so a trigger added ANYWHERE above is counted automatically. Deriving from
    // SizeBound instead — which is what I did first — only works while SizeBound
    // happens to be last, and "keep SizeBound last" in a comment is a warning,
    // not a control: the next person adds their trigger after it and the count
    // silently drops, which is the exact defect this replaced.
    _Count,
};

const char* flush_trigger_name(FlushTrigger t);

// ============================================================================
// The plan
// ============================================================================
enum class StepKind {
    ServeAny,     // serve ANY calls inline; the clock is NOT touched
    Pause,        // stop the clock (at most once per flush)
    ServePaused,  // serve ALL selected PAUSED calls inside that one span
    Unpause,      // resume the clock
    DrainTick,    // hand the selected TICK batch to the existing on_tick drain
    SetSpeed,     // restore the resting speed
};

const char* step_kind_name(StepKind k);

struct PlanStep {
    StepKind                  kind;
    std::vector<BufferedCall> calls;     // ServeAny / ServePaused / DrainTick
    int                       speed = 0; // SetSpeed
};

struct FlushPlan {
    uint64_t     flush_id = 0;
    FlushTrigger trigger  = FlushTrigger::None;
    uint64_t     wall_ms  = 0;          // the now_ms this plan was cut at

    ClockReading clock_before;
    ClockReading clock_after;           // state once every step has been applied

    std::vector<PlanStep> steps;        // the ordered script

    // The partition of this flush's SELECTED work (§2.2).
    std::vector<BufferedCall> any_calls;
    std::vector<BufferedCall> paused_calls;
    // tick_calls is READS FIRST, THEN WRITES — the concatenation of the two
    // vectors below, and the exact order the DrainTick step must apply.
    std::vector<BufferedCall> tick_calls;
    std::vector<BufferedCall> tick_read_calls;
    std::vector<BufferedCall> tick_write_calls;

    // Left buffered by a batch bound; carries its ticket into the next flush.
    std::vector<BufferedCall> deferred;

    std::map<std::string, size_t> per_session_counts;  // attribution spine, §2.8
    size_t unclassified = 0;            // calls that took the UNKNOWN->PAUSED default

    // The scheduler owns the clock in pipeline mode (G1), so a paused reading at
    // the head of an all-ANY flush is DRIFT, not our doing. The all-ANY fast path
    // still touches nothing (that invariant is absolute); it flags instead, so the
    // wiring bead can log/page rather than have the fast path silently "fix" it.
    bool clock_drift_observed = false;

    size_t pause_count() const;
    size_t unpause_count() const;
    bool   clock_touched() const;       // true iff any Pause/Unpause/SetSpeed step
    size_t selected_count() const;
};

// ============================================================================
// Config
// ============================================================================
struct FlushConfig {
    uint64_t period_ms                  = 10000; // G8: 10s default cadence
    size_t   buffer_capacity            = 256;   // global back-pressure bound
    size_t   per_session_capacity       = 64;    // one socket cannot fill the buffer
    size_t   early_flush_size           = 32;    // condition (c)
    size_t   max_paused_calls_per_flush = 64;    // bounded batch for the pause span
    size_t   max_tick_calls_per_flush   = 8;     // == MAX_ACTIONS_PER_TICK (bridge_init.cpp:1497)

    // ── THE CLOCK STARTS WHEN THE EPISODE STARTS ───────────────────────────
    // When true, a flush whose `privileged_reader` is EMPTY rests the world
    // PAUSED instead of at `resting_speed`. The episode observer's declaration
    // is what starts the clock.
    //
    // WHY. In pipeline mode the bridge owns the clock from LOAD and the world
    // runs ~1.9 game-days per SECOND. Between world-ACTIVE and episode-open the
    // driver does five provenance legs, a steer probe and a separation witness —
    // and that preflight burned more than the 30-day start_witness threshold, so
    // EVERY arm episode opened at a drifted date. A shakedown world was measured
    // 806 game-days past 1066.9.15 with start_witness correctly reading
    // "diverged". A session cannot simply pause it either: clock verbs return
    // clock_owned_by_pipeline, by G1's own design.
    //
    // BRING-UP AND PREFLIGHT ARE NOT THE EPISODE, so they must not consume game
    // time. Tying the clock's start to the observer's declaration makes observed
    // start equal declared start.
    //
    // IT IS A STATE, NOT A CHECKED CONDITION: every plan re-asserts the paused
    // rest while no observer holds the privilege, so there is no window in which
    // a missed check lets the world run.
    //
    // DEFAULT FALSE, and that is load-bearing: NULL mode is E0's observer-only
    // FREE RUN and does not take clock ownership. Turning this on there would
    // freeze the baseline. Only pipeline mode sets it.
    bool     hold_clock_until_observer  = false;
    // THE RESTING SPEED IS THE PRE-REGISTERED PIN, NOT A NUMBER CHOSEN HERE
    // (ck-on4u). It was `3` — a literal with no relationship to
    // `episode_validity.PINNED_SPEED = 2`, unreachable from production (the
    // default, three reads in the planner, and one assignment in a unit test),
    // and therefore a SECOND HOME for a pre-registered constant. The plan's
    // restore step then re-asserted it over the pin, once per world, and the
    // episode ran its whole horizon at a speed §7.3 does not declare.
    //
    // The default is now the declaration; `FlushScheduler::set_resting_speed`
    // is how an episode's own CK3_EPISODE_GAME_SPEED reaches it at bridge init.
    // DO NOT REPLACE THIS WITH A LITERAL: scripts/registered_speed.py's audit
    // fails on an integer literal here, by design.
    int      resting_speed              = kRegisteredGameSpeed;

    // ── ck-h6nx (D'): THE EPISODE'S OWN DECLARED OBSERVER ───────────────────
    // WHY THIS EXISTS. Early-flush condition (b) fires only when EVERY
    // registered session has spoken, and ipc_server registers every session
    // including agents. An agent speaks about once per LLM turn, so (b) stopped
    // firing between the meter's polls and EVERY READ FELL THROUGH TO THE
    // CADENCE — measured at 59 completed polls in 600 s against E0's 300, with
    // the meter's loop stalling rather than merely slowing.
    //
    // WHY IT IS A NAMED SESSION AND NOT `mode == observe`. A privileged reader
    // changes the flush cadence. Keyed on the MODE, any attached diagnostic —
    // a canary, a second meter lane, a probe — would start driving the cadence
    // of the world it is measuring. Keyed on ONE DECLARED PARTICIPANT of one
    // episode, a diagnostic observer changes the cadence in NEITHER direction.
    // That is the inverse of the defect this same line already had, where a
    // registered SILENT session SUPPRESSED the early flush.
    //
    // WHY THE READ LANE ONLY. Serving buffered WRITES on the observer's trigger
    // would make an agent's action land at a moment chosen by the METER, so
    // agent-action latency would become a function of how expensive a sample
    // is. The instrument does not decide when an agent's action lands: writes
    // stay on the cadence, whoever is attached.
    std::string privileged_reader;
    uint64_t retry_after_ms             = 250;   // back-pressure hint
};

// ============================================================================
// FlushScheduler
// ============================================================================
class FlushScheduler {
public:
    explicit FlushScheduler(const FlushConfig& cfg = FlushConfig());

    // --- the declared resting speed (ck-on4u) --------------------------------
    // Set ONCE at bridge init from the episode policy, BEFORE the conductor
    // thread starts (bridge_init.cpp resolves the policy at .init_array time and
    // calls pipeline_start() several hundred lines later), so this needs no lock
    // and takes none. Returns false and changes NOTHING for a value outside
    // 1..5: a planner that rests at a non-speed would emit a SetSpeed step the
    // sink clamps, and a clamped step is a write nobody declared.
    // The value it ended up at is `config().resting_speed` — there is no second
    // accessor for it, because a second way to ask is a second thing to keep
    // in agreement.
    bool   set_resting_speed(int speed);

    // ck-h6nx (D'): name the episode's declared observer. Set ONCE at its
    // handshake, before it has spoken, so no flush can have been decided by a
    // half-installed privilege. `pipeline_declare_episode_observer` enforces
    // first-wins; this setter is the mechanism, not the policy.
    void   set_privileged_reader(const std::string& session_id);
    // ck-zen6: PIPELINE MODE ONLY. See FlushConfig::hold_clock_until_observer
    // — null mode is E0's free run and must never be gated on a declaration.
    void   set_hold_clock_until_observer(bool on);

    // --- session facts (early-flush condition (b), fairness rotation) --------
    void   register_session(const std::string& session_id);
    void   unregister_session(const std::string& session_id);
    size_t registered_session_count() const;

    // --- ingress ------------------------------------------------------------
    // Never drops: a refusal is returned to the caller as
    // {accepted:false, reason:"buffer_full", retry_after_ms}.
    SubmitResult submit(const CallRequest& req, uint64_t now_ms);

    // Withdraw a still-buffered call (ck-6kvq.9, §2.10 `cancel`: "best-effort,
    // before the flush that would run it"). Returns true iff the call was still
    // in the buffer and has now been removed, so a cancelled call is never
    // planned, never served, and — the part that matters at N=6 — never spends
    // a slot of this flush's bounded batch on work nobody wants any more.
    // False means the flush already claimed it; the caller settles the race by
    // reading the ticket's state, which is the authority.
    bool cancel(uint64_t ticket);

    // --- cadence ------------------------------------------------------------
    FlushTrigger due(uint64_t now_ms) const;

    // Cut the plan and consume the selected calls. Mutates: flush id, buffer,
    // since-last-flush submission set, round-robin cursor, last-flush stamp.
    FlushPlan plan_flush(uint64_t now_ms, const ClockReading& clock, FlushTrigger trigger);

    // Convenience: due() then plan_flush(); returns a FlushTrigger::None plan
    // with no steps when not due.
    FlushPlan tick(uint64_t now_ms, const ClockReading& clock);

    // --- introspection (published on get_tick_info by bead .9) --------------
    size_t   buffer_depth() const;
    uint64_t last_flush_ms() const;

    // ck-43u3: WHICH TRIGGER SERVED EACH FLUSH, counted HERE rather than in the
    // conductor. The trigger is the scheduler's own decision, so the count
    // belongs beside it — and it is what lets the rule be DRIVEN: a probe can
    // tick this scheduler and watch the counts move, which a counter living in
    // the conductor could not witness without the engine.
    //
    // COUNTS, NOT JUST THE LAST ONE. "Are my reads served early" is a
    // DISTRIBUTION; any single sample can be lucky.
    uint64_t trigger_count(FlushTrigger t) const;
    FlushTrigger last_trigger() const;
    uint64_t last_flush_id() const;
    const FlushConfig& config() const { return cfg_; }

private:
    mutable std::mutex mu_;
    //: ck-43u3: one slot per FlushTrigger, guarded by mu_ like everything else
    //: here. Not atomics: they are written inside plan_flush, which already
    //: holds the lock, and read by a query that takes it.
    // ck-tqx2: the bound is DERIVED, not a literal. It was `[6]` here with a
    // hardcoded `< 6` at two use sites — so adding a seventh FlushTrigger would
    // have left the array at six and silently dropped the new trigger's count.
    // A census that loses a member reports FEWER triggers than fired, which is
    // the greener and therefore dangerous direction: the telemetry would have
    // looked clean precisely when a new trigger was unaccounted for.
    static constexpr std::size_t kNumTriggers =
        static_cast<std::size_t>(FlushTrigger::_Count);
    uint64_t     trigger_counts_[kNumTriggers] = {};
    FlushTrigger last_trigger_ = FlushTrigger::None;
    FlushConfig cfg_;

    std::vector<BufferedCall> buffer_;
    std::unordered_map<std::string, size_t> per_session_depth_;
    std::unordered_set<std::string> registered_;
    std::unordered_set<std::string> submitted_since_flush_;

    uint64_t next_ticket_   = 1;
    uint64_t next_seq_      = 1;
    uint64_t flush_id_      = 0;
    uint64_t last_flush_ms_ = 0;
    uint64_t rr_cursor_     = 0;

    FlushTrigger due_locked(uint64_t now_ms) const;
};

}  // namespace bridge
