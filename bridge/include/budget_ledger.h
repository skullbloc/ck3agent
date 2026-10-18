// budget_ledger.h — R6: the decision-budget governor's ENFORCING half (ck-6kvq.13).
//
// Design of record: docs/design/event-pipeline-conductor.md §5 (decisions G6,
// G7), as amended by the progenitor's 2026-09-12 correction restoring
// enforcement in full.
//
// WHAT IS COUNTED (§5.1 / D4)
// ---------------------------
// output tokens + uncached input tokens, PER SESSION, PER GAME YEAR. Cache
// reads and cache writes are RECORDED and reported and NEVER counted against
// the ceiling. `BudgetUsage::billed()` is the only thing the ceiling ever looks
// at, and it is written as an explicit two-term sum so that adding a field to
// the struct can never silently start charging for it.
//
// WHY THE LEDGER IS HERE AND NOT IN THE AGENT (§5.3 / G7)
// -------------------------------------------------------
// Reporting is the supervisor's; ENFORCEMENT is the bridge's. An agent cannot
// be trusted to stop itself, and the only thing that can actually stop one
// acting is the thing that decides whether its calls are accepted. The
// supervisor tails its own claude-CLI transcript (scripts/cost_tap.py's
// per-message layer — B-path, decision G6) and reports raw token counts; the
// BRIDGE decides the game year, holds the ledger, and refuses.
//
// That split is what makes the year key unforgeable: a supervisor reports
// tokens, never a date. The year comes from the engine's own calendar
// (`ck3::game_date`, ck-05mg) via observe_clock().
//
// WHAT A SPENT SESSION CAN STILL DO
// ---------------------------------
// Everything except spend the world's state. MUTATIONS ONLY are refused, with
// a typed reason carrying {reason:"budget_exhausted", session_id, game_date,
// spend, ceiling, resets_at}. Reads, `board_read` and bridge-management verbs
// (including `report_usage` itself, and `check`/`cancel`, so a spent session
// can still collect tickets it already has in flight) continue untouched. At
// the game-year rollover the ledger key changes, the new year has no entry, and
// the session rejoins with a full allowance.
//
// Refusals are logged and COUNTED, per session and per year, and exported into
// the episode bundle. A refusal never excludes an episode.
//
// PURITY: this translation unit has NO engine dependency — no offsets, no
// game-object reads, not even a stub global — so bridge/test/test_budget_ledger
// compiles it standalone on a host with no CK3 anywhere. That is deliberate:
// the enforcement rule is exactly the kind of thing that must be provable
// without a running game.

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bridge {

// ============================================================================
// The four numbers, and which two of them are billed
// ============================================================================
struct BudgetUsage {
    int64_t output_tokens = 0;
    int64_t uncached_input_tokens = 0;
    int64_t cache_read_tokens = 0;   // recorded, never billed (D4)
    int64_t cache_write_tokens = 0;  // recorded, never billed (D4)

    // §5.1 / D4. Explicit two-term sum, never "total minus cache".
    int64_t billed() const { return output_tokens + uncached_input_tokens; }

    bool is_zero() const {
        return !(output_tokens || uncached_input_tokens ||
                 cache_read_tokens || cache_write_tokens);
    }

    BudgetUsage& operator+=(const BudgetUsage& o) {
        output_tokens         += o.output_tokens;
        uncached_input_tokens += o.uncached_input_tokens;
        cache_read_tokens     += o.cache_read_tokens;
        cache_write_tokens    += o.cache_write_tokens;
        return *this;
    }
};

// One (session, game-year) row — what `run_summary.json`'s budget.spent[]
// carries, and what the acceptance test reads back.
struct BudgetRow {
    std::string session_id;
    int32_t     game_year = 0;
    BudgetUsage usage;
    int64_t     rounds    = 0;   // decision rounds: reported, never billed
    int64_t     messages  = 0;   // distinct transcript messages charged
    int64_t     refusals  = 0;   // mutations refused in THIS year
    bool        exhausted = false;
    std::string first_charge_date;
    std::string last_charge_date;
};

// One refused mutation. The design names these four fields explicitly; action
// and game_year are additive and free.
struct BudgetRefusal {
    uint64_t    seq = 0;
    std::string session_id;
    std::string game_date;
    int32_t     game_year = 0;
    std::string action;
    int64_t     spend = 0;
    int64_t     ceiling = 0;
};

// The answer to both "did this report cross the line" and "may this call run".
struct BudgetVerdict {
    bool        allowed = true;
    std::string reason;        // "" when allowed; "budget_exhausted" otherwise
    std::string session_id;
    std::string game_date;
    std::string resets_at;     // first day of the next game year
    int32_t     game_year = 0;
    int64_t     spend = 0;     // billed tokens this session spent this year
    int64_t     ceiling = 0;   // 0 => no cap configured
    bool        crossed = false;  // this report is the one that crossed
};

// ============================================================================
// The ledger
// ============================================================================
class BudgetLedger {
public:
    // A message-id map that grows without bound would be a slow leak in a
    // 20-game-year episode, so the per-session delta tracker is a FIFO ring.
    static constexpr size_t kMaxTrackedMessages = 8192;
    // Refusals are counted without bound; only the detail ring is capped.
    static constexpr size_t kMaxRefusalRing = 512;

    // ceiling_tokens <= 0 means NO CAP: record everything, refuse nothing.
    // That is the shipped default, because the ceiling is a per-episode config
    // value (1.5 x the pilot median, §5.3) that does not exist until the pilots
    // run. A positive value arms the stop.
    void configure(int64_t ceiling_tokens, bool enabled);
    int64_t ceiling() const;
    bool    enabled() const;

    // Cheap lock-free "is the stop armed at all?". The dispatch choke point
    // runs this on EVERY action, and in the shipped default (no ceiling) it
    // must cost one relaxed atomic load and nothing else — the same standard
    // the pipeline's legacy mode holds itself to.
    bool armed() const { return armed_.load(std::memory_order_relaxed); }

    // The clock. `year` is the engine's calendar year and `game_date` its
    // canonical YYYY.MM.DD rendering. MONOTONIC by construction: a year that
    // goes backwards (a bridge restart, a re-anchor, a glitched read) is
    // counted as a regression and IGNORED for keying, so nothing can refill an
    // allowance that has already been spent.
    void observe_clock(int32_t year, const std::string& game_date);
    int32_t     game_year() const;
    std::string game_date() const;
    std::string resets_at() const;
    int64_t     clock_regressions() const;

    // Report usage for a session. When `message_id` is non-empty the report is
    // treated as the CUMULATIVE total for that message and only the positive
    // per-field DELTA is charged — the claude CLI rewrites a message's
    // transcript entry as it streams, so a re-report must be charged once in
    // total, and a shrinking rewrite must never hand an agent a refund.
    // An empty `message_id` charges the usage as a straight delta.
    BudgetVerdict report(const std::string& session_id,
                         const std::string& message_id,
                         const BudgetUsage& usage,
                         int64_t rounds = 0);

    // THE GATE. `is_mutation` is the caller's (the dispatch choke point's)
    // classification; this class deliberately does not own the action taxonomy.
    // A refusal is recorded and counted here, so there is no path that refuses
    // a call without the episode record knowing.
    BudgetVerdict evaluate(const std::string& session_id,
                           const std::string& action,
                           bool is_mutation);

    // Non-recording query of the same predicate (telemetry, window frames).
    bool is_exhausted(const std::string& session_id) const;

    std::vector<BudgetRow>     rows() const;
    std::vector<BudgetRefusal> recent_refusals() const;
    int64_t refusal_count() const;
    int64_t refusal_count_for(const std::string& session_id) const;

    // The bundle payload: per-session per-year spend plus refusal counts.
    std::string export_json() const;

    void reset();

private:
    struct Entry {
        BudgetUsage usage;
        int64_t rounds = 0;
        int64_t messages = 0;
        int64_t refusals = 0;
        bool exhausted = false;
        std::string first_charge_date;
        std::string last_charge_date;
    };

    // (session_id, game_year) -> entry. std::map so rows() is ordered and two
    // runs diff cleanly.
    using Key = std::pair<std::string, int32_t>;

    struct SeenMessages {
        std::unordered_map<std::string, BudgetUsage> charged;
        std::deque<std::string> order;
    };

    Entry& entry_locked(const std::string& session_id);
    BudgetVerdict verdict_locked(const std::string& session_id,
                                 const Entry& e, bool allowed,
                                 const char* reason) const;

    mutable std::mutex mu_;
    std::map<Key, Entry> ledger_;
    std::unordered_map<std::string, SeenMessages> seen_;
    std::deque<BudgetRefusal> refusal_ring_;
    std::map<std::string, int64_t> refusals_by_session_;
    int64_t refusals_total_ = 0;
    uint64_t refusal_seq_ = 0;
    std::atomic<bool> armed_{false};
    int64_t ceiling_ = 0;
    bool enabled_ = true;
    int32_t year_ = 0;
    bool year_seen_ = false;
    std::string game_date_;
    int64_t clock_regressions_ = 0;
};

// The bridge's one ledger. Defined in budget_ledger.cpp.
BudgetLedger& budget_ledger();

// ============================================================================
// Where the gate applies
// ============================================================================
// True when `action_type` is the kind of call a spent session must not make.
// Deliberately NOT the same thing as `action_is_mutation` (which is simply
// !readonly): bridge-management verbs, `check`/`cancel`, `report_usage` itself
// and `board_read` are how a spent session keeps reading, logging and settling
// work it already has in flight (§5.3). The caller supplies the two bridge
// classifiers so this stays engine-free and host-testable.
bool budget_gate_applies(const char* action_type,
                         bool is_mutation,
                         bool is_management_verb);

// ============================================================================
// Engine-facing glue — implemented in dispatch_router.cpp, NOT here
// ============================================================================
// Declared here so the pure TU above stays free of every engine header while
// the choke point still has one place to look. `budget_ledger.cpp` defines
// none of these.
class ResponseBuilder;

// Arm the ceiling from the per-episode config (CK3_BUDGET_PER_AGENT_PER_YEAR,
// with CK3_BUDGET_ENFORCE=0 to record without refusing). Called from bridge
// init; safe to call twice. A ceiling set this way is PINNED: `set_budget_config`
// cannot raise or lower it, so an agent cannot lift its own cap.
void budget_init_from_env();
bool budget_config_is_pinned();

// Push the engine's current calendar year + date into the ledger. Cheap
// (one root read + one int32 read); a no-op when the game-state root does not
// resolve, so a date the bridge cannot read never moves the year.
void budget_observe_engine_clock();

// `budget` object on get_tick_info, next to `pipeline` (§2.10).
void budget_emit_state(ResponseBuilder& r);

}  // namespace bridge
