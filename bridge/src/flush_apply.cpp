// flush_apply.cpp — R1b: applying a FlushPlan (ck-6kvq.9). See flush_apply.h.
//
// Pure: no engine, no game memory, no bridge.h. Every effect goes through the
// FlushEffects interface, which is what lets test_pipeline.cpp drive the whole
// apply + ticket layer on a host with no CK3 binary.

#include "flush_apply.h"

namespace bridge {

namespace {

// Serve one call inline and settle its ticket.
//
// A ticket that will not move Pending -> Running is already settled: the
// overwhelmingly common cause is a `cancel` that landed after this plan was cut
// (§2.10 calls cancel "best-effort, before the flush that would run it" — this
// is the "best-effort" half). Serving it anyway would run work the client
// withdrew and emit a result frame for it, which a request_id-keyed client
// would resolve its call on. Skip it, and count the skip.
void serve_one(const BufferedCall& call, FlushEffects& fx, TicketTable& tickets,
               uint64_t flush_id, ApplyResult& out, size_t* counter) {
    if (!tickets.mark_running(call.ticket, flush_id, fx.now_ms())) {
        ++out.skipped_settled;
        return;
    }

    std::string result = fx.serve(call);
    const ExecutionStamp st = fx.stamp(call);

    if (result.empty()) {
        // A handler that produced nothing is a FAILED ticket carrying a typed
        // reason — never a silent loss. This is the same contract the
        // game-thread drain's `game_thread_no_reply` verdict already keeps.
        result = "{\"success\":false,\"reason\":\"no_reply\",\"phase\":\"serve\"}";
        tickets.fail(call.ticket, result, fx.now_ms());
        ++out.failed;
    } else {
        // ck-jg2b: carry the stamp's typed reason onto the ticket. Without
        // this the inline path would record a 0 with no explanation, which is
        // the unresolved-value-that-looks-measured shape this bead exists to
        // remove — a reader could not tell a claimless session from a bug.
        tickets.complete(call.ticket, result, flush_id, st.character_id,
                         st.tick_executed, st.game_date, fx.now_ms(),
                         st.attribution_reason);
        if (counter) ++(*counter);
    }
    fx.emit_result(call, result);
}

}  // namespace

ApplyResult apply_flush_plan(const FlushPlan& plan, FlushEffects& fx, TicketTable& tickets) {
    ApplyResult out;
    out.flush_id = plan.flush_id;
    out.clock_drift_observed = plan.clock_drift_observed;

    if (plan.trigger == FlushTrigger::None) return out;

    const uint64_t t_start = fx.now_ms();
    uint64_t pause_began_ms = 0;
    bool in_pause_span = false;

    // §2.3: a flush that touches no pause-needing work must not touch the clock
    // at all — not even to "restore" it. The scheduler already encodes that by
    // emitting no clock steps; we surface the drift it observed instead of
    // quietly healing it, so the hot path stays provably clock-silent.
    if (plan.clock_drift_observed) fx.note_clock_drift(plan);

    for (const PlanStep& step : plan.steps) {
        switch (step.kind) {
            case StepKind::ServeAny:
                for (const BufferedCall& c : step.calls) {
                    serve_one(c, fx, tickets, plan.flush_id, out, &out.served_any);
                }
                break;

            case StepKind::Pause: {
                ++out.clock_effects;
                if (!fx.pause()) ++out.clock_effect_failures;
                pause_began_ms = fx.now_ms();
                in_pause_span = true;
                break;
            }

            case StepKind::ServePaused:
                // ONE span serves ALL of them: every PAUSED read in this flush
                // is taken from one consistent, non-ticking world (§2.6).
                for (const BufferedCall& c : step.calls) {
                    serve_one(c, fx, tickets, plan.flush_id, out, &out.served_paused);
                }
                break;

            case StepKind::Unpause: {
                ++out.clock_effects;
                if (!fx.unpause()) ++out.clock_effect_failures;
                if (in_pause_span) {
                    const uint64_t now = fx.now_ms();
                    if (now > pause_began_ms) out.paused_ms += now - pause_began_ms;
                    in_pause_span = false;
                }
                break;
            }

            case StepKind::DrainTick:
                // step.calls is already READS-THEN-WRITES (§2.2). Preserve that
                // order exactly: a deferred-drain read ordered behind this
                // flush's writes would answer from a world that had already
                // moved past another agent's write in the same flush.
                for (const BufferedCall& c : step.calls) {
                    if (!fx.routes_to_drain(c)) {
                        // IPC-thread-safe deferred call: the flush has already
                        // guaranteed a live clock, which is the only thing the
                        // old per-call pulse was buying (§2.5). serve_one does
                        // its own Pending->Running claim.
                        serve_one(c, fx, tickets, plan.flush_id, out, &out.served_tick);
                        continue;
                    }
                    if (!tickets.mark_running(c.ticket, plan.flush_id, fx.now_ms())) {
                        ++out.skipped_settled;   // cancelled after the plan was cut
                        continue;
                    }
                    if (fx.enqueue_for_drain(c)) {
                        // Left RUNNING on purpose: the game-thread drain
                        // reports back and completes the ticket. It is never
                        // completed here on the strength of having been
                        // enqueued.
                        ++out.enqueued_tick;
                    } else {
                        const std::string r =
                            "{\"success\":false,\"reason\":\"drain_enqueue_failed\","
                            "\"phase\":\"enqueue\"}";
                        tickets.fail(c.ticket, r, fx.now_ms());
                        ++out.failed;
                        fx.emit_result(c, r);
                    }
                }
                break;

            case StepKind::SetSpeed:
                ++out.clock_effects;
                if (!fx.set_speed(step.speed)) ++out.clock_effect_failures;
                break;
        }
    }

    if (in_pause_span) {
        const uint64_t now = fx.now_ms();
        if (now > pause_began_ms) out.paused_ms += now - pause_began_ms;
    }

    const uint64_t t_end = fx.now_ms();
    fx.note_flush_record(plan, t_end > t_start ? t_end - t_start : 0, out.paused_ms);
    return out;
}

}  // namespace bridge
