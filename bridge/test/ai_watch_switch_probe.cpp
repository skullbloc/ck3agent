// ai_watch_switch_probe.cpp — DOES THE WATCH FOLLOW THE SUBJECT? (ck-6xi6 recut)
//
// Gate 0.5 for run 2 failed on a reading of the instrument, not of the game:
// the watch read g_controlled_primary ONCE, in its arming condition, and then
// tracked that one character for the whole run. Run 2's design reaches its
// subject through ep-03's own control path — 30853 -> 36375 -> 32642 -> 36552 —
// so the old instrument would have armed on the first, sat through every switch,
// and retired terminally at the first death, 850 ticks before the real subject
// was ever controlled. It would have emitted ARMED, SELFTEST PASS and
// CONTROLLER-GONE and measured nothing: a null for an INSTRUMENT reason, wearing
// the shape of a real one.
//
// WHAT THIS PROBE CAN AND CANNOT ESTABLISH, said up front because the split
// matters. ai_watch_arm needs only an ADDRESS, so the whole switch mechanism is
// host-drivable against two ordinary heap bytes with no game and no CK3. What is
// NOT driven here is the per-tick decision in bridge_init.cpp that COMPARES
// g_controlled_primary against g_ai_watch_char and calls the switch — that lives
// inline in the tick hook. Driving the mechanism and asserting the call site are
// two different claims, and a probe that proved only the first would leave the
// wiring unwitnessed, which is precisely how gate 0.5 came to fail. The call
// site is asserted structurally by scripts/acceptance_6xi6.py.
//
// THE ATTRIBUTION BUG THIS ALSO CATCHES. The drain used to label every hit with
// the LIVE g_ai_watch_char. Harmless while the subject could never change;
// an attribution bug the moment it can, because the old character's writes would
// print under the new character's id — and run 2's whole question is which
// character a given write landed on.

#include "ai_enabled_watch.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;
static void row(const char* name, bool ok, const std::string& detail) {
    printf("%-52s %s  %s\n", name, ok ? "PASS" : "FAIL", detail.c_str());
    if (!ok) ++g_failures;
}

static std::atomic<uint64_t> g_tick{0};

// Read the log back rather than trusting a return value: the LINE is the
// artefact the readout consumes, so the line is what has to be checked.
static FILE* g_lf = nullptr;
static std::string slurp(const char* path) {
    //: FLUSH FIRST. The log is a buffered FILE*, so without this every read-back
    //: returns an EMPTY string and every row that inspects the log fails — which
    //: looks exactly like the instrument emitting nothing. The apparatus lying is
    //: indistinguishable from the subject being silent unless you flush.
    if (g_lf) fflush(g_lf);
    FILE* f = fopen(path, "rb");
    if (!f) return std::string();
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

// seq= must increase down the whole stream, whoever emitted each line. That is
// the property that makes "was there a SWITCH between the disable and this
// value" answerable at all.
static bool seqs_increase(const std::string& log, std::vector<uint64_t>* out) {
    size_t i = 0;
    uint64_t last = 0;
    bool first = true, ok = true;
    while ((i = log.find("seq=", i)) != std::string::npos) {
        const uint64_t v = strtoull(log.c_str() + i + 4, nullptr, 10);
        if (!first && v <= last) ok = false;
        out->push_back(v);
        last = v;
        first = false;
        i += 4;
    }
    return ok;
}

int main() {
    //: UNBUFFERED. A crash with a block-buffered stdout loses every row
    //: printed before it, so the output cannot say how far the probe got —
    //: which turns a located fault into an unlocated one.
    setvbuf(stdout, nullptr, _IONBF, 0);
    fprintf(stderr, "[probe] start\n");
    char logpath[] = "/tmp/ck3-6xi6-switch-XXXXXX";
    const int fd = mkstemp(logpath);
    FILE* lf = fd >= 0 ? fdopen(fd, "w+") : nullptr;
    g_lf = lf;
    if (!lf) { printf("NOT EVALUABLE: no temp log (reason_code=no_tmpfile)\n"); return 3; }

    // Two DISTINCT bytes, heap-allocated, standing in for two characters'
    // ai_ctrl+0x28. Nothing here needs the game: the watchpoint is armed on an
    // address, and which object owns that address is the caller's business.
    auto* a = static_cast<volatile unsigned char*>(calloc(64, 1));
    auto* b = static_cast<volatile unsigned char*>(calloc(64, 1));
    if (!a || !b) { printf("NOT EVALUABLE: calloc failed\n"); return 3; }

    const uint32_t kOld = 30853;   // ep-03's first controlled character
    const uint32_t kNew = 36552;   // the adventurer run 2 is actually after

    g_tick.store(5);
    if (!bridge::ai_watch_arm(reinterpret_cast<uintptr_t>(a), kOld, &g_tick, lf)) {
        // A hardware watchpoint needs perf_event_open. Denied is NOT a failure of
        // the subject and must not be reported as one — it is the instrument
        // being unavailable, which is a different state with a different fix.
        printf("NOT EVALUABLE: perf_event_open/HW_BREAKPOINT unavailable here "
               "(reason_code=watchpoint_unavailable). Check "
               "/proc/sys/kernel/perf_event_paranoid.\n");
        fclose(lf); remove(logpath);
        return 3;
    }
    row("the_watch_ARMS_on_a_host_byte", true,
        "no game needed: arm takes an address, so the whole switch mechanism is "
        "host-drivable");

    // ── a write to the OLD subject ──────────────────────────────────────────
    *a = 1;
    bridge::ai_watch_drain(lf);

    // ── THE SWITCH, as the tick hook would call it ──────────────────────────
    g_tick.store(1550);   // ep-03's handoff tick, where the old watch retired
    bridge::note_primary_change(kNew, bridge::kPrimaryCauseControl);
    bridge::ai_watch_switch(kOld, kNew, reinterpret_cast<uintptr_t>(b),
                            bridge::primary_change_cause_for(kNew),
                            &g_tick, lf);
    row("the_switch_RE_ARMS_rather_than_retiring",
        bridge::g_ai_watch_on.load(std::memory_order_acquire)
        && !bridge::g_ai_watch_retired.load(std::memory_order_acquire),
        "armed on the new subject, and NOT retired — retiring here is what made "
        "gate 0.5 fail");
    row("the_switch_moved_the_WATCHED_CHARACTER",
        bridge::g_ai_watch_char.load(std::memory_order_relaxed) == kNew,
        "g_ai_watch_char follows the session's primary");

    // ── a write to the NEW subject ──────────────────────────────────────────
    g_tick.store(2400);   // ep-03's tick for the adventurer
    *b = 1;
    bridge::ai_watch_drain(lf);

    const std::string log = slurp(logpath);

    // ── ATTRIBUTION: each write under the character it landed on ────────────
    char want_old[96], want_new[96];
    snprintf(want_old, sizeof want_old, "char=%u", kOld);
    snprintf(want_new, sizeof want_new, "char=%u", kNew);
    const size_t sw = log.find("PRIMARY-SWITCHED");
    const size_t w_old = log.find("WRITE");
    const size_t w_new = log.rfind("WRITE");

    row("the_SWITCH_line_is_emitted_with_both_ids",
        sw != std::string::npos
        && log.find("char=30853 -> 36552", sw) != std::string::npos,
        "PRIMARY-SWITCHED names what it switched FROM and TO — a reader must not "
        "have to infer the old subject from an earlier line");
    row("the_switch_records_its_CAUSE",
        sw != std::string::npos && log.find("cause=control", sw) != std::string::npos,
        "read from the register the originating action wrote, paired with the id, "
        "not inferred from what else shared the tick");
    row("the_OLD_write_is_attributed_to_the_OLD_character",
        w_old != std::string::npos && sw != std::string::npos && w_old < sw
        && log.compare(log.rfind(want_old, sw), strlen(want_old), want_old) == 0,
        "the drain used to stamp every hit with the LIVE watched character, which "
        "would relabel this write as 36552's the moment the switch happened");
    row("the_NEW_write_is_attributed_to_the_NEW_character",
        w_new != std::string::npos && w_new > sw
        && log.find(want_new, w_new) != std::string::npos,
        "and the write after the switch belongs to the new subject");
    row("the_switch_sits_BETWEEN_the_two_writes",
        w_old < sw && sw < w_new,
        "ORDERED: the drain runs before any game-thread line, so 'was there a "
        "switch between these two values' is answerable from the log alone");

    // ── THE ORDER IS CHECKABLE, not asserted ────────────────────────────────
    std::vector<uint64_t> seqs;
    const bool inc = seqs_increase(log, &seqs);
    // COUNT THE LINES, NOT JUST THE STAMPS. My first version scanned for "seq="
    // and checked those were increasing — so a line with NO seq at all could not
    // fail it, which is the vacuous-green shape: the row could not fail for the
    // reason it names. ARMED and SELFTEST had no seq and the row passed anyway.
    size_t aiwatch_lines = 0;
    for (size_t i = 0; (i = log.find("[CK3-Bridge][aiwatch]", i)) != std::string::npos; ++i)
        ++aiwatch_lines;
    row("EVERY_aiwatch_line_carries_a_monotonic_seq",
        inc && aiwatch_lines > 0 && seqs.size() == aiwatch_lines,
        std::to_string(seqs.size()) + " seq stamps across " +
        std::to_string(aiwatch_lines) + " aiwatch lines" +
        (seqs.size() == aiwatch_lines
            ? " — every line placeable in one order"
            : " — A LINE WITHOUT A SEQ CANNOT BE ORDERED against the writes"));

    // ── A DEATH MUST NOT RETIRE THE WATCH ───────────────────────────────────
    bridge::ai_watch_disarm(kNew, lf, /*terminal=*/false);
    row("a_death_DISARMS_without_retiring",
        !bridge::g_ai_watch_on.load(std::memory_order_acquire)
        && !bridge::g_ai_watch_retired.load(std::memory_order_acquire),
        "so the arming block re-arms when the session takes another primary — in "
        "ep-03 the subject is controlled 850 ticks AFTER the first death");
    const std::string log2 = slurp(logpath);
    row("and_says_the_window_is_COMPLETE_not_void",
        log2.find("CONTROLLER-GONE char=") != std::string::npos
        && log2.find("is COMPLETE for this character") != std::string::npos,
        "MOVED voids its window (freed memory); a death does not (the readings "
        "were correct, of that character) — different facts, different sentences");

    // ── AND TERMINAL RETIREMENT STILL EXISTS ────────────────────────────────
    g_tick.store(9999);
    if (bridge::ai_watch_arm(reinterpret_cast<uintptr_t>(b), kNew, &g_tick, lf)) {
        bridge::ai_watch_disarm(kNew, lf, /*terminal=*/true);
        const std::string log3 = slurp(logpath);
        row("terminal_retirement_is_still_REACHABLE_and_distinct",
            bridge::g_ai_watch_retired.load(std::memory_order_acquire)
            && log3.find("CONTROLLER-GONE-TERMINAL") != std::string::npos,
            "the unrecoverable case keeps its own literal and its own sentence — "
            "making the death path non-terminal must not delete the terminal one");
    } else {
        row("terminal_retirement_is_still_REACHABLE_and_distinct", false,
            "could not re-arm to test it — NOT a pass");
    }

    // ── THREAD COVERAGE: the finding that made run 1's watchpoint column
    // unreadable. perf_event_open(pid=0) scopes a breakpoint to the CALLING
    // thread, and the tick hook runs on a five-worker pool — ck-zgh4 measured
    // two different tids on two ARMED lines 36 ticks apart. A single arm sees one
    // worker's share, so a zero cannot mean "nothing wrote".
    //
    // Driven here with real threads: spawn some, cover the process, and require
    // that a write from a thread that did NOT arm is still caught.
    {
        bridge::ai_watch_close_all_tids();
        bridge::g_ai_watch_on.store(false, std::memory_order_release);
        bridge::g_ai_watch_retired.store(false, std::memory_order_release);
        bridge::g_ai_watch_hits.store(0, std::memory_order_relaxed);

        std::atomic<bool> go{false}, done{false}, covered_self{false};
        auto* c = static_cast<volatile unsigned char*>(calloc(64, 1));
        // A worker standing in for a pool thread the tick hook lands on: it calls
        // cover_this_thread (as the hook does) and then writes the byte.
        //
        // IT HAS TO ARM ITSELF, and that is not a stylistic choice. Arming a
        // sibling from another thread is EACCES under perf_event_paranoid=2 — even
        // passing your OWN tid explicitly is refused — so the only mechanism
        // available is pid=0 from the thread itself.
        // THE STRONG CASE: this worker NEVER calls into the watch at all. It does
        // not arm, and it does not stand in for a thread the hook visits. It is a
        // thread that exists and writes — which is exactly the thread a
        // self-arming scheme could never cover, and which enumeration does.
        std::thread worker([&]{
            while (!go.load()) std::this_thread::yield();
            *c = 7;
            done.store(true);
        });
        g_tick.store(100);
        const bool armed = bridge::ai_watch_arm(
            reinterpret_cast<uintptr_t>(c), 12345, &g_tick, lf);
        const unsigned long long before =
            bridge::g_ai_watch_hits.load(std::memory_order_relaxed);
        go.store(true);
        worker.join();
        while (!done.load()) std::this_thread::yield();
        bridge::ai_watch_drain(lf);
        const int covered = bridge::g_ai_watch_ntids.load(std::memory_order_acquire);
        const unsigned long long after =
            bridge::g_ai_watch_hits.load(std::memory_order_relaxed);
        row("arming_covers_EVERY_THREAD_not_just_the_arming_one",
            armed && covered > 1 && !covered_self.load(),
            std::to_string(covered) + " threads covered by enumerating "
            "/proc/self/task — and the writer never armed anything, which is the "
            "case a self-arming scheme cannot reach");
        row("and_ITS_write_is_then_CAUGHT", after > before,
            "hits " + std::to_string(before) + " -> " + std::to_string(after) +
            " — a write on the second worker, invisible to a single-thread arm");
        const std::string lg = slurp(logpath);
        row("each_covered_thread_is_NAMED_with_its_comm",
            lg.find("THREAD-ARMED") != std::string::npos
            && lg.find("comm=") != std::string::npos,
            "'which of the five workers' is how the node seat reads the log, and a "
            "bare tid does not say that");
        bridge::ai_watch_close_all_tids();
    }

    //: ON FAILURE, SHOW THE ARTEFACT. A row that fails while the log stays
    //: hidden makes the reader guess which of the log and the row is wrong.
    if (g_failures) {
        const std::string dump = slurp(logpath);
        fprintf(stderr, "\n--- aiwatch log ---\n%s-------------------\n",
                dump.c_str());
    }
    fclose(lf); remove(logpath);
    printf("\n%s (%d failing rows)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
