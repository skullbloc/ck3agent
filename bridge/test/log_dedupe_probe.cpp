// ck-os50 — the hot log line fires once per character, and the count survives.
//
// HOST ONLY. Pure header, no engine, no game: FirstPerKeyLog was put in a
// header precisely so the dedupe could be graded without a live bridge.
//
//   O1  the first occurrence of a key logs
//   O2  repeats of the SAME key do not log
//   O3  THE CONTROL: two DIFFERENT characters both log -- the dedupe must not
//       pass by suppressing everything, which is the cheap wrong fix
//   O4  the count is not lost: a rollup lands on the Nth suppression and
//       carries total/distinct/suppressed
//   O5  the d43 shape: 195,823 occurrences over 2 chars collapses from 195,823
//       lines to 2 + 195 rollups, and `total` still reconciles
//   O6  a rollup never fires on a first occurrence (that line is already in
//       the log, and doubling it is how a dedupe becomes its own spam)
#include <cstdio>
#include <cstdlib>
#include "log_dedupe.h"

static int failures = 0;

static void check(const char* name, bool ok, const char* detail = "") {
    std::printf("  %s  %s%s%s\n", ok ? "PASS" : "FAIL", name,
                detail[0] ? "  " : "", detail);
    if (!ok) ++failures;
}

int main() {
    {
        bridge::FirstPerKeyLog g;
        auto a = g.observe(33403);
        check("O1 the first occurrence of a key logs", a.log_line && a.distinct == 1);

        auto b = g.observe(33403);
        check("O2 a repeat of the same key does not log", !b.log_line && b.suppressed == 1);

        // ── O3 THE CONTROL ──────────────────────────────────────────────────
        auto c = g.observe(33777);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "distinct=%zu suppressed=%zu",
                      c.distinct, c.suppressed);
        check("O3 CONTROL: a DIFFERENT character still logs", c.log_line
              && c.distinct == 2 && c.suppressed == 1, buf);
    }
    {
        // ── O4 the count reaches the log ────────────────────────────────────
        bridge::FirstPerKeyLog g;
        g.observe(1);
        bool saw_rollup = false;
        bridge::FirstPerKeyLog::Decision at;
        for (std::size_t i = 0; i < bridge::kRollupEvery; ++i) {
            auto d = g.observe(1);
            if (d.log_rollup) { saw_rollup = true; at = d; }
        }
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "rollup at total=%zu distinct=%zu suppressed=%zu",
                      at.total, at.distinct, at.suppressed);
        check("O4 a rollup lands on the Nth suppression and carries the counts",
              saw_rollup && at.suppressed == bridge::kRollupEvery
              && at.total == bridge::kRollupEvery + 1 && at.distinct == 1, buf);
    }
    {
        // ── O5 the d43 shape ────────────────────────────────────────────────
        bridge::FirstPerKeyLog g;
        const std::size_t kD43 = 195823;
        std::size_t lines = 0, rollups = 0;
        for (std::size_t i = 0; i < kD43; ++i) {
            auto d = g.observe(i % 2 ? 33403u : 33777u);
            if (d.log_line) ++lines;
            if (d.log_rollup) ++rollups;
        }
        const std::size_t emitted = lines + rollups;
        char buf[220];
        std::snprintf(buf, sizeof(buf),
                      "%zu occurrences -> %zu line(s) + %zu rollup(s) = %zu emitted "
                      "(was %zu); total=%zu", kD43, lines, rollups, emitted, kD43,
                      g.total());
        check("O5 the d43 shape collapses and `total` still reconciles",
              lines == 2 && g.total() == kD43
              && g.suppressed() == kD43 - 2 && emitted < kD43 / 100, buf);
    }
    {
        // ── O6 a rollup never doubles a first occurrence ────────────────────
        bridge::FirstPerKeyLog g;
        bool both = false;
        for (std::uint32_t k = 0; k < 4000; ++k) {
            auto d = g.observe(k);          // every one of these is a FIRST
            if (d.log_line && d.log_rollup) both = true;
        }
        check("O6 a rollup never fires on a first occurrence", !both);
    }

    std::printf("\n%d failed\n", failures);
    return failures ? 1 : 0;
}
