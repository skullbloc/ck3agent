// flush_scheduler.cpp — R1a: the buffered, polarity-driven flush loop (ck-6kvq.8).
//
// See flush_scheduler.h for the shape ruling. Deliberately dependency-free:
// <cstdint>/<string>/<vector>/<mutex> and nothing from the bridge or the game.
// A flush is a PLAN, not an effect; ck-6kvq.9 applies it.

#include "flush_scheduler.h"

#include <algorithm>

namespace bridge {

// ============================================================================
// Names
// ============================================================================
const char* polarity_name(Polarity p) {
    switch (p) {
        case Polarity::ANY:        return "any";
        case Polarity::PAUSED:     return "paused";
        case Polarity::TICK_READ:  return "tick_read";
        case Polarity::TICK_WRITE: return "tick_write";
        case Polarity::UNKNOWN:    return "unknown";
    }
    return "unknown";
}

bool is_tick_polarity(Polarity p) {
    return p == Polarity::TICK_READ || p == Polarity::TICK_WRITE;
}

Polarity resolve_polarity(Polarity p) {
    // Unclassified -> PAUSED. Identical to the gateway default
    // (mcp_server.py:166-177): the safe read default, never a silent mutation.
    return (p == Polarity::UNKNOWN) ? Polarity::PAUSED : p;
}

const char* flush_trigger_name(FlushTrigger t) {
    switch (t) {
        case FlushTrigger::None:                 return "none";
        case FlushTrigger::Cadence:              return "cadence";
        case FlushTrigger::AllAny:               return "all_any";
        case FlushTrigger::AllSessionsSubmitted: return "all_sessions_submitted";
        case FlushTrigger::SizeBound:            return "size_bound";
        case FlushTrigger::ReadLaneEarly:        return "read_lane_early";
        case FlushTrigger::_Count:               break;   // not a trigger
    }
    return "none";
}

const char* step_kind_name(StepKind k) {
    switch (k) {
        case StepKind::ServeAny:    return "serve_any";
        case StepKind::Pause:       return "pause";
        case StepKind::ServePaused: return "serve_paused";
        case StepKind::Unpause:     return "unpause";
        case StepKind::DrainTick:   return "drain_tick";
        case StepKind::SetSpeed:    return "set_speed";
    }
    return "?";
}

// ============================================================================
// FlushPlan helpers
// ============================================================================
size_t FlushPlan::pause_count() const {
    size_t n = 0;
    for (const PlanStep& s : steps) if (s.kind == StepKind::Pause) ++n;
    return n;
}

size_t FlushPlan::unpause_count() const {
    size_t n = 0;
    for (const PlanStep& s : steps) if (s.kind == StepKind::Unpause) ++n;
    return n;
}

bool FlushPlan::clock_touched() const {
    for (const PlanStep& s : steps) {
        if (s.kind == StepKind::Pause || s.kind == StepKind::Unpause ||
            s.kind == StepKind::SetSpeed) {
            return true;
        }
    }
    return false;
}

size_t FlushPlan::selected_count() const {
    return any_calls.size() + paused_calls.size() + tick_calls.size();
}

// ============================================================================
// Round-robin selection
// ============================================================================
// Replaces the global FIFO under MAX_ACTIONS_PER_TICK (bridge_init.cpp:1497).
// Sessions are ordered by first arrival within `pool`, the starting session is
// rotated by `rr_start` so the head of the list does not win every flush, and
// one call per session is taken per round until the bound is reached. Anything
// left over is DEFERRED — it keeps its ticket and its seq, so it sits at the
// head of the next flush's pool.
static void rr_select(const std::vector<BufferedCall>& pool,
                      size_t bound,
                      uint64_t rr_start,
                      std::vector<BufferedCall>& selected,
                      std::vector<BufferedCall>& deferred) {
    selected.clear();
    deferred.clear();
    if (pool.empty()) return;
    if (bound == 0) { deferred = pool; return; }

    // Session order = first appearance in arrival order.
    std::vector<std::string> sessions;
    std::vector<std::vector<const BufferedCall*>> queues;
    for (const BufferedCall& c : pool) {
        size_t idx = sessions.size();
        for (size_t i = 0; i < sessions.size(); ++i) {
            if (sessions[i] == c.session_id) { idx = i; break; }
        }
        if (idx == sessions.size()) {
            sessions.push_back(c.session_id);
            queues.emplace_back();
        }
        queues[idx].push_back(&c);
    }

    const size_t n = sessions.size();
    const size_t start = static_cast<size_t>(rr_start % n);
    std::vector<size_t> cursor(n, 0);
    std::vector<bool> taken(pool.size(), false);

    bool progress = true;
    while (selected.size() < bound && progress) {
        progress = false;
        for (size_t i = 0; i < n && selected.size() < bound; ++i) {
            const size_t s = (start + i) % n;
            if (cursor[s] >= queues[s].size()) continue;
            const BufferedCall* c = queues[s][cursor[s]++];
            selected.push_back(*c);
            taken[static_cast<size_t>(c - pool.data())] = true;
            progress = true;
        }
    }

    for (size_t i = 0; i < pool.size(); ++i) {
        if (!taken[i]) deferred.push_back(pool[i]);
    }
}

// ============================================================================
// FlushScheduler
// ============================================================================
FlushScheduler::FlushScheduler(const FlushConfig& cfg) : cfg_(cfg) {}

// ck-on4u. The planner's resting speed is the episode's DECLARED one. Refuses a
// non-speed rather than storing it: the restore step below compares against
// this value and emits a SetSpeed carrying it, and `game_set_speed` clamps
// anything it dislikes — so a bad value here becomes a clock write that no
// declaration asked for and that the plan's own record misreports.
bool FlushScheduler::set_resting_speed(int speed) {
    if (!game_speed_in_range(speed)) return false;
    cfg_.resting_speed = speed;
    return true;
}

void FlushScheduler::set_privileged_reader(const std::string& session_id) {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_.privileged_reader = session_id;
}

void FlushScheduler::set_hold_clock_until_observer(bool on) {
    std::lock_guard<std::mutex> lk(mu_);
    cfg_.hold_clock_until_observer = on;
}

void FlushScheduler::register_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lk(mu_);
    registered_.insert(session_id);
}

void FlushScheduler::unregister_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lk(mu_);
    registered_.erase(session_id);
    submitted_since_flush_.erase(session_id);
    // Buffered calls are NOT dropped: a detached session's tickets survive
    // (ck-6kvq.5 resume). They are still flushed and still attributed.
}

size_t FlushScheduler::registered_session_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return registered_.size();
}

SubmitResult FlushScheduler::submit(const CallRequest& req, uint64_t now_ms) {
    std::lock_guard<std::mutex> lk(mu_);

    SubmitResult r;
    r.polarity = resolve_polarity(req.polarity);

    // Back-pressure, never a silent drop (§2.8).
    const size_t sess_depth = per_session_depth_.count(req.session_id)
                                  ? per_session_depth_[req.session_id]
                                  : 0;
    if (buffer_.size() >= cfg_.buffer_capacity ||
        sess_depth >= cfg_.per_session_capacity) {
        r.accepted       = false;
        r.reason         = "buffer_full";
        r.retry_after_ms = cfg_.retry_after_ms;
        r.limit = (buffer_.size() >= cfg_.buffer_capacity) ? BackPressureLimit::Global
                                                           : BackPressureLimit::Session;
        return r;
    }

    BufferedCall c;
    c.ticket               = next_ticket_++;
    c.seq                  = next_seq_++;
    c.session_id           = req.session_id;
    c.action               = req.action;
    c.polarity             = r.polarity;
    c.polarity_was_unknown = (req.polarity == Polarity::UNKNOWN);
    c.enqueued_ms          = now_ms;
    buffer_.push_back(c);
    per_session_depth_[req.session_id] = sess_depth + 1;
    submitted_since_flush_.insert(req.session_id);

    r.accepted = true;
    r.ticket   = c.ticket;
    const uint64_t elapsed = (now_ms >= last_flush_ms_) ? (now_ms - last_flush_ms_) : 0;
    r.flush_eta_ms = (elapsed >= cfg_.period_ms) ? 0 : (cfg_.period_ms - elapsed);
    return r;
}

// ck-6kvq.9, §2.10 `cancel`. Withdrawing the buffered descriptor is what makes
// cancel mean something: leaving it in the buffer and merely failing the ticket
// still lets the call consume one of this flush's bounded tick/paused slots, and
// still drives it through the apply layer's serve path — which then emits a
// spurious result frame for a call the client explicitly withdrew.
bool FlushScheduler::cancel(uint64_t ticket) {
    std::lock_guard<std::mutex> lk(mu_);
    for (size_t i = 0; i < buffer_.size(); ++i) {
        if (buffer_[i].ticket != ticket) continue;
        const std::string sid = buffer_[i].session_id;
        buffer_.erase(buffer_.begin() + static_cast<long>(i));
        auto it = per_session_depth_.find(sid);
        if (it != per_session_depth_.end()) {
            if (it->second > 0) --it->second;
            if (it->second == 0) per_session_depth_.erase(it);
        }
        return true;
    }
    return false;
}

FlushTrigger FlushScheduler::due_locked(uint64_t now_ms) const {
    const uint64_t elapsed = (now_ms >= last_flush_ms_) ? (now_ms - last_flush_ms_) : 0;
    const bool cadence_due = elapsed >= cfg_.period_ms;

    if (buffer_.empty()) {
        // Nothing pending: no EARLY condition can fire (an empty buffer is not
        // "all ANY work" and nobody has spoken). The cadence still fires as the
        // flush-liveness heartbeat that last_flush_ms exposes (G9).
        return cadence_due ? FlushTrigger::Cadence : FlushTrigger::None;
    }

    // (a) all-ANY: no clock cycle is needed at all, so waiting is pure latency.
    bool all_any = true;
    for (const BufferedCall& c : buffer_) {
        if (c.polarity != Polarity::ANY) { all_any = false; break; }
    }
    if (all_any) return FlushTrigger::AllAny;

    // (b) everyone registered has spoken since the last flush. Recovers the
    // "everyone has spoken" property WITHOUT the world ever waiting for anyone:
    // if a session stays silent this simply does not fire and the cadence does.
    if (!registered_.empty()) {
        bool all_spoke = true;
        for (const std::string& s : registered_) {
            if (!submitted_since_flush_.count(s)) { all_spoke = false; break; }
        }
        if (all_spoke) return FlushTrigger::AllSessionsSubmitted;
    }

    // (D', ck-h6nx) THE DECLARED OBSERVER'S OWN SUBMISSION SERVES THE READ
    // LANE. Placed AFTER (b) so an all-spoke flush still wins and serves
    // everything: this trigger is the narrower case, not a replacement.
    if (!cfg_.privileged_reader.empty()
        && submitted_since_flush_.count(cfg_.privileged_reader)) {
        return FlushTrigger::ReadLaneEarly;
    }

    // (c) size bound.
    if (buffer_.size() >= cfg_.early_flush_size) return FlushTrigger::SizeBound;

    return cadence_due ? FlushTrigger::Cadence : FlushTrigger::None;
}

FlushTrigger FlushScheduler::due(uint64_t now_ms) const {
    std::lock_guard<std::mutex> lk(mu_);
    return due_locked(now_ms);
}

FlushPlan FlushScheduler::plan_flush(uint64_t now_ms,
                                     const ClockReading& clock,
                                     FlushTrigger trigger) {
    std::lock_guard<std::mutex> lk(mu_);

    // ck-43u3: count the trigger that produced this plan. Here rather than in
    // the conductor, because the trigger is this object's own decision — and
    // because a probe that ticks a scheduler can then WITNESS the count move,
    // which a counter in the conductor could not show without the engine.
    //
    // ck-tqx2: under the SAME lock the rest of the function already takes. It
    // was a scoped lock_guard immediately followed by re-acquiring mu_ — not a
    // deadlock, the scope closes first, but a redundant lock cycle on EVERY
    // flush. ck-t5bc flagged it as a timing perturbation on the flush path,
    // which is where the arm-A crash lives, so it is a weak confound for the
    // first instrumented run and worth closing before that run rather than
    // whenever this file is next touched.
    const std::size_t ti = static_cast<std::size_t>(trigger);
    if (ti < kNumTriggers) ++trigger_counts_[ti];
    last_trigger_ = trigger;

    FlushPlan plan;
    plan.trigger      = trigger;
    plan.wall_ms      = now_ms;
    plan.clock_before = clock;
    plan.clock_after  = clock;

    if (trigger == FlushTrigger::None) {
        plan.flush_id = flush_id_;  // no flush happened; id unchanged
        return plan;
    }

    plan.flush_id = ++flush_id_;

    // ---- partition by polarity (§2.2) --------------------------------------
    // The two tick halves share ONE selection pool, because they share one
    // MAX_ACTIONS_PER_TICK budget and one fairness axis. They are separated
    // again after selection so the plan can order reads ahead of writes.
    std::vector<BufferedCall> any_pool, paused_pool, tick_pool;
    for (const BufferedCall& c : buffer_) {
        if (c.polarity_was_unknown) ++plan.unclassified;
        switch (c.polarity) {
            case Polarity::ANY:        any_pool.push_back(c);    break;
            case Polarity::TICK_READ:
            case Polarity::TICK_WRITE: tick_pool.push_back(c);   break;
            case Polarity::PAUSED:
            case Polarity::UNKNOWN:
            default:                   paused_pool.push_back(c); break;
        }
    }

    // ---- bounded, round-robin batch selection (§2.8) ------------------------
    // ANY work is served inline with the clock untouched, so it is not rate
    // limited: bounding it would add latency to the one class that costs the
    // world nothing.
    plan.any_calls = any_pool;

    std::vector<BufferedCall> paused_def, tick_def, tick_selected;
    rr_select(paused_pool, cfg_.max_paused_calls_per_flush, rr_cursor_,
              plan.paused_calls, paused_def);

    // §2.2 TICK SPLIT. The tick budget is ONE budget shared by both halves, and
    // it is allocated by SESSION, not by class — §2.8's fairness axis is the
    // socket, and giving the read half first call on the whole budget lets one
    // session's deferred-drain reads starve every other session's writes
    // outright (at N=6 that is not theoretical; it is the exact failure the
    // round robin exists to prevent).
    //
    // ORDER is a separate question from BUDGET, and the reads win that one: a
    // deferred-drain read ordered behind this flush's writes would answer from a
    // world that had already moved past another agent's write in the same flush,
    // which silently breaks the §2.6 coherence claim. So: select fairly over the
    // combined pool, then apply reads first.
    rr_select(tick_pool, cfg_.max_tick_calls_per_flush, rr_cursor_,
              tick_selected, tick_def);
    for (const BufferedCall& c : tick_selected) {
        if (c.polarity == Polarity::TICK_READ) plan.tick_read_calls.push_back(c);
        else                                   plan.tick_write_calls.push_back(c);
    }

    plan.tick_calls = plan.tick_read_calls;
    plan.tick_calls.insert(plan.tick_calls.end(),
                           plan.tick_write_calls.begin(), plan.tick_write_calls.end());

    // (D') A READ-LANE FLUSH LEAVES EVERY TICK CALL BUFFERED. Selection above
    // already ran; here the write half is handed BACK to the buffer rather than
    // served. This is the whole difference from the plain observer-triggered
    // flush that was ruled inadmissible, and `by_read_flush` in the acceptance
    // is what proves it rather than this comment.
    if (trigger == FlushTrigger::ReadLaneEarly && !plan.tick_calls.empty()) {
        for (const BufferedCall& tc : plan.tick_calls) tick_def.push_back(tc);
        plan.tick_calls.clear();
        plan.tick_read_calls.clear();
        plan.tick_write_calls.clear();
    }

    plan.deferred = paused_def;
    plan.deferred.insert(plan.deferred.end(), tick_def.begin(), tick_def.end());
    std::sort(plan.deferred.begin(), plan.deferred.end(),
              [](const BufferedCall& a, const BufferedCall& b) { return a.seq < b.seq; });

    for (const BufferedCall& c : plan.any_calls)    ++plan.per_session_counts[c.session_id];
    for (const BufferedCall& c : plan.paused_calls) ++plan.per_session_counts[c.session_id];
    for (const BufferedCall& c : plan.tick_calls)   ++plan.per_session_counts[c.session_id];

    // ---- the script (§2.3) -------------------------------------------------
    ClockReading cur = clock;

    if (!plan.any_calls.empty()) {
        PlanStep s; s.kind = StepKind::ServeAny; s.calls = plan.any_calls;
        plan.steps.push_back(s);
    }

    // An all-ANY flush (which includes the empty heartbeat flush) touches NOTHING.
    // This invariant is absolute: the common agent hot path must never move the
    // clock. A paused reading here is drift we flag but do not act on.
    const bool all_any = paused_pool.empty() && tick_pool.empty();
    if (all_any) {
        plan.clock_drift_observed = clock.paused;
        plan.clock_after = cur;
        // fall through to buffer consumption
    } else {
        // ORDER: paused reads, THEN ticked writes. Every agent in this flush
        // reads the same world and sees the consequences in the NEXT flush.
        if (!plan.paused_calls.empty()) {
            if (!cur.paused) {
                PlanStep s; s.kind = StepKind::Pause;
                plan.steps.push_back(s);
                cur.paused = true;
            }
            PlanStep s; s.kind = StepKind::ServePaused; s.calls = plan.paused_calls;
            plan.steps.push_back(s);   // ONE span serves ALL of them
        }

        if (!plan.tick_calls.empty()) {
            if (cur.paused) {
                PlanStep s; s.kind = StepKind::Unpause;
                plan.steps.push_back(s);
                cur.paused = false;
            }
            PlanStep s; s.kind = StepKind::DrainTick; s.calls = plan.tick_calls;
            plan.steps.push_back(s);   // handed to the existing on_tick drain
        }

        // ---- restore the resting state: RUNNING at the DECLARED speed ------
        //
        // UNCONDITIONAL since 2026-09-12 (§2.7): the progenitor reversed G3', so
        // there is no gate that can hold the clock and the world's resting state
        // is always RUNNING. An agent's latency is its own; what it costs the
        // experiment is MEASURED (unavailability.h), not prevented.
        //
        // ck-on4u: THE SetSpeed STEP BELOW IS THE SECOND WRITER OF THE GAME
        // SPEED, and for a year nothing knew it. It reaches `game_set_speed`
        // through the effects vtable (flush_apply.cpp -> the
        // `set_speed(int) override` in pipeline.cpp -> do_set_speed), so a
        // textual grep for `game_set_speed(` finds ONE call site while two
        // paths write; and because it enters at `do_set_speed` rather than
        // through the IPC dispatcher it logs no "Action: set_speed" line, so a
        // log-based discriminator keyed on that line reports this write as
        // "not a client" — a CONFIDENT WRONG ANSWER, in the direction that made
        // the wrong conclusion look confirmed.
        //
        // IT FIRES EXACTLY ONCE PER WORLD, and the third line says why: the
        // planner updates its OWN model to the resting speed, so every later
        // plan sees agreement and emits nothing. A once-only signature is not
        // evidence of which writer you are watching.
        //
        // The step is NOT the defect and is not removed — it is what makes an
        // E0 episode free-run at a FIXED speed at all. Resting at a hard-coded
        // 3 over a pin of 2 was.
        // ── THE RESTING STATE IS PAUSED UNTIL AN OBSERVER DECLARES ─────
        // Bring-up and preflight are not the episode and must not consume game
        // time. While nobody holds the read-lane privilege there is no episode,
        // so the world rests PAUSED and the clock starts when the observer
        // declares. Re-asserted every flush, so it is a STATE rather than a
        // condition somebody has to remember to check.
        // ONE RELEASE SIGNAL, AND IT CANNOT FIRE EARLY (ck-8o5j).
        // This used to also release when `privileged_reader` was non-empty —
        // i.e. when the episode OBSERVER declared. ck-9rto found the hazard:
        // the standalone meter lane (meter_lane.py run) is the only thing in
        // the tree that declares, and if an operator starts it against a
        // pipeline bridge BEFORE the driver opens the episode, its declaration
        // released the clock during preflight — the exact defect ck-zen6 was
        // written to kill, arriving through the other signal.
        //
        // So the observer release is RETIRED. The hold is released by the
        // episode opening and by nothing else, which also retires the argument
        // that two release signals cannot race: an argument you do not need is
        // an argument you cannot get wrong later. The observer privilege goes
        // back to meaning only the read-lane early flush.
        if (cfg_.hold_clock_until_observer) {
            if (!cur.paused) {
                PlanStep s; s.kind = StepKind::Pause;
                plan.steps.push_back(s);
                cur.paused = true;
            }
            // ── OWNING A RESOURCE MEANS SETTING IT ─────────────────────────
            // The speed is driven to the declared pin even while PAUSED. The
            // flush loop excluded every session from the clock — set_speed and
            // set_pause both return clock_owned_by_pipeline — and then did not
            // drive it, so the world sat at CK3's default 3 while §7.3 declared
            // 2, and the driver correctly refused to start because live speed
            // did not equal the pin. Refusing everyone else from a resource and
            // not setting it yourself is the defect; a paused world at the pin
            // is what lets an arm START.
            if (cur.speed != cfg_.resting_speed) {
                PlanStep s; s.kind = StepKind::SetSpeed; s.speed = cfg_.resting_speed;
                plan.steps.push_back(s);
                cur.speed = cfg_.resting_speed;
            }
            plan.clock_after = cur;
        } else {
        if (cur.paused) {
            PlanStep s; s.kind = StepKind::Unpause;
            plan.steps.push_back(s);
            cur.paused = false;
        }
        if (cur.speed != cfg_.resting_speed) {
            PlanStep s; s.kind = StepKind::SetSpeed; s.speed = cfg_.resting_speed;
            plan.steps.push_back(s);
            cur.speed = cfg_.resting_speed;
        }
        plan.clock_after = cur;
        }
    }

    // ---- consume ------------------------------------------------------------
    buffer_ = plan.deferred;
    per_session_depth_.clear();
    for (const BufferedCall& c : buffer_) ++per_session_depth_[c.session_id];
    submitted_since_flush_.clear();
    last_flush_ms_ = now_ms;
    ++rr_cursor_;

    return plan;
}

FlushPlan FlushScheduler::tick(uint64_t now_ms, const ClockReading& clock) {
    const FlushTrigger t = due(now_ms);
    return plan_flush(now_ms, clock, t);
}

uint64_t FlushScheduler::trigger_count(FlushTrigger t) const {
    std::lock_guard<std::mutex> lk(mu_);
    const int i = static_cast<int>(t);
    return (i >= 0 && static_cast<std::size_t>(i) < kNumTriggers)
               ? trigger_counts_[i] : 0;
}

FlushTrigger FlushScheduler::last_trigger() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_trigger_;
}

size_t FlushScheduler::buffer_depth() const {
    std::lock_guard<std::mutex> lk(mu_);
    return buffer_.size();
}

uint64_t FlushScheduler::last_flush_ms() const {
    std::lock_guard<std::mutex> lk(mu_);
    return last_flush_ms_;
}

uint64_t FlushScheduler::last_flush_id() const {
    std::lock_guard<std::mutex> lk(mu_);
    return flush_id_;
}

}  // namespace bridge
