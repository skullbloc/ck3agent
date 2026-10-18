// inflight_probe.cpp — CAN A SECOND THREAD DESCRIBE A STUCK ONE? (ck-qnft)
//
// All three arm-A attempts ended with BridgeTimeout on get_tick_info at 15s.
// Attempt 3 did it with CK3 alive and the game thread's cadence uniform, so the
// invariant failure is a TIMEOUT and ck-duv5's fault instrument is correctly
// silent on it. ck-qnft names the outstanding request instead.
//
// THE CLAIM UNDER TEST IS THE ONE THE WHOLE DESIGN RESTS ON: a thread OTHER than
// the serving thread can read the in-flight record and name the request WHILE
// the serving thread is blocked. If that does not hold, the instrument reports
// only after the stall clears — which is exactly too late, and is why the report
// lives on server_loop rather than in reader_loop.
//
// WHAT THIS DOES NOT COVER: ipc_server.cpp cannot link without the engine, so
// that the watcher is actually WIRED into server_loop, and that the scope wraps
// the whole dispatch, are checked by the acceptance script as READS.
#include "../include/inflight.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {
int g_failed = 0;
void row(const char* name, bool ok) {
    if (!ok) ++g_failed;
    std::printf("%s\t%d\n", name, ok ? 1 : 0);
}
}  // namespace

using namespace bridge;

int main() {
    std::printf("case\tvalue\n");
    char b[512];

    // ── idle ────────────────────────────────────────────────────────────────
    {
        const uint64_t now = inflight_now_ms();
        std::string out = format_inflight(b, sizeof b, inflight_read(now),
                                          drain_lock_state(), now);
        row("idle_says_IDLE", out.find("inflight=IDLE") != std::string::npos);
        row("idle_names_no_request", out.find("action=") == std::string::npos);
    }

    // ── THE ROW THAT MATTERS: a blocked thread, described by another ─────────
    // Thread A enters an InflightScope and STAYS there, exactly as reader_loop
    // does when it is stuck. Main reads it meanwhile. Nothing is simulated: two
    // real threads, real tids, a real overlap.
    {
        std::atomic<bool> release{false};
        std::atomic<int>  server_tid{0};
        std::thread stuck([&]{
            server_tid.store(inflight_tid());
            InflightScope scope("query_interaction_acceptance", "req-abc-123");
            while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        });
        // Let it get inside the scope.
        for (int i = 0; i < 200; ++i) {
            if (inflight_read(inflight_now_ms()).busy) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        const uint64_t t1 = inflight_now_ms();
        InflightView v1 = inflight_read(t1);
        row("a_blocked_thread_is_visible_to_another_thread", v1.stable && v1.busy);
        row("the_watcher_names_the_outstanding_action",
            std::string(v1.action) == "query_interaction_acceptance");
        row("the_watcher_names_the_request_id",
            std::string(v1.request_id) == "req-abc-123");
        row("the_watcher_names_the_OWNING_thread_not_its_own",
            v1.tid == server_tid.load() && v1.tid != inflight_tid());

        // AGE MUST GROW. A record that merely EXISTS cannot distinguish a slow
        // request from a wedged one — the whole point is to tell those apart, so
        // the age is asked to CHANGE rather than to be present.
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        InflightView v2 = inflight_read(inflight_now_ms());
        row("the_reported_age_GROWS_while_the_thread_stays_stuck",
            v2.stable && v2.busy && v2.age_ms > v1.age_ms);

        std::string out = format_inflight(b, sizeof b, v2, drain_lock_state(),
                                          inflight_now_ms());
        row("the_rendered_line_carries_the_action_and_an_age",
            out.find("action=query_interaction_acceptance") != std::string::npos &&
            out.find("age_ms=") != std::string::npos);

        release.store(true);
        stuck.join();
    }

    // ── and it clears ───────────────────────────────────────────────────────
    {
        const uint64_t now = inflight_now_ms();
        row("the_record_clears_when_the_request_finishes",
            !inflight_read(now).busy);
    }

    // ── a torn read REFUSES rather than naming a half-written request ───────
    // Forced by leaving the sequence odd, which is what a reader sees mid-write.
    // Same rule as ck-duv5's unresolved offset: a well-formed wrong answer is
    // worse than a refusal, and this one would name the wrong request.
    {
        inflight_slot().seq.fetch_add(1, std::memory_order_acq_rel);   // -> odd
        const uint64_t now = inflight_now_ms();
        InflightView v = inflight_read(now);
        row("a_torn_read_is_reported_UNSTABLE", !v.stable);
        std::string out = format_inflight(b, sizeof b, v, drain_lock_state(), now);
        row("an_unstable_read_names_no_action",
            out.find("inflight=UNSTABLE") != std::string::npos &&
            out.find("action=") == std::string::npos);
        inflight_slot().seq.fetch_add(1, std::memory_order_acq_rel);   // -> even
    }

    // ── the drain lock's own fields ─────────────────────────────────────────
    // These are the 12s-budget numbers: whether it was held, by whom, how long,
    // and how many callers were queued behind it.
    {
        DrainLockState& d = drain_lock_state();
        const uint64_t now = inflight_now_ms();
        std::string free_line = format_inflight(b, sizeof b, inflight_read(now), d, now);
        row("an_unheld_drain_lock_reports_holder_0",
            free_line.find("drain_lock_holder_tid=0") != std::string::npos);

        d.holder_tid.store(4242);
        d.since_ms.store(now - 3000);
        d.waiters.store(2);
        std::string held_line = format_inflight(b, sizeof b, inflight_read(now + 3000), d,
                                                now + 3000);
        row("a_held_drain_lock_names_its_holder",
            held_line.find("drain_lock_holder_tid=4242") != std::string::npos);
        row("a_held_drain_lock_reports_how_long_it_has_been_held",
            held_line.find("drain_lock_held_ms=6000") != std::string::npos);
        row("queued_callers_are_counted",
            held_line.find("drain_waiters=2") != std::string::npos);
        d.holder_tid.store(0); d.since_ms.store(0); d.waiters.store(0);
    }

    std::printf("\n%s: %d row(s) failed\n", g_failed ? "FAIL" : "PASS", g_failed);
    return g_failed ? 1 : 0;
}
