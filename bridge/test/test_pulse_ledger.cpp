// test_pulse_ledger.cpp — R1c host unit tests (ck-6kvq.10).
//
// Design of record: docs/design/event-pipeline-conductor.md §2.9.
//
// No CK3 binary, no game, no engine. Memory is a FakeMemory map, so the exact
// hazard the design describes — a `siglongjmp` out of the middle of a pulse —
// is reproduced here with a REAL siglongjmp, not simulated with a flag.
//
// THE NEGATIVE CONTROL IS THE POINT (test 6). The ck-hxh7 lane proved its fix by
// REMOVING it and watching the test reproduce the live symptom. Same discipline:
// test 6 runs the identical crash scenario with the ledger DISABLED and asserts
// the global is left CLOBBERED — proving the test reproduces the real failure
// and that the ledger, not the test's own scaffolding, is what fixes it.

#include "pulse_ledger.h"

#include <setjmp.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace bridge;

// ---------------------------------------------------------------- harness ---
static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) {                                                        \
            ++g_failures;                                                     \
            printf("  FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__);       \
        }                                                                     \
    } while (0)

#define CHECK_EQ(a, b, msg)                                                   \
    do {                                                                      \
        ++g_checks;                                                           \
        auto _a = (a); auto _b = (b);                                         \
        if (!(_a == _b)) {                                                    \
            ++g_failures;                                                     \
            printf("  FAIL: %s  (got %lld, want %lld)  (%s:%d)\n", (msg),     \
                   (long long)_a, (long long)_b, __FILE__, __LINE__);         \
        }                                                                     \
    } while (0)

static void section(const char* name) { printf("\n[%s]\n", name); }

// ------------------------------------------------------------ FakeMemory ----
// A sparse address space. Anything not explicitly mapped is unmapped, so the
// "could not even read it back" path is testable too.
class FakeMemory : public PulseMemory {
public:
    std::map<uintptr_t, uint64_t> cells;
    size_t writes = 0;

    void map(uintptr_t a, uint64_t v) { cells[a] = v; }

    bool mapped(uintptr_t a) const override { return cells.count(a) != 0; }
    uint64_t read(uintptr_t a, uint8_t) const override {
        auto it = cells.find(a);
        return it == cells.end() ? 0 : it->second;
    }
    void write(uintptr_t a, uint8_t, uint64_t v) override {
        cells[a] = v;
        ++writes;
    }
};

// Stand-ins for the two globals the design names by address.
static constexpr uintptr_t AI_WAR_SUPPRESS_ADDR   = 0xBEAFB48;
static constexpr uintptr_t ALLOW_RANDOM_ADDR      = 0xBECD570;

// ===========================================================================
// 1. The happy path: enter, write, restore, exit -> the ledger is EMPTY.
// ===========================================================================
static void test_clean_pulse() {
    section("1. a completed pulse leaves the ledger empty");
    PulseLedger L;
    FakeMemory m;
    m.map(AI_WAR_SUPPRESS_ADDR, 1);

    const uint64_t saved = m.read(AI_WAR_SUPPRESS_ADDR, 1);
    const uint64_t h = L.enter("AI_WAR_SUPPRESS", "execute_war_intent",
                               AI_WAR_SUPPRESS_ADDR, 1, saved, /*flush*/ 7, 1000);
    CHECK(h != 0, "enter returns a handle");
    CHECK_EQ(L.live(), 1u, "one pulse live while the engine call runs");

    m.write(AI_WAR_SUPPRESS_ADDR, 1, 0);       // the pulse
    m.write(AI_WAR_SUPPRESS_ADDR, 1, saved);   // the restore
    L.exit(h);

    CHECK_EQ(L.live(), 0u, "ledger empty after exit");

    std::vector<PulseLeak> leaks;
    CHECK_EQ(L.sweep(PulseScope::Drain, 8, 2000, m, &leaks, true), 0u,
             "a clean pulse yields NO leak");
    CHECK(leaks.empty(), "no leak records");
    CHECK_EQ(L.leak_count(), 0u, "leak counter still zero");
    CHECK_EQ(m.read(AI_WAR_SUPPRESS_ADDR, 1), 1u, "global holds its original value");
}

// ===========================================================================
// 2. THE HAZARD, with a real siglongjmp out of the middle of the pulse.
//    Matches bridge_init.cpp:1736-1750: the outer recovery jumps clean out of
//    the drain, past every catch(...) and every destructor and past the restore.
// ===========================================================================
static sigjmp_buf g_outer_recovery;

static void pulse_and_explode(PulseLedger& L, FakeMemory& m) {
    const uint64_t saved = m.read(AI_WAR_SUPPRESS_ADDR, 1);
    (void)L.enter("AI_WAR_SUPPRESS", "execute_war_intent",
                  AI_WAR_SUPPRESS_ADDR, 1, saved, /*flush*/ 7, 1000);
    m.write(AI_WAR_SUPPRESS_ADDR, 1, 0);   // cleared...
    siglongjmp(g_outer_recovery, 1);       // ...and the restore NEVER RUNS.
}

static void test_longjmp_leak_is_caught_and_healed() {
    section("2. a siglongjmp mid-pulse is caught AND healed");
    PulseLedger L;
    FakeMemory m;
    m.map(AI_WAR_SUPPRESS_ADDR, 1);   // the world's AI is pacing its wars

    if (sigsetjmp(g_outer_recovery, 1) == 0) {
        pulse_and_explode(L, m);
        CHECK(false, "unreachable: the longjmp must fire");
    }

    // This is the live symptom: AI_WAR_SUPPRESS is 0, for the rest of the
    // process, and nothing in the bridge would ever notice.
    CHECK_EQ(m.read(AI_WAR_SUPPRESS_ADDR, 1), 0u,
             "pre-sweep: the global IS clobbered (the symptom reproduces)");
    CHECK_EQ(L.live(), 1u, "the ledger still holds the pulse (it survived the jump)");

    std::vector<PulseLeak> leaks;
    const size_t n = L.sweep(PulseScope::Drain, 7, 5000, m, &leaks, true);

    CHECK_EQ(n, 1u, "the sweep finds exactly one leak");
    CHECK_EQ(leaks.size(), 1u, "one leak record");
    CHECK(leaks[0].global == "AI_WAR_SUPPRESS", "leak names the global");
    CHECK(leaks[0].site == "execute_war_intent", "leak names the site");
    CHECK_EQ(leaks[0].saved, 1u, "leak carries `saved`");
    CHECK_EQ(leaks[0].observed, 0u, "leak carries `observed`");
    CHECK(leaks[0].restored, "leak says it restored");
    CHECK_EQ(leaks[0].age_ms, 4000u, "leak carries its age");

    CHECK_EQ(m.read(AI_WAR_SUPPRESS_ADDR, 1), 1u,
             "POST-SWEEP: the global is RESTORED, not leaked");
    CHECK_EQ(L.live(), 0u, "the ledger is empty again");
    CHECK_EQ(L.leak_count(), 1u, "leak counted");

    // The published record must be one line of valid, flat JSON — the abort
    // ladder flattens the telemetry block to key=value lines.
    const std::string j = leaks[0].to_json();
    CHECK(j.find('\n') == std::string::npos, "leak JSON is single-line");
    CHECK(j.find("\"global\":\"AI_WAR_SUPPRESS\"") != std::string::npos,
          "leak JSON carries global");
    CHECK(j.find("\"saved\":1") != std::string::npos, "leak JSON carries saved");
    CHECK(j.find("\"observed\":0") != std::string::npos, "leak JSON carries observed");
}

// ===========================================================================
// 3. The FLUSH-scope generation guard. A pulse still legitimately in flight on
//    the game thread must NOT be convicted by the conductor thread: a false
//    pulse_leak aborts a healthy episode.
// ===========================================================================
static void test_flush_generation_guard() {
    section("3. flush-scope sweep is generation-guarded");
    PulseLedger L;
    FakeMemory m;
    m.map(ALLOW_RANDOM_ADDR, 0xDEADBEEF);

    const uint64_t h = L.enter("ALLOW_RANDOM_IN_SCOPE", "arm_fake_random_scope",
                               ALLOW_RANDOM_ADDR, 8, 0xDEADBEEF, /*flush*/ 12, 100);
    m.write(ALLOW_RANDOM_ADDR, 8, 0xF00D);

    std::vector<PulseLeak> leaks;
    CHECK_EQ(L.sweep(PulseScope::Flush, /*current flush*/ 12, 200, m, &leaks, false), 0u,
             "flush 12 does NOT convict a pulse entered under flush 12");
    CHECK_EQ(L.live(), 1u, "the pulse is still live");
    CHECK_EQ(m.read(ALLOW_RANDOM_ADDR, 8), 0xF00Du, "and untouched");

    CHECK_EQ(L.sweep(PulseScope::Flush, /*current flush*/ 13, 300, m, &leaks, false), 1u,
             "flush 13 DOES convict it — it survived a whole flush boundary");
    CHECK_EQ(m.read(ALLOW_RANDOM_ADDR, 8), 0xDEADBEEFu, "and restores it");
    CHECK(leaks.back().scope == PulseScope::Flush, "scope recorded as flush");

    // The late restore must not free a slot somebody else has since claimed.
    L.exit(h);
    CHECK_EQ(L.stale_exit_count(), 1u, "the late exit is counted, not silent");
}

// ===========================================================================
// 4. self_thread_only: the drain anchor must not steal another thread's pulse.
// ===========================================================================
static void test_thread_scoping() {
    section("4. a drain sweep only convicts its own thread's pulses");
    PulseLedger L;
    FakeMemory m;
    m.map(ALLOW_RANDOM_ADDR, 42);

    // Another thread enters a pulse and leaves it open (it is still running).
    std::thread t([&] {
        (void)L.enter("ALLOW_RANDOM_IN_SCOPE", "other_thread",
                      ALLOW_RANDOM_ADDR, 8, 42, 1, 0);
    });
    t.join();

    std::vector<PulseLeak> leaks;
    CHECK_EQ(L.sweep(PulseScope::Drain, 1, 10, m, &leaks, /*self_thread_only=*/true), 0u,
             "this thread's drain sweep ignores the other thread's pulse");
    CHECK_EQ(L.live(), 1u, "it is still live");

    CHECK_EQ(L.sweep(PulseScope::Flush, 2, 20, m, &leaks, /*self_thread_only=*/false), 1u,
             "the cross-thread flush sweep is the net that catches it");
}

// ===========================================================================
// 5. Bookkeeping that must never be silent: overflow, address-exit, unmapped.
// ===========================================================================
static void test_bookkeeping() {
    section("5. overflow, address-exit and unmapped are all reported");
    PulseLedger L;
    FakeMemory m;

    // --- overflow ---
    for (size_t i = 0; i < PulseLedger::CAPACITY; ++i) {
        uintptr_t a = 0x1000 + i * 8;
        m.map(a, i);
        CHECK(L.enter("X", "fill", a, 8, i, 1, 0) != 0, "slot claimed");
    }
    CHECK_EQ(L.enter("X", "overflow", 0x9999, 8, 0, 1, 0), 0u,
             "a full ledger returns handle 0");
    CHECK_EQ(L.overflow_count(), 1u, "overflow is COUNTED, never silent");

    std::vector<PulseLeak> leaks;
    CHECK_EQ(L.sweep(PulseScope::Drain, 1, 0, m, &leaks, true),
             PulseLedger::CAPACITY, "the sweep drains every slot");

    // --- exit by address, LIFO, and quiet when nothing is live ---
    L.reset_for_test();
    m.map(ALLOW_RANDOM_ADDR, 7);
    const uint64_t h1 = L.enter("R", "outer", ALLOW_RANDOM_ADDR, 8, 7, 1, 0);
    const uint64_t h2 = L.enter("R", "inner", ALLOW_RANDOM_ADDR, 8, 9, 1, 0);
    CHECK(h2 > h1, "handles are monotonic");
    CHECK_EQ(L.exit_newest_for_addr(ALLOW_RANDOM_ADDR), h2, "LIFO: inner first");
    CHECK_EQ(L.exit_newest_for_addr(ALLOW_RANDOM_ADDR), h1, "then outer");
    CHECK_EQ(L.exit_newest_for_addr(ALLOW_RANDOM_ADDR), 0u, "then nothing");
    CHECK_EQ(L.stale_exit_count(), 0u,
             "a redundant ADDRESS exit is NOT a stale exit — several sites "
             "restore the same global on more than one path on purpose");

    // --- an unmapped address is reported honestly, not guessed at ---
    L.reset_for_test();
    (void)L.enter("GONE", "unmapped", 0xDEAD0000, 8, 5, 1, 0);
    leaks.clear();
    CHECK_EQ(L.sweep(PulseScope::Drain, 1, 0, m, &leaks, true), 1u, "still a leak");
    CHECK(!leaks[0].mapped, "reported as unmapped");
    CHECK(!leaks[0].restored, "and NOT claimed to have been restored");
}

// ===========================================================================
// 6. *** THE NEGATIVE CONTROL ***
//
//    The identical crash scenario as test 2, with the ledger DISABLED. If this
//    test ever goes green-by-healing, the mechanism under test is not the
//    ledger and every other assertion here is worthless. (ck-hxh7's lane proved
//    its fix by removing it and watching the symptom come back; same here.)
// ===========================================================================
static void test_negative_control() {
    section("6. NEGATIVE CONTROL: with the ledger off, the global stays leaked");
    PulseLedger L;
    L.set_enabled(false);            // <-- the ONLY difference from test 2
    FakeMemory m;
    m.map(AI_WAR_SUPPRESS_ADDR, 1);

    if (sigsetjmp(g_outer_recovery, 1) == 0) {
        pulse_and_explode(L, m);
        CHECK(false, "unreachable: the longjmp must fire");
    }

    CHECK_EQ(m.read(AI_WAR_SUPPRESS_ADDR, 1), 0u, "the global is clobbered");
    CHECK_EQ(L.live(), 0u, "the disabled ledger recorded nothing");

    std::vector<PulseLeak> leaks;
    CHECK_EQ(L.sweep(PulseScope::Drain, 7, 5000, m, &leaks, true), 0u,
             "a disabled ledger finds nothing to sweep");
    CHECK_EQ(m.read(AI_WAR_SUPPRESS_ADDR, 1), 0u,
             "*** AND THE GLOBAL STAYS CLOBBERED — the symptom reproduces, so "
             "test 2's green is the LEDGER's doing and not the harness's ***");
    CHECK_EQ(m.writes, 1u, "nothing wrote to the global except the pulse itself");

    // Sanity: re-enable and the same ledger heals. Same object, same scenario,
    // one flag.
    L.set_enabled(true);
    m.cells[AI_WAR_SUPPRESS_ADDR] = 1;
    if (sigsetjmp(g_outer_recovery, 1) == 0) {
        pulse_and_explode(L, m);
        CHECK(false, "unreachable");
    }
    leaks.clear();
    CHECK_EQ(L.sweep(PulseScope::Drain, 7, 5000, m, &leaks, true), 1u,
             "re-enabled: the leak is found");
    CHECK_EQ(m.read(AI_WAR_SUPPRESS_ADDR, 1), 1u, "re-enabled: and healed");
}

// ===========================================================================
// 7. The LATCHED verdict — a later clean flush must not erase the evidence.
// ===========================================================================
static void test_latched_verdict() {
    section("7. the pulse_leak verdict is latched, never cleared");
    pulse_reset_verdict_for_test();
    CHECK(!pulse_verdict().leaked, "clean to start");

    PulseLeak a;
    a.global = "AI_WAR_SUPPRESS"; a.saved = 1; a.observed = 0; a.site = "first";
    PulseLeak b;
    b.global = "ALLOW_RANDOM_IN_SCOPE"; b.saved = 9; b.observed = 0; b.site = "second";

    // FREE-PLAY: a leak is logged and healed, but there is no episode to void.
    pulse_latch_verdict(a, /*experiment_mode=*/false);
    pulse_latch_verdict(b, /*experiment_mode=*/false);

    PulseVerdict v = pulse_verdict();
    CHECK(v.leaked, "latched");
    CHECK_EQ(v.count, 2u, "both counted");
    CHECK(v.first.site == "first",
          "the FIRST leak is kept — it is the one that broke the episode");
    CHECK(!v.episode_aborted, "free-play: a leak does NOT abort — no episode to void");
    CHECK(!pulse_episode_aborted(), "and the containment flag is clear");
    CHECK(std::string(v.abort_reason) == "", "no abort reason");

    // Nothing in the API can un-latch it outside a test.
    v = pulse_verdict();
    CHECK(v.leaked, "still latched on a second read");
    pulse_reset_verdict_for_test();
}

// ===========================================================================
// 8. §2.9's abort half: in EXPERIMENT mode a leak aborts the episode.
// ===========================================================================
static void test_experiment_mode_abort() {
    section("8. experiment mode: a leak latches the episode abort");
    pulse_reset_verdict_for_test();

    PulseLedger L;
    FakeMemory m;
    m.map(AI_WAR_SUPPRESS_ADDR, 1);

    CHECK(!pulse_episode_aborted(), "not aborted to start");

    if (sigsetjmp(g_outer_recovery, 1) == 0) {
        pulse_and_explode(L, m);
        CHECK(false, "unreachable");
    }

    std::vector<PulseLeak> leaks;
    CHECK_EQ(L.sweep(PulseScope::Drain, 7, 5000, m, &leaks, true), 1u, "leak found");

    // This is exactly what pipeline.cpp's report_leaks() does with the sweep's
    // output when episode_is_experiment() (episode_policy.h) is true.
    for (const PulseLeak& l : leaks) pulse_latch_verdict(l, /*experiment_mode=*/true);

    PulseVerdict v = pulse_verdict();
    CHECK(v.leaked, "verdict latched");
    CHECK(v.episode_aborted, "*** the episode is ABORTED ***");
    CHECK(std::string(v.abort_reason) == "pulse_leak",
          "with the TYPED reason ck-6kvq.1's ladder uses (exit 13)");
    CHECK(pulse_episode_aborted(),
          "and the bridge-wide containment flag is set, so every further "
          "control-session submission is refused {reason:\"episode_aborted\"}");
    CHECK_EQ(m.read(AI_WAR_SUPPRESS_ADDR, 1), 1u,
             "the global is still healed — containment is IN ADDITION to the heal");

    // One-way. A later clean flush cannot clear it.
    pulse_latch_verdict(PulseLeak(), false);
    CHECK(pulse_episode_aborted(), "a later free-play latch does not un-abort");
    pulse_reset_verdict_for_test();
}

int main() {
    printf("=== ck-6kvq.10 R1c: pulse ledger (§2.9) ===\n");
    test_clean_pulse();
    test_longjmp_leak_is_caught_and_healed();
    test_flush_generation_guard();
    test_thread_scoping();
    test_bookkeeping();
    test_negative_control();
    test_latched_verdict();
    test_experiment_mode_abort();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) printf("PASS\n");
    else                 printf("FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
