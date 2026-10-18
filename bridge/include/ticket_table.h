// ticket_table.h — R1b: the async ticket layer (ck-6kvq.9).
//
// Design of record: docs/design/event-pipeline-conductor.md §2.4, §2.8.
//
// WHAT THIS IS. A generalisation of a mechanism the bridge already runs: the
// intent queue's store_completed / completed_snapshot / find_completed_by_
// request_id and its 128-deep completed ring (intent_queue.h, sized by a real
// incident where a burst of intents evicted a verdict before its poller read
// it). That ring records the fate of INTENTS. This one records the fate of
// EVERY buffered call — reads included — so an agent can fire and collect.
//
// PURITY. No engine, no game memory, no bridge.h: a ticket is {id, session,
// action, polarity, state, attribution, result-json}. That keeps it host
// testable (test_pipeline.cpp) with no CK3 binary, exactly like the scheduler.
//
// TWO KEYS, ONE RECORD — this is what makes the migration safe (§8.2). A ticket
// is addressable by its ticket id (the new async client) AND by the request_id
// the client already mints for every call today (mcp_server.py:825-858). A
// legacy client that ignores `ticket` and blocks on its request_id still gets
// its result, just later; it simply experiences the call as slower, bounded by
// the flush period. No rewrite, no flag day.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "flush_scheduler.h"

namespace bridge {

// ============================================================================
// Ticket lifecycle (§2.4)
// ============================================================================
enum class TicketState {
    Pending,   // buffered, not yet selected by a flush
    Running,   // selected by a flush; being served / draining on the game thread
    Done,      // a result was produced (which may itself be {success:false})
    Failed,    // the bridge could not produce a result (crash, refusal, drain loss)
    Expired,   // aged out of the ring before it was collected
};

const char* ticket_state_name(TicketState s);

// ============================================================================
// One ticket
// ============================================================================
struct Ticket {
    uint64_t    id = 0;              // monotonic; wire form is "tk_%08llu"
    std::string session_id;
    std::string request_id;          // the client's own key (may be empty)
    std::string action;
    Polarity    polarity = Polarity::PAUSED;
    TicketState state    = TicketState::Pending;

    // ---- the attribution spine (§2.8) -------------------------------------
    // Minted by the bridge, NEVER reported by the agent. This is the
    // unforgeable spine of R4, R9 and every mediator in the paper.
    uint64_t    flush_id = 0;
    uint32_t    character_id = 0;        // the session's primary at execution time
    uint64_t    tick_executed = 0;
    std::string game_date_executed;      // "YYYY.MM.DD", or "" before execution
    //: ck-jg2b. WHY character_id is 0, when it is. EMPTY means "attributed" —
    //: a real character_id is in the field above. Non-empty names the leg:
    //:   "claimless_session"  the session exists and holds no primary character
    //:   "session_gone"       the ticket resolved but the session no longer exists
    //: Before this, a claimless session's action was stamped with
    //: g_controlled_primary — SOMEONE ELSE'S character — so the record named an
    //: actor that had not acted. A 0 with a reason is a fact; a borrowed id is
    //: a fabrication that reads as evidence.
    std::string attribution_reason;

    // ---- payload ----------------------------------------------------------
    // The handler's own response object, verbatim, carrying the existing
    // {success, message, reason, phase} taxonomy INSIDE it (§2.4) so "how far
    // did it get" is never ambiguous.
    std::string result_json;

    uint64_t    created_ms = 0;
    uint64_t    updated_ms = 0;
};

// Wire form of a ticket id: "tk_00000123". Parsing accepts the wire form and a
// bare decimal; returns 0 when neither.
std::string ticket_id_to_string(uint64_t id);
uint64_t    ticket_id_from_string(const char* s);

// ============================================================================
// TicketTable — a bounded ring with a TTL and two indices.
// ============================================================================
//
// CAPACITY. Ring depth defaults to 512 (4x the intent ring's 128, because this
// records reads as well as intents and N=6 agents share it). Eviction is
// oldest-first and an evicted-but-uncollected ticket is reported as `expired`
// on the next `check` rather than vanishing — the intent ring's incident was
// exactly a silent eviction.
struct TicketTableConfig {
    size_t   capacity = 512;
    uint64_t ttl_ms   = 300000;   // 5 min; a collected result is not needed longer
};

class TicketTable {
public:
    using Config = TicketTableConfig;

    explicit TicketTable(const TicketTableConfig& cfg = TicketTableConfig());

    // Create a ticket in Pending. `id` is supplied by the scheduler's submit()
    // so the two agree on one number.
    void open(uint64_t id, const std::string& session_id, const std::string& request_id,
              const std::string& action, Polarity polarity, uint64_t now_ms);

    // Pending -> Running, stamping the flush that selected it. Returns TRUE
    // only if that transition actually happened. False means the ticket is
    // unknown or already terminal — a call cancelled between the plan being cut
    // and the plan being applied is exactly that, and the apply layer uses this
    // answer to skip it rather than serve work nobody is waiting for.
    bool mark_running(uint64_t id, uint64_t flush_id, uint64_t now_ms);

    // Running -> Done, stamping the full attribution record.
    void complete(uint64_t id, const std::string& result_json, uint64_t flush_id,
                  uint32_t character_id, uint64_t tick_executed,
                  const std::string& game_date, uint64_t now_ms,
                  const std::string& attribution_reason = std::string());

    // -> Failed. `result_json` carries the typed reason.
    void fail(uint64_t id, const std::string& result_json, uint64_t now_ms);

    // Lookups. Return false when the id/request_id is not in the ring at all —
    // which `check` must report as a TYPED refusal (unknown_ticket), never as
    // silence (§2.4 acceptance).
    bool find(uint64_t id, Ticket* out) const;
    bool find_by_request_id(const std::string& request_id, Ticket* out) const;

    // Expire anything older than the TTL that was never collected. Returns how
    // many were expired.
    size_t expire_older_than(uint64_t now_ms);

    size_t size() const;
    std::vector<Ticket> snapshot() const;

    // How many tickets were evicted by the capacity bound over this table's
    // life. A nonzero value means a client is not collecting; it is published
    // so the condition is loud.
    uint64_t evicted_count() const;

private:
    mutable std::mutex mu_;
    TicketTableConfig cfg_;
    std::deque<Ticket> ring_;
    std::unordered_map<uint64_t, size_t> by_id_;        // id -> ring index
    std::unordered_map<std::string, uint64_t> by_rid_;  // request_id -> id
    uint64_t evicted_ = 0;

    void reindex_locked();
    Ticket* find_locked(uint64_t id);
};

}  // namespace bridge
