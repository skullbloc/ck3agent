// deferred_drain.cpp — the deferred-slot single-flight lock.
//
// A DEFERRED call (CB validity, acceptance, trigger eval, decision eval, the
// faction verdicts, the interaction cascade) submits one of the game-thread
// query slots and polls for its verdict. L13 D1: the handler itself runs on the
// game thread (is_game_state_query) and services its own slot inline there;
// query_interaction_check polls from the IPC thread while the drain services its
// check intent.
//
// L13 D1/D3: that drain runs after every session pass, paused or running, so
// a deferred call no longer needs the clock to run. The pause "pulse" this file
// used to open around a paused call (record the pause flag, unpause until a
// tick ran, re-pause) is deleted with that need, and with it the
// drain_unavailable refusal it answered when no tick came (ck-p2sb) and the
// pulse acceptance (ck-cyhr). What stays is what was never about the clock:
// ONE single-flight lock around the service, so two callers cannot interleave
// on one slot; the typed busy_deferred refusal when it is held past its budget;
// and the ck-qnft holder/waiter visibility the watchdog reads.
//
// Bounded throughout (no signal watchdog can land on the SIGALRM-masked reader
// thread — ipc_server.cpp reader_loop — so the bound is a POLLED budget, never
// a signal timeout: the I15 lesson).

#include "inflight.h"
#include "bridge.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>

namespace bridge {

// Lock budget: single-flight wait before returning busy_deferred rather than
// blocking a new caller behind a wedged holder. Sized to comfortably exceed the
// heaviest legitimate service window (a batched CB sweep over several targets
// at up to ~1.2 s per engine check) so a busy holder is reported only when
// genuinely wedged, never merely slow.
static constexpr int LOCK_BUDGET_MS = 12000;

// One global single-flight lock serializes the deferred services (the
// bridge-side analogue of the gateway's _strict_lock).
static std::timed_mutex g_deferred_drain_lock;

// ── ck-p2sb: THE TYPED REFUSAL IS COUNTED ──────────────────────────────────
// busy_deferred used to survive ONLY in the reply frame it was written into.
// Every queued response on this socket is BROADCAST and request_id is the only
// discriminator, so the one surviving copy of a wedged-lock event travelled by
// exactly the mechanism ck-st70 and ck-i44k exist because of. A refusal nobody
// can count is a refusal whose ABSENCE cannot be read as evidence.
// DEFINED IN src/deferred_counters.cpp, NOT HERE: dispatch_router.cpp reports it
// and several host test targets compile dispatch_router.cpp against stubs, so
// the counter is a leaf TU (see that file, and the ck-1ek9 note beside
// src/pipeline_mode.cpp in bridge/CMakeLists.txt).

// Synthesize a typed status JSON into `out` for the refusal. Kept minimal and
// self-describing so any caller gets an actionable signal, not a bare timeout.
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

DeferredDrainStatus run_deferred_single_flight(
    const std::function<void(std::string*)>& service,
    std::string* out) {

    // --- Single-flight acquire (bounded) -------------------------------------
    std::unique_lock<std::timed_mutex> lk(
        g_deferred_drain_lock, std::defer_lock);
    // ck-qnft: this try_lock_for blocks the SOLE serving thread for up to
    // LOCK_BUDGET_MS = 12000 against the driver's 15s deadline. Whether it was
    // held, by whom, and for how long is the first question to ask of a
    // timeout. The waiter count is incremented BEFORE the wait so a watcher on
    // another thread sees the queue while it is forming, not after it drains.
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
        g_busy_deferred_count.fetch_add(1, std::memory_order_relaxed);
        if (g_logfile) fprintf(g_logfile,
            "[CK3-Bridge] deferred-drain: lock held past %d ms budget "
            "(busy_deferred), count=%llu — a deferred call was REFUSED and did "
            "not run\n", LOCK_BUDGET_MS,
            (unsigned long long)g_busy_deferred_count.load(
                std::memory_order_relaxed));
        return DeferredDrainStatus::BusyDeferred;
    }

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

    // The service's own bounded poll sees the drain service its slot on the
    // next session pass; whatever it writes (success OR its own graceful
    // timeout) is the caller's result, passed through.
    service(out);
    return DeferredDrainStatus::Ok;
}

// The deferred-slot submitters: handlers that submit a game-thread query slot
// and poll for its verdict. L13 D1: all but query_interaction_check are
// game-thread reads (is_game_state_query) that service their own slot inline
// there (service_slots_if_game_thread); query_interaction_check enqueues a check
// intent and polls from the IPC thread (is_bridge_internal_query). Every one is
// dispatched under the single-flight lock above (ipc_server.cpp). (query_conquest_targets
// left this set in L13: it calls the strict CB evaluation directly on the game thread.)
bool is_deferred_drain_action(const char* action_type) {
    if (!action_type) return false;
    static const char* deferred[] = {
        // Bounded-poll deferred queries (via g_cb_query / g_acceptance_query /
        // g_trigger_query / g_decision_eval_query).
        "query_valid_cbs",
        "query_cb_validity_strict",
        "query_cbs_for_target",
        // Batched CB enumeration: ONE hold of the lock, N targets drained on the
        // shared single-flight g_cb_query slot (_slot_drain_profile.md §3b).
        "query_cbs_for_targets",
        "query_interaction_acceptance",
        "query_interaction",
        "query_trigger_result",
        "list_valid_decisions",
        "list_decisions_annotated",
        "query_joinable_factions",      // F16: engine faction verdicts (factions.cpp)
        "query_interaction_check",            // F16: the send pipeline's checks, drained, nothing sent
        "probe_interaction_cascade",
        "probe_gate_4_slots",
    };
    for (const char* d : deferred) {
        if (strcmp(action_type, d) == 0) return true;
    }
    return false;
}

} // namespace bridge
