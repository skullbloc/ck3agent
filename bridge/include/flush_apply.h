// flush_apply.h — R1b: applying a FlushPlan against real effects (ck-6kvq.9).
//
// Design of record: docs/design/event-pipeline-conductor.md §2.3, §2.4, §2.8.
//
// ck-6kvq.8 split the flush in two on purpose: FlushScheduler emits a PLAN and
// performs no effect. This file is the other half — it walks the plan's ordered
// script and calls an EFFECTS interface for each step, driving the ticket table
// as it goes. The effects interface is abstract for exactly the reason the
// scheduler is pure: the real implementation pauses a game and dispatches
// engine handlers (pipeline.cpp), and a fake implementation lets the whole
// apply/ticket layer be host-tested with no CK3 binary.
//
// WHAT APPLY GUARANTEES
//  - The ordered script is applied verbatim: ServeAny, then (Pause,
//    ServePaused), then DrainTick (READS BEFORE WRITES — §2.2), then the
//    restore to the resting state.
//  - An all-ANY flush calls NO clock effect at all. That invariant is absolute
//    and is asserted, not merely intended: the common agent hot path
//    (liveness polls, rosters, `check`) must be provably incapable of moving
//    the world.
//  - Every ticket it touches moves Pending -> Running -> {Done|Failed}, with
//    the bridge-minted attribution record stamped on completion (§2.8).
//  - A DrainTick call is handed to the existing on_tick drain and left RUNNING;
//    the drain reports back later (pipeline.cpp's drain hook). It is never
//    silently completed here.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "flush_scheduler.h"
#include "ticket_table.h"

namespace bridge {

// The bridge-minted attribution stamped on every executed action (§2.8).
struct ExecutionStamp {
    uint32_t    character_id = 0;
    uint64_t    tick_executed = 0;
    std::string game_date;
    //: ck-jg2b: empty when character_id is real; otherwise names why it is 0.
    std::string attribution_reason;
};

// ============================================================================
// FlushEffects — everything applying a plan needs the outside world to do.
// ============================================================================
class FlushEffects {
public:
    virtual ~FlushEffects() = default;

    // Clock. Return false when the effect did not take (a wedged idler, a
    // no-op unpause); apply records that and keeps going rather than wedging.
    virtual bool pause()            = 0;
    virtual bool unpause()          = 0;
    virtual bool set_speed(int spd) = 0;

    // Serve one call to completion right here, returning its handler response
    // verbatim. Used for ANY, PAUSED and the IPC-thread-safe half of TICK_READ.
    virtual std::string serve(const BufferedCall& call) = 0;

    // Hand one call to the existing game-thread drain
    // (bridge_init.cpp's g_msg_queue -> drain_and_execute_intents). Returns
    // false when it could not be enqueued at all — which is a FAILED ticket,
    // never a lost one.
    virtual bool enqueue_for_drain(const BufferedCall& call) = 0;

    // True when this call must go to the game-thread queue rather than being
    // served inline. Mirrors the reader thread's existing routing decision.
    virtual bool routes_to_drain(const BufferedCall& call) const = 0;

    // Bridge-minted attribution for a call about to complete.
    virtual ExecutionStamp stamp(const BufferedCall& call) = 0;

    // Monotonic wall clock, milliseconds.
    virtual uint64_t now_ms() const = 0;

    // Emit a completed call's result to its client, keyed by the client's OWN
    // request_id (§8.2: this is what makes a legacy, ticket-ignorant client
    // still work in pipeline mode).
    virtual void emit_result(const BufferedCall& call, const std::string& result_json) = 0;

    // Optional observability hooks; the default implementations do nothing.
    virtual void note_clock_drift(const FlushPlan&) {}
    virtual void note_flush_record(const FlushPlan&, uint64_t /*wall_ms_elapsed*/,
                                   uint64_t /*paused_ms*/) {}
};

// ============================================================================
// The outcome of applying one plan (published, and asserted in tests).
// ============================================================================
struct ApplyResult {
    uint64_t flush_id = 0;
    size_t   served_any     = 0;
    size_t   served_paused  = 0;
    size_t   enqueued_tick  = 0;   // handed to the game-thread drain
    size_t   served_tick    = 0;   // TICK calls that were IPC-thread-safe
    size_t   failed         = 0;
    // Selected by the plan but already terminal when the plan was applied —
    // in practice a `cancel` that raced the flush. Skipped, not served, and
    // counted so "the flush did less than it planned" is never silent.
    size_t   skipped_settled = 0;
    size_t   clock_effects  = 0;   // pause/unpause/set_speed calls actually made
    size_t   clock_effect_failures = 0;
    uint64_t paused_ms      = 0;
    bool     clock_drift_observed = false;
};

// Apply one plan. `tickets` is mutated in place. Safe to call with a
// FlushTrigger::None plan (does nothing).
ApplyResult apply_flush_plan(const FlushPlan& plan, FlushEffects& fx, TicketTable& tickets);

}  // namespace bridge
