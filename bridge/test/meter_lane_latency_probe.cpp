// meter_lane_latency_probe.cpp — ck-fvms.1.
//
// WHAT DOES THE OBSERVER'S READ ACTUALLY WAIT FOR, and why does the same read
// cost 0.10 s on E0 and 10.13 s on an arm?
//
// ck-zgh4 measured the arm's blocked meter reads: 90 events, median 10.02 s, 54
// of them within 0.13 s of exactly 10.00 s, against pipeline.flush_period_ms =
// 10000. That match names the flush period as the cost. It does NOT explain two
// things the same measurement set contains:
//
//   1. WHY A FULL PERIOD RATHER THAN HALF OF ONE. Reads arriving at random
//      inside a 10 s cadence should wait uniformly 0-10 s and average 5. The
//      observed distribution is a SPIKE at the full period.
//   2. WHY E0 DOES NOT PAY IT. E0's max poll step is ~4 game-days; the arm's is
//      173-347. ck-zgh4 excluded _record_ownership_sample's extra reads by
//      measurement, so the difference is not the observer doing more work.
//
// THE HYPOTHESIS THIS PROBE DISCRIMINATES. Early-flush condition (b) —
// flush_scheduler.cpp:261 — fires when EVERY REGISTERED SESSION has submitted
// since the last flush, and ipc_server.cpp:602 registers EVERY live session,
// agents included. With the observer alone registered, its own submission
// satisfies (b) and the flush is immediate. Add one agent that speaks on a ~120
// s LLM turn and (b) stops firing between the observer's polls, so every read
// falls through to the cadence.
//
// And then the loop SELF-SYNCHRONISES, which is the part that predicts a spike
// rather than a spread: the read is served AT a flush, which resets the period;
// the driver's loop immediately issues the next read, so it arrives at the START
// of a fresh period and waits a very nearly full one. The first read pays a
// partial wait and every read after it pays ~period.
//
// PURE. Wall time is passed in, no game, no bridge, no sockets. The scheduler is
// the shipping source (src/flush_scheduler.cpp), not a model of it.
#include "flush_scheduler.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace bridge;

namespace {

int g_checks = 0, g_fails = 0;

void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) { ++g_fails; std::printf("  FAIL  %s\n", what.c_str()); }
    else       std::printf("  PASS  %s\n", what.c_str());
}

bool submit(FlushScheduler& s, const char* sess, Polarity p, uint64_t now_ms) {
    CallRequest r;
    r.session_id = sess;
    r.action = "query_dejure_progress";
    r.polarity = p;
    return s.submit(r, now_ms).accepted;
}

// One observer poll: issue the read at `now`, then advance a millisecond at a
// time until the scheduler says a flush is due, and flush. Returns the wait.
//
// `agent_period_ms` = 0 means no agent is attached at all (the E0 shape).
// Otherwise a second registered session speaks once every that many ms, which is
// the arm shape: present, registered, and much slower than the flush period.
uint64_t one_poll(FlushScheduler& s, uint64_t& now, uint64_t agent_period_ms,
                  uint64_t& next_agent_ms, uint64_t cap_ms) {
    const uint64_t issued = now;
    submit(s, "observer", Polarity::PAUSED, now);
    for (uint64_t waited = 0; waited <= cap_ms; ++waited) {
        if (agent_period_ms && now >= next_agent_ms) {
            submit(s, "agent", Polarity::TICK_WRITE, now);
            next_agent_ms += agent_period_ms;
        }
        if (s.due(now) != FlushTrigger::None) {
            s.tick(now, ClockReading{false, 3});
            return now - issued;
        }
        ++now;
    }
    return cap_ms + 1;                       // never served inside the cap
}

std::vector<uint64_t> run_shape(uint64_t agent_period_ms, int polls) {
    FlushConfig cfg;                          // period_ms = 10000, as measured live
    FlushScheduler s(cfg);
    s.register_session("observer");
    if (agent_period_ms) s.register_session("agent");

    std::vector<uint64_t> waits;
    uint64_t now = 0, next_agent = agent_period_ms;
    for (int i = 0; i < polls; ++i)
        waits.push_back(one_poll(s, now, agent_period_ms, next_agent, 60000));
    return waits;
}

void report(const char* label, const std::vector<uint64_t>& w) {
    uint64_t total = 0, mx = 0;
    for (uint64_t v : w) { total += v; if (v > mx) mx = v; }
    std::printf("  %-34s n=%2zu  total=%6llu ms  mean=%6llu ms  max=%6llu ms\n",
                label, w.size(), (unsigned long long)total,
                (unsigned long long)(total / (w.empty() ? 1 : w.size())),
                (unsigned long long)mx);
    std::printf("      first 8 waits (ms):");
    for (size_t i = 0; i < w.size() && i < 8; ++i)
        std::printf(" %llu", (unsigned long long)w[i]);
    std::printf("\n");
}

}  // namespace

int main() {
    std::printf("\nck-fvms.1 — what the observer's read waits for\n");
    std::printf("period_ms = 10000 (the live pipeline.flush_period_ms)\n\n");

    std::printf("A  E0 SHAPE — the observer is the only registered session\n");
    const std::vector<uint64_t> e0 = run_shape(0, 20);
    report("observer alone", e0);
    bool e0_immediate = true;
    for (uint64_t v : e0) if (v != 0) e0_immediate = false;
    check(e0_immediate,
          "every read is served IMMEDIATELY — its own submission satisfies (b)");

    std::printf("\nB  ARM SHAPE — one agent also registered, speaking every 120 s\n");
    const std::vector<uint64_t> arm = run_shape(120000, 20);
    report("observer + 1 agent @120s", arm);

    size_t full_period = 0;
    for (size_t i = 1; i < arm.size(); ++i) if (arm[i] >= 9900) ++full_period;
    check(full_period >= arm.size() - 2,
          "after the first, EVERY read waits a very nearly FULL period — a "
          "spike, not a 0-10 s spread");

    uint64_t arm_total = 0, e0_total = 0;
    for (uint64_t v : arm) arm_total += v;
    for (uint64_t v : e0) e0_total += v;
    check(arm_total >= 100 * (e0_total + 1),
          "the arm shape costs orders of magnitude more than the E0 shape for "
          "IDENTICAL observer work");

    std::printf("\nC  IS IT THE AGENT'S SILENCE, OR MERELY ITS PRESENCE?\n");
    // An agent that speaks FASTER than the flush period restores (b). If the
    // cost tracked "an agent is attached" the wait would not move; if it tracks
    // "somebody registered has not spoken since the last flush" it collapses.
    const std::vector<uint64_t> chatty = run_shape(50, 20);
    report("observer + 1 agent @50ms", chatty);
    uint64_t chatty_total = 0;
    for (uint64_t v : chatty) chatty_total += v;
    check(chatty_total * 10 < arm_total,
          "a FAST-speaking agent collapses the wait — so the cost is the "
          "SILENCE between turns, not the agent's presence");

    std::printf("\nD  THE CONTROL — is the cadence doing the waiting at all?\n");
    // Same arm shape against a 1-second period. If the wait is the cadence, it
    // scales with the period; if it were a timeout somewhere else, it would not.
    FlushConfig fast;
    fast.period_ms = 1000;
    FlushScheduler s(fast);
    s.register_session("observer");
    s.register_session("agent");
    uint64_t now = 0, next_agent = 120000;
    std::vector<uint64_t> shortp;
    for (int i = 0; i < 10; ++i)
        shortp.push_back(one_poll(s, now, 120000, next_agent, 60000));
    report("same, period_ms = 1000", shortp);
    bool scales = true;
    for (size_t i = 1; i < shortp.size(); ++i)
        if (shortp[i] < 900 || shortp[i] > 1000) scales = false;
    check(scales,
          "the wait TRACKS the period exactly — it is the cadence, and not a "
          "timeout that would have stayed at 10 s");

    std::printf("\nE  THE COST IS PER-READ AND THE SAMPLE ISSUES MANY\n");
    // ck-fvms measured a SAMPLE at 6-28 bridge calls and a non-sample poll at
    // exactly one. ck-zgh4 measured 90 blocked calls totalling 710 s. If each
    // read pays a full period SERIALLY, a sample's cost is (reads x period) and
    // not (period) — which is the arithmetic that turns a 10 s cadence into the
    // 100-200 s stalls actually observed, and thence into 173-347 game-day poll
    // steps. Both shapes below do the SAME WORK; they differ only in whether the
    // reads are outstanding together.
    {
        const int READS = 14;                 // mid-range of the measured 6-28

        // SERIAL — what the driver does now: call, block, call, block. One read
        // is outstanding at a time, so each one meets a fresh period.
        FlushConfig cfg;
        FlushScheduler ser(cfg);
        ser.register_session("observer");
        ser.register_session("agent");
        uint64_t now = 0, next_agent = 120000, serial_total = 0;
        for (int i = 0; i < READS; ++i)
            serial_total += one_poll(ser, now, 120000, next_agent, 60000);

        // BATCHED — every read of the sample submitted before waiting. The
        // scheduler already serves the whole buffer in one flush; nothing about
        // the pipeline has to change for this, only the client's issue order.
        FlushScheduler bat(cfg);
        bat.register_session("observer");
        bat.register_session("agent");
        uint64_t bnow = 0;
        for (int i = 0; i < READS; ++i) submit(bat, "observer", Polarity::PAUSED, bnow);
        uint64_t batched_total = 0;
        while (bat.due(bnow) == FlushTrigger::None && batched_total <= 60000) {
            ++bnow; ++batched_total;
        }
        const FlushPlan p = bat.tick(bnow, ClockReading{false, 3});

        std::printf("      %2d reads SERIAL   %6llu ms\n", READS,
                    (unsigned long long)serial_total);
        std::printf("      %2d reads BATCHED  %6llu ms   (%zu served in one flush)\n",
                    READS, (unsigned long long)batched_total, p.selected_count());

        check(serial_total == (uint64_t)READS * cfg.period_ms,
              "SERIAL costs reads x period — the sample, not the poll, is what "
              "stalls");
        check(batched_total <= cfg.period_ms,
              "BATCHED costs ONE period for a batch WITHIN THE BOUND");
        check(p.selected_count() == (size_t)READS,
              "and the scheduler already serves them all in that one flush — "
              "the change is the CLIENT's issue order, not the pipeline's");

        // AND THE BOUND, because "one flush however deep" is FALSE and I had
        // written it (ck-760i's correction). A flush serves at most
        // max_paused_calls_per_flush and DEFERS the remainder; one socket may
        // not even buffer more than per_session_capacity. 41 counties gave 44
        // ids and fit; e_britannia's de-jure tree is 93 counties, which does
        // not. The lane chunks for exactly this reason.
        FlushScheduler big(cfg);
        big.register_session("observer");
        big.register_session("agent");
        const size_t OVER = cfg.per_session_capacity + 20;
        size_t accepted = 0, refused = 0;
        for (size_t i = 0; i < OVER; ++i)
            (submit(big, "observer", Polarity::PAUSED, 0) ? accepted : refused)++;
        const FlushPlan bp = big.plan_flush(1, ClockReading{false, 3},
                                            FlushTrigger::SizeBound);
        std::printf("      %zu submitted from ONE session -> %zu accepted, "
                    "%zu REFUSED buffer_full; flush served %zu, deferred %zu\n",
                    OVER, accepted, refused, bp.selected_count(),
                    bp.deferred.size());
        // I EXPECTED DEFERRAL AND GOT REFUSAL, which is worse and is the reason
        // the lane chunks. per_session_capacity bounds ONE SOCKET's buffered
        // calls, and it is checked at SUBMIT: past it, `submit` returns
        // accepted:false reason:"buffer_full" and the call never enters the
        // buffer at all. So an over-bound batch does not cost an extra period —
        // its tail is never answered, and a client waiting on those request_ids
        // waits out its timeout before the serial path picks them up. Slower
        // than never having batched.
        check(refused == 20 && accepted == cfg.per_session_capacity,
              "past per_session_capacity a submit is REFUSED, not queued");
        check(bp.selected_count() <= cfg.max_paused_calls_per_flush,
              "and a flush serves a BOUNDED batch, never an unbounded buffer");
    }

    std::printf("\n%d checks, %d failure(s)\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}
