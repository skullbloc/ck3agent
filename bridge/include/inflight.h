#pragma once
// ck-qnft: WHICH REQUEST WAS OUTSTANDING WHEN THE CLIENT GAVE UP.
//
// All three arm-A attempts died the same way — BridgeTimeout on get_tick_info
// after 15s — and attempt 3 did it with CK3 alive, the bridge still logging, and
// the game thread's tick cadence uniform to the end. So the invariant failure is
// a TIMEOUT, not a crash, and ck-duv5's fault instrument is correctly silent on
// it. Nothing in the bridge currently says what the serving thread was doing.
//
// THE DESIGN CONSTRAINT THAT SHAPES ALL OF THIS: THE OBSERVER MUST NOT BE THE
// OBSERVED. reader_loop is a single thread serving every client serially, so
// when it is occupied it cannot answer a query — which means an ACTION that
// reports in-flight state is useless at exactly the moment it is needed. The
// report has to be a LOG written by a different thread. server_loop polls on a
// 1s timeout and is not blocked by reader_loop, so it is the observer.
//
// (who_holds_the_lock, despite the name, reports the client CONNECTION — fd,
// peer pid, age. It says nothing about the serving thread or the drain lock, so
// there was nothing here to reuse.)
//
// READING IT WITHOUT A LOCK. The record is written by the serving thread and
// read by the watcher while the writer may be mid-update, so it uses a seqlock:
// odd sequence means a write is in progress and the reader retries or reports
// the state as in-flux. A torn action name presented as fact is the same defect
// class as ck-duv5's plausible 0x0 — it would name the wrong request with
// perfect confidence.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <ctime>
#include <unistd.h>
#include <sys/syscall.h>

namespace bridge {

inline uint64_t inflight_now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ull + ts.tv_nsec / 1000000ull;
}

inline constexpr size_t kInflightNameMax = 64;

struct InflightSlot {
    std::atomic<uint64_t> seq{0};          // even = stable, odd = being written
    std::atomic<uint64_t> start_ms{0};     // 0 = idle
    std::atomic<int>      tid{0};
    char action[kInflightNameMax]     = {0};
    char request_id[kInflightNameMax] = {0};
};

inline InflightSlot& inflight_slot() {
    static InflightSlot s;
    return s;
}

// ── the deferred drain lock's own state ────────────────────────────────────
// LOCK_BUDGET_MS is 12000 and the driver's deadline is 15000. A call that finds
// this lock held blocks the SOLE serving thread for up to twelve of the
// client's fifteen seconds, so whether it was held — and for how long, and by
// whom — is the first question to ask of a timeout.
struct DrainLockState {
    std::atomic<int>      holder_tid{0};     // 0 = not held
    std::atomic<uint64_t> since_ms{0};
    std::atomic<int>      waiters{0};
    std::atomic<uint64_t> busy_refusals{0};  // times the budget was exhausted
};

inline DrainLockState& drain_lock_state() {
    static DrainLockState s;
    return s;
}

inline int inflight_tid() { return static_cast<int>(syscall(SYS_gettid)); }

inline void inflight_begin(const char* action, const char* request_id) {
    InflightSlot& s = inflight_slot();
    s.seq.fetch_add(1, std::memory_order_acq_rel);          // -> odd
    std::snprintf(s.action, sizeof s.action, "%s", action ? action : "?");
    std::snprintf(s.request_id, sizeof s.request_id, "%s", request_id ? request_id : "?");
    s.tid.store(inflight_tid(), std::memory_order_relaxed);
    s.start_ms.store(inflight_now_ms(), std::memory_order_relaxed);
    s.seq.fetch_add(1, std::memory_order_acq_rel);          // -> even
}

inline void inflight_end() {
    InflightSlot& s = inflight_slot();
    s.seq.fetch_add(1, std::memory_order_acq_rel);
    s.start_ms.store(0, std::memory_order_relaxed);         // 0 = idle
    s.seq.fetch_add(1, std::memory_order_acq_rel);
}

struct InflightScope {
    InflightScope(const char* a, const char* r) { inflight_begin(a, r); }
    ~InflightScope()                            { inflight_end(); }
    InflightScope(const InflightScope&)            = delete;
    InflightScope& operator=(const InflightScope&) = delete;
};

// A consistent snapshot, or `stable=false` if the writer was mid-update through
// every attempt. Never returns a half-written name as though it were fact.
struct InflightView {
    bool     stable   = false;
    bool     busy     = false;     // a request is in flight
    uint64_t age_ms   = 0;
    int      tid      = 0;
    char     action[kInflightNameMax]     = {0};
    char     request_id[kInflightNameMax] = {0};
};

inline InflightView inflight_read(uint64_t now_ms) {
    InflightSlot& s = inflight_slot();
    InflightView v;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint64_t a = s.seq.load(std::memory_order_acquire);
        if (a & 1ull) continue;                       // writer in progress
        const uint64_t start = s.start_ms.load(std::memory_order_relaxed);
        const int      t     = s.tid.load(std::memory_order_relaxed);
        char act[kInflightNameMax], rid[kInflightNameMax];
        std::memcpy(act, s.action, sizeof act);
        std::memcpy(rid, s.request_id, sizeof rid);
        const uint64_t b = s.seq.load(std::memory_order_acquire);
        if (a != b) continue;                         // changed under us
        v.stable = true;
        v.busy   = (start != 0);
        v.age_ms = (start != 0 && now_ms >= start) ? (now_ms - start) : 0;
        v.tid    = t;
        act[sizeof act - 1] = '\0';
        rid[sizeof rid - 1] = '\0';
        std::snprintf(v.action, sizeof v.action, "%s", act);
        std::snprintf(v.request_id, sizeof v.request_id, "%s", rid);
        return v;
    }
    return v;   // stable = false
}

// ONE SPELLING, shared by the watcher and by the probe that grades it. An
// unstable read says so rather than presenting a possibly-torn name, for the
// same reason ck-duv5's unresolved offset does not print 0x0: a well-formed
// wrong answer is worse than a refusal, and this one would name a request.
inline const char* format_inflight(char* buf, size_t n, const InflightView& v,
                                   const DrainLockState& d, uint64_t now_ms) {
    const int      holder = d.holder_tid.load(std::memory_order_relaxed);
    const uint64_t since  = d.since_ms.load(std::memory_order_relaxed);
    const uint64_t held   = (holder && now_ms >= since) ? (now_ms - since) : 0;

    if (!v.stable) {
        std::snprintf(buf, n,
                      "inflight=UNSTABLE (the record was mid-update on every "
                      "attempt; no request is named rather than naming a torn "
                      "one) drain_lock_holder_tid=%d drain_lock_held_ms=%llu "
                      "drain_waiters=%d",
                      holder, (unsigned long long)held,
                      d.waiters.load(std::memory_order_relaxed));
        return buf;
    }
    if (!v.busy) {
        std::snprintf(buf, n,
                      "inflight=IDLE (the serving thread holds no request) "
                      "drain_lock_holder_tid=%d drain_lock_held_ms=%llu "
                      "drain_waiters=%d",
                      holder, (unsigned long long)held,
                      d.waiters.load(std::memory_order_relaxed));
        return buf;
    }
    std::snprintf(buf, n,
                  "inflight action=%s request_id=%s tid=%d age_ms=%llu "
                  "drain_lock_holder_tid=%d drain_lock_held_ms=%llu "
                  "drain_waiters=%d busy_refusals=%llu",
                  v.action, v.request_id, v.tid,
                  (unsigned long long)v.age_ms, holder,
                  (unsigned long long)held,
                  d.waiters.load(std::memory_order_relaxed),
                  (unsigned long long)d.busy_refusals.load(std::memory_order_relaxed));
    return buf;
}

}  // namespace bridge
