// deferred_drain.cpp — bridge-level pause-transparency coordinator.
//
// Implements the design in docs/notes/surface-verify/_PAUSE_TRANSPARENCY_DESIGN.md.
//
// A DEFERRED call (CB validity, acceptance, trigger / named-trigger eval,
// decision eval, conquest-targets) does its real work on the GAME thread inside
// drain_and_execute_intents(), which only runs while the sim ticks. Called while
// PAUSED, on_tick never fires, so:
//   - the bounded-poll deferred queries (query_valid_cbs, *_strict, acceptance,
//     named-trigger, decision-eval) returned a graceful `timeout_waiting_for_
//     game_thread` after ~2-7s — correct, but the caller had to KNOW to unpause; and
//   - the UNCLASSIFIED handlers (query_dejure_progress, query_conquest_targets)
//     fell through to g_msg_queue and never drained → the client HUNG to its own
//     timeout (the I15 hang, flagged P0 in the LiveVerify health matrix).
//
// run_deferred_pause_transparent() closes both: it records the live pause state,
// and — only if paused — pulses the game unpaused just long enough for one drain,
// runs the deferred service closure, then restores THE PAUSE FLAG on every exit
// path. The pause flag, and nothing else: it never writes the game speed
// (ck-cyhr — the speed half of the original restore reverted the pre-registered
// E0 speed pin on every meter sample). The speed is OBSERVED at pulse open and
// close and the divergence logged, because a tempo change somebody else made
// inside the window is evidence, not drift.
//
// Bounded throughout (no signal watchdog can land on the SIGALRM-masked
// reader thread — see ipc_server.cpp:564 — so every bound here is a POLLED budget,
// never a signal timeout: the I15 lesson). Caller-transparent: same response shape
// whether the game was paused or not. No-op (just runs the closure) when already
// unpaused — the common agent-loop hot path is never touched.

#include "inflight.h"
#include "bridge.h"
#include "ck3_offsets.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace bridge {

// --- Tunables (polled budgets; §3.4 of the design) ---------------------------
// Pulse-start budget: after game_unpause(), how long to wait for g_tick_id to
// advance before declaring the sim not-live (idler null / load / menu /
// transition). game_unpause() can silently no-op, so we DETECT real ticking
// rather than assume it.
static constexpr int PULSE_START_BUDGET_MS = 1500;
static constexpr int PULSE_START_POLL_MS   = 10;
// Lock budget: single-flight wait before returning busy_deferred rather than
// blocking a new caller behind a wedged holder. Sized to comfortably exceed the
// heaviest legitimate service window (query_conquest_targets can run up to ~6
// engine CB checks at ~1.2s each ≈ 7.2s) so a busy holder is reported only when
// genuinely wedged, never merely slow.
static constexpr int LOCK_BUDGET_MS        = 12000;
// Settle budget after re-pausing on restore — purely cosmetic; we do not block
// on it (the pause flag write is observed at the top of the engine's tick loop).

// One global single-flight lock serializes the whole record→pulse→drain→restore
// section (the bridge-side analogue of the gateway's _strict_lock). Guarantees
// only one coordinator owns the pause-pulse at a time so two concurrent deferred
// calls cannot interleave their record/restore and corrupt the recorded
// {paused,speed} (the "tempo drift" bug).
static std::timed_mutex g_deferred_drain_lock;

// Pulse refcount (§3.5): lets nested / queued callers that arrive while the lock
// holder is mid-pulse share one pulse window. With a single global lock only the
// holder ever touches this, but the refcount is kept so the last-out re-pauses
// even if a future change relaxes the lock to allow pulse sharing.
static std::atomic<int> g_pulse_depth{0};

// ── ck-p2sb: THE TWO TYPED REFUSALS ARE NOW COUNTED, AND SYMMETRICALLY ─────
// Both of this coordinator's refusals used to survive ONLY in the reply frame
// they were written into. Every queued response on this socket is BROADCAST and
// request_id is the only discriminator (ipc_server.cpp:1507), so the one
// surviving copy of a wedged-drain event travelled by exactly the mechanism
// ck-st70 and ck-i44k exist because of. A refusal nobody can count is a refusal
// whose ABSENCE cannot be read as evidence — "the drain never wedged this
// episode" was a sentence nobody could say.
//
// SYMMETRIC ON PURPOSE. The bead's evidence was the ASYMMETRY between these
// two: drain_unavailable wrote a log line with its reason and busy_deferred
// wrote nothing, 95 lines apart in one function family. Counting only
// busy_deferred would fix that asymmetry by minting a new one (log+count vs
// log only), so both are counted. Instrumentation only — no behavioural effect,
// the same shape as g_cb_query_busy_count (ai_eval_hook.cpp, H3) and as the
// carrier's flag-plus-count (ck-hzp7).
// DEFINED IN src/deferred_counters.cpp, NOT HERE. dispatch_router.cpp reports
// them and several host test targets compile dispatch_router.cpp against stubs;
// defining them in this TU stopped three of those linking, and adding this TU to
// their source lists drags in is_paused()/game_get_speed() — engine symbols — so
// the counters are a leaf TU instead. See that file, and the ck-1ek9 note beside
// src/pipeline_mode.cpp in bridge/CMakeLists.txt.

bool await_tick_advance(uint64_t baseline_tick, int budget_ms) {
    if (budget_ms < 0) budget_ms = 0;
    int waited = 0;
    // Fast path: maybe a tick already landed between unpause and this call.
    if (g_tick_id.load(std::memory_order_acquire) > baseline_tick) return true;
    while (waited < budget_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(PULSE_START_POLL_MS));
        waited += PULSE_START_POLL_MS;
        if (g_tick_id.load(std::memory_order_acquire) > baseline_tick) return true;
    }
    return false;
}

// Synthesize a typed status JSON into `out` for the non-Ok paths. Kept minimal
// and self-describing so any caller (agent, raw IPC, harness, suite) gets an
// actionable, non-hanging signal instead of a bare timeout.
static void emit_status(std::string* out, const char* status, const char* hint) {
    if (!out) return;
    std::string s;
    s.reserve(256);
    s += "{\"status\":\"";
    s += status;
    s += "\",\"deferred\":true,\"hint\":\"";
    s += hint;
    s += "\"}";
    *out = std::move(s);
}

DeferredDrainStatus run_deferred_pause_transparent(
    const std::function<void(std::string*)>& service,
    std::string* out) {

    // --- Single-flight acquire (bounded) -------------------------------------
    std::unique_lock<std::timed_mutex> lk(
        g_deferred_drain_lock, std::defer_lock);
    // ck-qnft: this try_lock_for blocks the SOLE serving thread for up to
    // LOCK_BUDGET_MS = 12000 against the driver's 15s deadline. Whether it was
    // held, by whom, and for how long is the first question to ask of a
    // timeout — and none of it was observable. The waiter count is incremented
    // BEFORE the wait so a watcher on another thread sees the queue while it is
    // forming, not after it drains.
    bridge::drain_lock_state().waiters.fetch_add(1, std::memory_order_relaxed);
    struct WaiterGuard {
        ~WaiterGuard() {
            bridge::drain_lock_state().waiters.fetch_sub(1, std::memory_order_relaxed);
        }
    } waiter_guard;
    if (!lk.try_lock_for(std::chrono::milliseconds(LOCK_BUDGET_MS))) {
        bridge::drain_lock_state().busy_refusals.fetch_add(1, std::memory_order_relaxed);
        emit_status(out, "busy_deferred",
            "Another deferred call held the drain lock past the budget. Retry shortly.");
        // ck-p2sb. The pattern is its sibling's, 95 lines down: a raw fprintf
        // with the reason written out. This is the ONE refusal in this file
        // that had neither a log nor a counter.
        g_busy_deferred_count.fetch_add(1, std::memory_order_relaxed);
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge] deferred-drain: lock held past %d ms budget "
            "(busy_deferred), count=%llu — a deferred call was REFUSED and did "
            "not run\n", LOCK_BUDGET_MS,
            (unsigned long long)g_busy_deferred_count.load(
                std::memory_order_relaxed));
        return DeferredDrainStatus::BusyDeferred;
    }

    // --- Record the live PAUSE STATE before any toggle (§3.6) ----------------
    // ck-qnft: the lock is now held by this thread. Recorded so a watcher can
    // name the holder while a second caller is still inside its 12s wait.
    bridge::drain_lock_state().holder_tid.store(bridge::inflight_tid(),
                                                std::memory_order_relaxed);
    bridge::drain_lock_state().since_ms.store(bridge::inflight_now_ms(),
                                              std::memory_order_relaxed);
    struct HolderGuard {
        ~HolderGuard() {
            bridge::drain_lock_state().holder_tid.store(0, std::memory_order_relaxed);
            bridge::drain_lock_state().since_ms.store(0, std::memory_order_relaxed);
        }
    } holder_guard;

    // The pause flag is the only thing this coordinator restores (ck-cyhr). The
    // speed is read BESIDE it as an OBSERVATION — it is logged at pulse close so
    // an engine-side or client-side tempo change during the window is visible in
    // the bridge log, and it is never written back. See PulseRestore below.
    const bool was_paused = is_paused();
    const int  speed_at_open = game_get_speed();  // internal 0-indexed; OBSERVED ONLY

    // Common hot path: already unpaused. Do NOT touch the clock — just run the
    // closure. The whole record/pulse/restore is a no-op here.
    if (!was_paused) {
        service(out);
        return DeferredDrainStatus::Ok;
    }

    // --- Pulse (record → unpause → wait-for-tick) ----------------------------
    // Precondition (§3.3 / risk #2): we ONLY pulse from the paused state, where
    // the game thread is parked in its idle loop — exactly the condition under
    // which game_unpause()'s synchronous PAUSE_SET is safe to issue from the IPC
    // thread. We never pulse an already-running game (guarded by was_paused).
    //
    // RAII restore (design risk #3): the re-pause/refcount-decrement is bound to
    // a scope-guard so it runs on EVERY exit path — success, drain-timeout,
    // frozen, and any C++ exception out of `service` — never leaving the game in
    // a lingering-unpaused state. (SIGSEGV inside the closure is already converted
    // to a normal return by its sigsetjmp guard; this also covers any throw.)
    //
    // THE PULSE RESTORES THE PAUSE FLAG ONLY. IT NEVER WRITES SPEED (ck-cyhr).
    // This guard used to also re-assert the speed recorded at pulse open:
    //
    //     if (game_get_speed() != prev_speed) game_set_speed(prev_speed + 1);
    //
    // guarded by a comment reading "re-assert defensively only if it drifted
    // (some other client legitimately changed speed during our window)". That
    // branch is UNREACHABLE for anything this coordinator did — the pulse toggles
    // the pause flag and nothing else (game_pause/game_unpause go through
    // PAUSE_SET; pause is a separate flag, not speed 0), so speed_at_open is
    // intact by construction. It is reachable ONLY when somebody ELSE moved the
    // speed inside the window, which the comment itself calls LEGITIMATE — and it
    // undid it anyway.
    //
    // That is how it killed E0. The pre-registered speed pin (display 2) landed
    // inside a window opened at display 3; on close the guard called it drift and
    // wrote 3 back, within ~8-15s of every pin. The meter lane pauses to read and
    // unpauses after, continuously, so the window that undoes the pin opened on
    // EVERY SAMPLE: the observer's own act of observing destroyed the condition
    // it then recorded as violated.
    //
    // An engine-side or client-side speed change during the window is an
    // OBSERVATION TO LOG, never a change to revert. A bridge that silently undoes
    // what something else did hides what that something else did. So the speed is
    // read at close, compared against the open reading, and the divergence is
    // written to the log — with no write to the game.
    //
    // Covered by bridge/test/pulse_restore_probe.cpp +
    // scripts/acceptance_pulse_restore.py (the first coverage this file has had).
    struct PulseRestore {
        int speed_at_open;  // OBSERVED at pulse open; never restored, only logged
        bool active = true;
        ~PulseRestore() {
            if (!active) return;
            // Last-out re-pause: only the final decrementer re-asserts the
            // recorded pause, so a burst of deferred calls riding one pulse
            // window cannot pause/unpause-thrash.
            if (g_pulse_depth.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                game_pause();  // deterministic "put it back exactly"; guarded byte write
                const int speed_at_close = game_get_speed();
                if (g_logfile) fprintf(g_logfile,
                    "[CK3-Bridge] deferred-drain: restored {paused=1} "
                    "(speed observed %d at open, %d at close; not touched)\n",
                    speed_at_open + 1, speed_at_close + 1);
                if (speed_at_close != speed_at_open && g_logfile) fprintf(g_logfile,
                    "[CK3-Bridge] deferred-drain: OBSERVED speed change during "
                    "pulse window: display %d -> %d. Recorded, NOT reverted "
                    "(ck-cyhr).\n",
                    speed_at_open + 1, speed_at_close + 1);
            }
        }
    };

    const uint64_t baseline_tick = g_tick_id.load(std::memory_order_acquire);
    g_pulse_depth.fetch_add(1, std::memory_order_relaxed);
    PulseRestore restorer{speed_at_open};
    game_unpause();  // fault-guarded inside event_handler.cpp

    if (!await_tick_advance(baseline_tick, PULSE_START_BUDGET_MS)) {
        // The unpause silently no-op'd: sim not live (idler null / load / menu /
        // transition). Do not run the closure (it would only time out); return a
        // typed drain_unavailable. RAII restorer still re-pauses. This is the
        // primary I15 frozen-thread guard — bounded, never a hang.
        emit_status(out, "drain_unavailable",
            "Game did not start ticking within the pulse budget "
            "(loading screen / between sessions). The sim is not live; "
            "this is not a pause error and not fixable by toggling pause.");
        g_drain_unavailable_count.fetch_add(1, std::memory_order_relaxed);  // ck-p2sb
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge] deferred-drain: pulse no-tick (drain_unavailable), "
            "baseline_tick=%llu count=%llu\n", (unsigned long long)baseline_tick,
            (unsigned long long)g_drain_unavailable_count.load(
                std::memory_order_relaxed));
        return DeferredDrainStatus::DrainUnavailable;
    }

    // The clock is live — run the existing submit+poll closure. Its own bounded
    // poll drains on the live tick(s); whatever it writes (success OR its own
    // graceful timeout) is the caller's result, passed through. The RAII
    // restorer re-pauses on return (or on any throw out of `service`).
    service(out);
    return DeferredDrainStatus::Ok;
}

// Actions whose real work runs on the game thread post-tick (serviced in
// drain_and_execute_intents). These are the calls that need the coordinator's
// pulse when invoked PAUSED. Kept in sync with the deferred classification in
// snapshot.cpp's is_bridge_internal_query (the inline-serviced deferred queries)
// plus the two formerly-unclassified read handlers the health matrix flagged as
// hanging (query_dejure_progress, query_conquest_targets).
bool is_deferred_drain_action(const char* action_type) {
    if (!action_type) return false;
    static const char* deferred[] = {
        // Bounded-poll deferred queries (already inline-serviced via g_cb_query /
        // g_acceptance_query / g_trigger_query / g_named_trigger_query /
        // g_decision_eval_query — they return a graceful timeout when paused; the
        // pulse turns that timeout into a real result).
        "query_valid_cbs",
        "query_cb_validity_strict",
        "query_cbs_for_target",
        // Batched CB enumeration: ONE coordinator pulse, N targets drained on the
        // shared single-flight g_cb_query slot inside that window (collapses N
        // pulses → 1; _slot_drain_profile.md §3b).
        "query_cbs_for_targets",
        "query_interaction_acceptance",
        "query_interaction",
        "query_trigger_result",
        "evaluate_named_trigger",
        "list_valid_decisions",
        "list_decisions_annotated",
        "probe_interaction_cascade",
        "probe_gate_4_slots",
        // query_conquest_targets HUNG when paused (P0 in the LiveVerify health
        // matrix): it was unclassified, so it sat in g_msg_queue and never drained
        // while paused. Its handler body walks the game graph (pure safe_read) AND
        // calls the bounded cb_query_strict_full_sync deferred probe (needs a tick
        // for the engine county_conquest verdict). Routed through the coordinator
        // so a paused call PULSES → the probe drains → real verdict → restore;
        // unpaused it keeps its proven race-free game-thread-queue path (the
        // ipc_server Tier-0 carve-out declines it inline when unpaused).
        //
        // NOTE query_dejure_progress is NOT here — it is a pure safe_read meter
        // with no game-thread sub-call, so it is fixed by classifying it as a
        // Tier-2 is_game_state_query (serve inline when paused, no pulse needed).
        "query_conquest_targets",
    };
    for (const char* d : deferred) {
        if (strcmp(action_type, d) == 0) return true;
    }
    return false;
}

} // namespace bridge
