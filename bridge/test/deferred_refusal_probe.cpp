// deferred_refusal_probe.cpp — DOES A REFUSED DEFERRED CALL LEAVE A TRACE? (ck-p2sb)
//
// Drives the REAL bridge/src/deferred_drain.cpp through its busy_deferred path,
// so what is measured is the shipping coordinator rather than a model of it.
//
// HOW THE REFUSAL IS REACHED WITH NO GAME. run_deferred_pause_transparent's very
// first act is a bounded try_lock_for on its single-flight lock; the refusal is
// returned BEFORE any engine call. So: one thread takes the lock and holds it in
// its service closure past LOCK_BUDGET_MS, a second thread calls in and is
// refused. With is_paused() stubbed false the holder takes the documented hot
// path — `service(out); return Ok;` — and touches nothing else in the engine.
//
// Built TWICE by scripts/acceptance_deferred_refusal.py: against the shipping
// coordinator and against the PRE-FIX blob. The pre-fix build has no counter
// symbols at all, which is the defect stated as an absence, so the counters are
// read only under CK3_DEFERRED_HAS_COUNTERS. THE LOG IS READ IN BOTH — that is
// the differential the lane turns on, because it is a surface both builds have
// and only one of them writes to.
//
// Usage:  deferred_refusal_probe <logfile-path>
// Prints ONE json object.

#include "bridge.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

namespace bridge {
FILE* g_logfile = nullptr;
uintptr_t             g_base_addr = 0;
std::vector<MemRange> g_valid_ranges;
std::shared_mutex     g_ranges_mutex;
std::atomic<uint64_t> g_tick_id{0};

// The engine surface this coordinator touches. is_paused() returns FALSE so the
// lock holder takes the already-unpaused hot path and never reaches the pulse —
// the refusal under test happens strictly before any of this.
bool is_paused() { return false; }
int  game_get_speed() { return 3; }
void game_unpause() {}
void game_pause() {}
void game_set_speed(int) {}
}  // namespace bridge

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: probe <logfile>\n"); return 2; }
    bridge::g_logfile = fopen(argv[1], "w");

    std::string out_holder, out_refused;
    bridge::DeferredDrainStatus st_holder{}, st_refused{};

    // THE HOLDER: occupies the single-flight lock past the lock budget.
    std::thread holder([&] {
        st_holder = bridge::run_deferred_pause_transparent(
            [](std::string* o) {
                std::this_thread::sleep_for(std::chrono::milliseconds(13500));
                if (o) *o = R"({"status":"ok","probe":"holder"})";
            },
            &out_holder);
    });

    // Let the holder win the lock, then call in and be refused.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    st_refused = bridge::run_deferred_pause_transparent(
        [](std::string* o) { if (o) *o = R"({"status":"ok","probe":"SHOULD NOT RUN"})"; },
        &out_refused);

    holder.join();
    if (bridge::g_logfile) { fflush(bridge::g_logfile); fclose(bridge::g_logfile); }

    printf("{\"refused_status\":%d,\"holder_status\":%d,\"refused_reply\":%s",
           static_cast<int>(st_refused), static_cast<int>(st_holder),
           out_refused.empty() ? "null" : out_refused.c_str());
#ifdef CK3_DEFERRED_HAS_COUNTERS
    printf(",\"counter_surface\":true,\"busy_deferred_count\":%llu"
           ",\"drain_unavailable_count\":%llu",
           (unsigned long long)bridge::g_busy_deferred_count.load(),
           (unsigned long long)bridge::g_drain_unavailable_count.load());
#else
    // THE PRE-FIX STATE, and it is the finding rather than a build artefact:
    // there was no counter to read. The refusal existed only in the reply.
    printf(",\"counter_surface\":false");
#endif
    printf("}\n");
    return 0;
}
