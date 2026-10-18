// test/test_pipeline.cpp — unit tests for the R1b apply + ticket layer (ck-6kvq.9).
//
// Standalone: compiles flush_scheduler.cpp + ticket_table.cpp + flush_apply.cpp
// with NO engine, NO game and NO CK3 binary. The plan is applied against a FAKE
// FlushEffects, which is precisely what the ck-6kvq.8 scheduler/apply split was
// for — the wiring can be asserted on a host.
//
//   cmake -DBUILD_BRIDGE_TESTS=ON .. && make -j$(nproc) test_pipeline
//   ./test_pipeline
//
// What is asserted maps 1:1 onto the bead's OFFLINE acceptance list:
//   - a ticket's lifecycle across states
//   - `check` on an unknown ticket is a TYPED refusal, not silence
//   - a legacy request_id-keyed client still gets its result
//   - ANY-polarity calls bypass the buffer entirely (never a clock effect)

#include "flush_apply.h"
#include "flush_scheduler.h"
#include "ticket_table.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

using namespace bridge;

// ---------------------------------------------------------------------------
// Assertion harness (independent of NDEBUG).
// ---------------------------------------------------------------------------
static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond)                                                           \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) {                                                        \
            ++g_failures;                                                     \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                     \
    } while (0)

#define CHECK_EQ(a, b)                                                        \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!((a) == (b))) {                                                  \
            ++g_failures;                                                     \
            std::printf("  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); \
        }                                                                     \
    } while (0)

static void section(const char* name) { std::printf("[ %s ]\n", name); }

// ---------------------------------------------------------------------------
// FakeEffects — a recording, scriptable stand-in for the engine.
// ---------------------------------------------------------------------------
struct EmittedFrame {
    uint64_t    ticket;
    std::string action;
    std::string result;
};

class FakeEffects : public FlushEffects {
public:
    // --- scripted behaviour ------------------------------------------------
    std::vector<std::string> drain_actions;   // these route to the game-thread queue
    bool  enqueue_succeeds = true;
    bool  serve_returns_empty = false;
    uint64_t clock_ms = 0;

    // --- recorded facts ----------------------------------------------------
    ClockReading clock{false, 3};
    int pauses = 0, unpauses = 0, speed_sets = 0;
    std::vector<std::string> served;        // actions served inline, in order
    std::vector<std::string> enqueued;      // actions handed to the drain, in order
    std::vector<EmittedFrame> emitted;      // result frames, keyed by ticket
    int drift_notes = 0;
    int flush_records = 0;

    bool pause() override {
        CHECK(!clock.paused);               // never pause an already-paused clock
        clock.paused = true; ++pauses; return true;
    }
    bool unpause() override { clock.paused = false; ++unpauses; return true; }
    bool set_speed(int s) override { clock.speed = s; ++speed_sets; return true; }

    uint64_t now_ms() const override { return clock_ms; }

    bool routes_to_drain(const BufferedCall& c) const override {
        for (const std::string& a : drain_actions) if (a == c.action) return true;
        return false;
    }

    std::string serve(const BufferedCall& c) override {
        served.push_back(c.action);
        if (serve_returns_empty) return std::string();
        return std::string("{\"success\":true,\"action\":\"") + c.action + "\"}";
    }

    bool enqueue_for_drain(const BufferedCall& c) override {
        if (!enqueue_succeeds) return false;
        enqueued.push_back(c.action);
        return true;
    }

    ExecutionStamp stamp(const BufferedCall&) override {
        ExecutionStamp st;
        st.character_id  = 14859;
        st.tick_executed = 4242;
        st.game_date     = "1066.09.15";
        return st;
    }

    void emit_result(const BufferedCall& c, const std::string& r) override {
        emitted.push_back(EmittedFrame{c.ticket, c.action, r});
    }

    void note_clock_drift(const FlushPlan&) override { ++drift_notes; }
    void note_flush_record(const FlushPlan&, uint64_t, uint64_t) override { ++flush_records; }
};

// A submit helper that keeps the scheduler and the ticket table in step exactly
// the way pipeline.cpp's ingress does.
static uint64_t submit(FlushScheduler& s, TicketTable& t, const char* session,
                       const char* action, Polarity pol, const char* request_id,
                       uint64_t now = 0) {
    CallRequest req{session, action, pol};
    SubmitResult sr = s.submit(req, now);
    if (!sr.accepted) return 0;
    t.open(sr.ticket, session, request_id ? request_id : "", action, sr.polarity, now);
    return sr.ticket;
}

// ===========================================================================
static void test_ticket_lifecycle() {
    section("ticket lifecycle: pending -> running -> done, with attribution");

    FlushScheduler s;
    TicketTable t;
    FakeEffects fx;

    const uint64_t tk = submit(s, t, "s1", "query_realm", Polarity::PAUSED, "rid-1");
    CHECK(tk != 0);

    Ticket rec;
    CHECK(t.find(tk, &rec));
    CHECK(rec.state == TicketState::Pending);
    CHECK_EQ(rec.request_id, std::string("rid-1"));
    // Nothing is stamped before execution — the attribution record is minted by
    // the bridge at execution time, never accepted from the agent.
    CHECK_EQ(rec.character_id, 0u);
    CHECK(rec.game_date_executed.empty());

    FlushPlan plan = s.plan_flush(20000, ClockReading{false, 3}, FlushTrigger::Cadence);
    ApplyResult ar = apply_flush_plan(plan, fx, t);

    CHECK(t.find(tk, &rec));
    CHECK(rec.state == TicketState::Done);
    CHECK_EQ(rec.flush_id, plan.flush_id);
    CHECK_EQ(rec.character_id, 14859u);
    CHECK_EQ(rec.tick_executed, 4242u);
    CHECK_EQ(rec.game_date_executed, std::string("1066.09.15"));
    CHECK(rec.result_json.find("\"success\":true") != std::string::npos);
    CHECK_EQ(ar.served_paused, size_t(1));

    // A completed ticket is terminal: a second flush cannot walk it back.
    t.mark_running(tk, 999, 30000);
    CHECK(t.find(tk, &rec));
    CHECK(rec.state == TicketState::Done);
    CHECK_EQ(rec.flush_id, plan.flush_id);

    // FAILED: a handler that produced nothing is a typed failure, never a
    // silent loss.
    FlushScheduler s2;
    TicketTable t2;
    FakeEffects fx2;
    fx2.serve_returns_empty = true;
    const uint64_t tk2 = submit(s2, t2, "s1", "query_realm", Polarity::PAUSED, "rid-2");
    FlushPlan p2 = s2.plan_flush(20000, ClockReading{false, 3}, FlushTrigger::Cadence);
    apply_flush_plan(p2, fx2, t2);
    CHECK(t2.find(tk2, &rec));
    CHECK(rec.state == TicketState::Failed);
    CHECK(rec.result_json.find("no_reply") != std::string::npos);

    // EXPIRED: aged out of the ring but still answerable — the intent ring's
    // incident was a SILENT eviction.
    TicketTable t3;
    t3.open(7, "s1", "rid-7", "query_realm", Polarity::PAUSED, /*now=*/0);
    CHECK_EQ(t3.expire_older_than(1000), size_t(0));   // TTL default is 5 min
    CHECK_EQ(t3.expire_older_than(400000), size_t(1));
    CHECK(t3.find(7, &rec));
    CHECK(rec.state == TicketState::Expired);
}

// ===========================================================================
static void test_unknown_ticket_is_typed() {
    section("check on an unknown ticket is a TYPED refusal, not silence");

    TicketTable t;
    t.open(1, "s1", "rid-1", "query_realm", Polarity::PAUSED, 0);

    Ticket rec;
    // A ticket id that was never minted is not found — which the `check`
    // handler turns into {state:"unknown", reason:"unknown_ticket"} rather
    // than omitting the row.
    CHECK(!t.find(2, &rec));
    CHECK(!t.find(0, &rec));
    CHECK(t.find(1, &rec));

    // Malformed ids parse to 0, which is never a live ticket, so they take the
    // same typed path instead of silently addressing ticket zero.
    CHECK_EQ(ticket_id_from_string("tk_00000001"), 1u);
    CHECK_EQ(ticket_id_from_string("1"), 1u);
    CHECK_EQ(ticket_id_from_string("tk_beef"), 0u);
    CHECK_EQ(ticket_id_from_string("tk_"), 0u);
    CHECK_EQ(ticket_id_from_string(""), 0u);
    CHECK_EQ(ticket_id_from_string(nullptr), 0u);
    CHECK_EQ(ticket_id_to_string(1), std::string("tk_00000001"));

    // Eviction is COUNTED, not silent.
    TicketTableConfig small;
    small.capacity = 2;
    TicketTable t2(small);
    t2.open(1, "s", "r1", "a", Polarity::ANY, 0);
    t2.open(2, "s", "r2", "a", Polarity::ANY, 0);
    CHECK_EQ(t2.evicted_count(), 0u);
    t2.open(3, "s", "r3", "a", Polarity::ANY, 0);
    CHECK_EQ(t2.size(), size_t(2));
    CHECK_EQ(t2.evicted_count(), 1u);
    CHECK(!t2.find(1, &rec));
    // ...and the evicted request_id index entry goes with it, so a legacy
    // client's lookup cannot resolve onto a recycled record.
    CHECK(!t2.find_by_request_id("r1", &rec));
    CHECK(t2.find_by_request_id("r3", &rec));
}

// ===========================================================================
static void test_legacy_request_id_client() {
    section("a legacy request_id-keyed client still gets its result (§8.2)");

    FlushScheduler s;
    TicketTable t;
    FakeEffects fx;

    // The client mints a request_id exactly as mcp_server.py:825-858 does today
    // and knows nothing about tickets.
    const uint64_t tk = submit(s, t, "s1", "query_realm", Polarity::PAUSED,
                               "b3f1e2d4-legacy");
    FlushPlan plan = s.plan_flush(20000, ClockReading{false, 3}, FlushTrigger::Cadence);
    apply_flush_plan(plan, fx, t);

    // The result is addressable by the client's OWN key...
    Ticket by_rid;
    CHECK(t.find_by_request_id("b3f1e2d4-legacy", &by_rid));
    CHECK(by_rid.state == TicketState::Done);
    CHECK_EQ(by_rid.id, tk);
    // ...and is the SAME record the ticket id addresses.
    Ticket by_tk;
    CHECK(t.find(tk, &by_tk));
    CHECK_EQ(by_tk.result_json, by_rid.result_json);

    // A result frame was emitted for it (pipeline.cpp keys that frame by the
    // same request_id, which is the whole of "no rewrite, no flag day").
    CHECK_EQ(fx.emitted.size(), size_t(1));
    CHECK_EQ(fx.emitted[0].ticket, tk);

    // A call with NO request_id (a pure ticket client) is still complete and
    // simply has no request_id index entry.
    const uint64_t tk2 = submit(s, t, "s1", "query_realm", Polarity::PAUSED, "", 20000);
    FlushPlan p2 = s.plan_flush(40000, ClockReading{false, 3}, FlushTrigger::Cadence);
    apply_flush_plan(p2, fx, t);
    Ticket rec;
    CHECK(t.find(tk2, &rec));
    CHECK(rec.state == TicketState::Done);
    CHECK(!t.find_by_request_id("", &rec));
}

// ===========================================================================
static void test_any_polarity_bypasses_the_clock() {
    section("ANY-polarity work never touches the clock (the ops ladder polls it)");

    FlushScheduler s;
    TicketTable t;
    FakeEffects fx;
    fx.clock = ClockReading{false, 5};   // NOT the resting speed (3)

    // get_tick_info / get_bridge_state / who_holds_the_lock: the entire ops
    // ladder (ck3_game_selfheal.sh, operator_page.sh, overlay_poller.py,
    // run_monitor.py) polls these. A flush carrying only ANY work must be
    // provably incapable of moving the world.
    submit(s, t, "s1", "get_tick_info",    Polarity::ANY, "r1");
    submit(s, t, "s1", "get_bridge_state", Polarity::ANY, "r2");
    submit(s, t, "s2", "who_holds_the_lock", Polarity::ANY, "r3");

    FlushPlan plan = s.tick(1, ClockReading{false, 5});
    CHECK(plan.trigger == FlushTrigger::AllAny);
    ApplyResult ar = apply_flush_plan(plan, fx, t);

    CHECK_EQ(ar.clock_effects, size_t(0));
    CHECK_EQ(fx.pauses, 0);
    CHECK_EQ(fx.unpauses, 0);
    CHECK_EQ(fx.speed_sets, 0);
    CHECK_EQ(fx.clock.speed, 5);          // not even "restored" to the resting speed
    CHECK_EQ(ar.served_any, size_t(3));
    CHECK_EQ(fx.served.size(), size_t(3));

    // A paused clock at the head of an all-ANY flush is DRIFT: reported, not
    // corrected. The hot path stays clock-silent.
    FlushScheduler s2;
    TicketTable t2;
    FakeEffects fx2;
    fx2.clock = ClockReading{true, 3};
    submit(s2, t2, "s1", "get_tick_info", Polarity::ANY, "r1");
    FlushPlan p2 = s2.tick(1, ClockReading{true, 3});
    ApplyResult ar2 = apply_flush_plan(p2, fx2, t2);
    CHECK(ar2.clock_drift_observed);
    CHECK_EQ(fx2.drift_notes, 1);
    CHECK_EQ(ar2.clock_effects, size_t(0));
    CHECK(fx2.clock.paused);              // still paused: we did NOT heal it

    // The empty heartbeat flush is the same promise: a flush record, no clock.
    FlushScheduler s3;
    TicketTable t3;
    FakeEffects fx3;
    FlushPlan p3 = s3.plan_flush(10000, ClockReading{false, 3}, FlushTrigger::Cadence);
    ApplyResult ar3 = apply_flush_plan(p3, fx3, t3);
    CHECK_EQ(ar3.clock_effects, size_t(0));
    CHECK_EQ(fx3.flush_records, 1);
    CHECK(p3.flush_id != 0);
}

// ===========================================================================
static void test_mixed_flush_ordering_and_drain() {
    section("mixed flush: one pause span, reads before writes, drain left RUNNING");

    FlushScheduler s;
    TicketTable t;
    FakeEffects fx;
    // The tier-3 mutation goes to the game-thread queue; the deferred-drain
    // READ is IPC-thread-safe and is served inline during the tick phase.
    fx.drain_actions = {"create_title"};

    const uint64_t any_tk   = submit(s, t, "s1", "get_tick_info",   Polarity::ANY,        "r0");
    const uint64_t read_tk  = submit(s, t, "s1", "query_realm",     Polarity::PAUSED,     "r1");
    const uint64_t tread_tk = submit(s, t, "s1", "query_valid_cbs", Polarity::TICK_READ,  "r2");
    const uint64_t write_tk = submit(s, t, "s2", "create_title",    Polarity::TICK_WRITE, "r3");

    FlushPlan plan = s.plan_flush(20000, ClockReading{false, 3}, FlushTrigger::Cadence);

    // §2.2: within the tick phase, READS are ordered before WRITES.
    CHECK_EQ(plan.tick_read_calls.size(), size_t(1));
    CHECK_EQ(plan.tick_write_calls.size(), size_t(1));
    CHECK_EQ(plan.tick_calls.size(), size_t(2));
    CHECK_EQ(plan.tick_calls[0].action, std::string("query_valid_cbs"));
    CHECK_EQ(plan.tick_calls[1].action, std::string("create_title"));

    ApplyResult ar = apply_flush_plan(plan, fx, t);

    CHECK_EQ(fx.pauses, 1);               // exactly ONE pause span (§2.6)
    CHECK_EQ(fx.unpauses, 1);
    CHECK(!fx.clock.paused);              // resting state restored: RUNNING

    // Inline service order: ANY, then the paused read, then the tick read.
    CHECK_EQ(fx.served.size(), size_t(3));
    CHECK_EQ(fx.served[0], std::string("get_tick_info"));
    CHECK_EQ(fx.served[1], std::string("query_realm"));
    CHECK_EQ(fx.served[2], std::string("query_valid_cbs"));

    // The write went to the EXISTING game-thread drain, and its ticket is left
    // RUNNING: it is settled by the drain, never by having been enqueued.
    CHECK_EQ(fx.enqueued.size(), size_t(1));
    CHECK_EQ(fx.enqueued[0], std::string("create_title"));
    CHECK_EQ(ar.enqueued_tick, size_t(1));

    Ticket rec;
    CHECK(t.find(any_tk, &rec));   CHECK(rec.state == TicketState::Done);
    CHECK(t.find(read_tk, &rec));  CHECK(rec.state == TicketState::Done);
    CHECK(t.find(tread_tk, &rec)); CHECK(rec.state == TicketState::Done);
    CHECK(t.find(write_tk, &rec)); CHECK(rec.state == TicketState::Running);
    // No result frame was emitted for the enqueued write — the drain emits its
    // own request_id-keyed reply, and a second copy would double-answer a
    // legacy client.
    CHECK_EQ(fx.emitted.size(), size_t(3));

    // ...and when the drain reports back, the ticket settles.
    t.complete(write_tk, "{\"success\":true}", plan.flush_id, 14859, 4243,
               "1066.09.16", 21000);
    CHECK(t.find(write_tk, &rec));
    CHECK(rec.state == TicketState::Done);
    CHECK_EQ(rec.tick_executed, 4243u);
}

// ===========================================================================
static void test_drain_enqueue_failure_is_typed() {
    section("a drain enqueue that fails is a FAILED ticket, never a lost one");

    FlushScheduler s;
    TicketTable t;
    FakeEffects fx;
    fx.drain_actions = {"create_title"};
    fx.enqueue_succeeds = false;

    const uint64_t tk = submit(s, t, "s1", "create_title", Polarity::TICK_WRITE, "r1");
    FlushPlan plan = s.plan_flush(20000, ClockReading{false, 3}, FlushTrigger::Cadence);
    ApplyResult ar = apply_flush_plan(plan, fx, t);

    Ticket rec;
    CHECK(t.find(tk, &rec));
    CHECK(rec.state == TicketState::Failed);
    CHECK(rec.result_json.find("drain_enqueue_failed") != std::string::npos);
    CHECK_EQ(ar.failed, size_t(1));
    CHECK_EQ(fx.emitted.size(), size_t(1));   // the client IS told
}

// ===========================================================================
static void test_backpressure_is_visible_to_the_ticket_layer() {
    section("back-pressure refuses; a refused call mints NO ticket");

    FlushConfig cfg;
    cfg.buffer_capacity = 4;
    cfg.per_session_capacity = 2;
    FlushScheduler s(cfg);
    TicketTable t;

    CHECK(submit(s, t, "hog", "query_realm", Polarity::PAUSED, "r1") != 0);
    CHECK(submit(s, t, "hog", "query_realm", Polarity::PAUSED, "r2") != 0);
    // Third call from the same session is refused by the per-session bound...
    CHECK_EQ(submit(s, t, "hog", "query_realm", Polarity::PAUSED, "r3"), 0u);
    // ...and no ticket was opened for it, so `check` on "r3" is honestly a
    // miss rather than a phantom pending row.
    Ticket rec;
    CHECK(!t.find_by_request_id("r3", &rec));
    CHECK_EQ(t.size(), size_t(2));
    // A different session is unaffected: one socket cannot fill the buffer.
    CHECK(submit(s, t, "polite", "query_realm", Polarity::PAUSED, "r4") != 0);
}


// ===========================================================================
// `cancel` (§2.10). Two halves, and they are different facts:
//   - the clean case: cancel lands BEFORE the flush that would run the call;
//   - the race:       cancel lands AFTER the plan was cut.
// Both must end with "the call did not run and the client got no result frame
// for it". The first must additionally not spend a slot of the flush's bounded
// batch, because that budget is §2.8's fairness axis.
// ===========================================================================
static void test_cancel_withdraws_the_call() {
    section("cancel before the flush: withdrawn from the buffer, never served");

    FlushConfig cfg;
    cfg.max_tick_calls_per_flush = 2;    // small, so a wasted slot is visible
    FlushScheduler s(cfg);
    TicketTable t;
    FakeEffects fx;

    // Three TICK_WRITEs from one session; the client changes its mind about
    // the first. With a budget of 2, a cancel that only failed the ticket would
    // let the dead call take a slot and leave "c" behind.
    const uint64_t a = submit(s, t, "s1", "a", Polarity::TICK_WRITE, "ra");
    const uint64_t b = submit(s, t, "s1", "b", Polarity::TICK_WRITE, "rb");
    const uint64_t c = submit(s, t, "s1", "c", Polarity::TICK_WRITE, "rc");
    CHECK_EQ(s.buffer_depth(), size_t(3));

    // Exactly what handle_cancel does, in the same order.
    CHECK(s.cancel(a));
    t.fail(a, "{\"success\":false,\"reason\":\"cancelled\",\"phase\":\"buffered\"}", 0);
    CHECK_EQ(s.buffer_depth(), size_t(2));
    // Cancelling it twice is a miss, not a second withdrawal.
    CHECK(!s.cancel(a));

    FlushPlan plan = s.plan_flush(20000, ClockReading{false, 3}, FlushTrigger::Cadence);
    ApplyResult ar = apply_flush_plan(plan, fx, t);

    // The budget went to live work, not to the cancelled call.
    CHECK_EQ(plan.tick_calls.size(), size_t(2));
    CHECK_EQ(plan.tick_calls[0].ticket, b);
    CHECK_EQ(plan.tick_calls[1].ticket, c);
    CHECK_EQ(ar.skipped_settled, size_t(0));

    // The cancelled call was never served and the client was never sent a
    // result frame for it — which a request_id-keyed client would have
    // resolved its (already cancelled) call on.
    CHECK_EQ(fx.served.size(), size_t(2));
    for (const std::string& act : fx.served) CHECK(act != "a");
    for (const EmittedFrame& e : fx.emitted) CHECK(e.ticket != a);

    // And the ticket still reads `cancelled`, not `done`.
    Ticket rec;
    CHECK(t.find(a, &rec));
    CHECK(rec.state == TicketState::Failed);
    CHECK(rec.result_json.find("cancelled") != std::string::npos);
}

// ===========================================================================
static void test_cancel_racing_the_flush_is_skipped_not_served() {
    section("cancel after the plan was cut: skipped by the apply layer, counted");

    FlushScheduler s;
    TicketTable t;
    FakeEffects fx;
    fx.drain_actions = {"create_title"};

    const uint64_t paused_tk = submit(s, t, "s1", "query_realm",  Polarity::PAUSED,     "r1");
    const uint64_t tick_tk   = submit(s, t, "s1", "create_title", Polarity::TICK_WRITE, "r2");
    const uint64_t live_tk   = submit(s, t, "s2", "query_realm",  Polarity::PAUSED,     "r3");

    // The plan is cut FIRST — the calls have already left the buffer, so
    // s.cancel() misses. This is the "best-effort" half of §2.10.
    FlushPlan plan = s.plan_flush(20000, ClockReading{false, 3}, FlushTrigger::Cadence);
    CHECK(!s.cancel(paused_tk));
    CHECK(!s.cancel(tick_tk));
    t.fail(paused_tk, "{\"success\":false,\"reason\":\"cancelled\",\"phase\":\"buffered\"}", 0);
    t.fail(tick_tk,   "{\"success\":false,\"reason\":\"cancelled\",\"phase\":\"buffered\"}", 0);

    ApplyResult ar = apply_flush_plan(plan, fx, t);

    // Both halves of the apply layer (inline serve AND the drain hand-off)
    // skip a settled ticket rather than running it.
    CHECK_EQ(ar.skipped_settled, size_t(2));
    CHECK_EQ(ar.served_paused, size_t(1));      // only s2's live read
    CHECK_EQ(fx.served.size(), size_t(1));
    CHECK_EQ(fx.enqueued.size(), size_t(0));    // the cancelled write never reached the drain
    CHECK_EQ(fx.emitted.size(), size_t(1));
    CHECK_EQ(fx.emitted[0].ticket, live_tk);

    // Terminal states are not walked back by the flush that raced them.
    Ticket rec;
    CHECK(t.find(paused_tk, &rec));
    CHECK(rec.state == TicketState::Failed);
    CHECK(t.find(tick_tk, &rec));
    CHECK(rec.state == TicketState::Failed);

    // The live call is unharmed, and the flush still left the world RUNNING.
    CHECK(t.find(live_tk, &rec));
    CHECK(rec.state == TicketState::Done);
    CHECK(!fx.clock.paused);
    CHECK_EQ(fx.pauses, 1);
    CHECK_EQ(fx.unpauses, 1);
}

// ===========================================================================
int main() {
    std::printf("test_pipeline (ck-6kvq.9)\n");

    test_ticket_lifecycle();
    test_unknown_ticket_is_typed();
    test_legacy_request_id_client();
    test_any_polarity_bypasses_the_clock();
    test_mixed_flush_ordering_and_drain();
    test_drain_enqueue_failure_is_typed();
    test_backpressure_is_visible_to_the_ticket_layer();
    test_cancel_withdraws_the_call();
    test_cancel_racing_the_flush_is_skipped_not_served();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) { std::printf("FAILED\n"); return 1; }
    std::printf("OK\n");
    return 0;
}
