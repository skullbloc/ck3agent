// ck-zs2x — the ring's LOSS must be measurable, and a wrap must be visible.
//
// This ring DROPS on full; it does not overwrite. That matters for what can be
// measured: the hits lost were never written to a slot, so the surviving batch
// looks complete and the data carries no trace of the loss. `dropped` is the only
// evidence, and before ck-zs2x nothing published it — a formatter built an
// aiwatch_dropped= string and was called from nowhere, so the literal was absent
// from the
// .so (strings = 0). Exists, is called, produces: three different claims, and only
// the first was true.
//
// The rows below measure the three things ck-zs2x asks for: the real CAPACITY, the
// threshold at which loss BEGINS (the wrap point), and whether a wrap is VISIBLE.
#include "ai_enabled_watch.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
static void row(const char* n, bool ok, const std::string& d) {
    printf("%-58s %s  %s\n", n, ok ? "PASS" : "FAIL", d.c_str());
    if (!ok) failures++;
}

static bridge::AiWatchHit hit(unsigned long long seq) {
    bridge::AiWatchHit h{};
    h.seq = seq; h.tick = 7300; h.rip = 0x4e22cb0; h.tid = 1234;
    h.char_id = 68445; h.value = 1; h.from_selftest = false;
    return h;
}

// Drain into a memory stream so the emitted LINES can be asserted, not just the
// counters. A row that checks a counter it also set proves nothing about output.
static std::string drain_to_string() {
    char* buf = nullptr; size_t len = 0;
    FILE* f = open_memstream(&buf, &len);
    bridge::ai_watch_drain(f);
    fflush(f); fclose(f);
    std::string out(buf ? buf : "", len);
    free(buf);
    return out;
}

static void reset_ring() {
    bridge::AiWatchHit tmp;
    while (bridge::g_ai_watch_ring.pop(&tmp)) {}
    bridge::g_ai_watch_ring.dropped.store(0, std::memory_order_relaxed);
    bridge::g_ai_watch_dropped_published.store(0, std::memory_order_relaxed);
}

int main() {
    // ── CAPACITY: 63, not 64 ──
    row("the_named_capacity_is_one_less_than_the_slots",
        bridge::AiWatchRing::kCapacity == bridge::AiWatchRing::kSlots - 1,
        "kSlots=" + std::to_string(bridge::AiWatchRing::kSlots) +
        " kCapacity=" + std::to_string(bridge::AiWatchRing::kCapacity));

    reset_ring();
    for (int i = 0; i < bridge::AiWatchRing::kCapacity; ++i)
        bridge::g_ai_watch_ring.push(hit(i));
    row("filling_to_CAPACITY_drops_nothing",
        bridge::g_ai_watch_ring.dropped.load(std::memory_order_relaxed) == 0,
        std::to_string(bridge::AiWatchRing::kCapacity) + " pushes, 0 dropped");

    // ── THE WRAP POINT, found by measurement rather than read off kSlots ──
    bridge::g_ai_watch_ring.push(hit(999));
    row("the_NEXT_push_is_the_one_that_drops",
        bridge::g_ai_watch_ring.dropped.load(std::memory_order_relaxed) == 1,
        "loss begins at push #" + std::to_string(bridge::AiWatchRing::kCapacity + 1) +
        ", which is kSlots — a reader reasoning from kSlots would call this a "
        "counter bug rather than the capacity");

    // ── IS THE WRAP VISIBLE? ──
    {
        const std::string out = drain_to_string();
        const bool has = out.find("[CK3-Bridge][aiwatch] DROPPED ") != std::string::npos;
        row("a_wrap_is_VISIBLE_on_a_line_of_its_own", has,
            has ? out.substr(out.find("DROPPED"), 70) : "no DROPPED line was emitted");
        row("the_line_carries_the_RAW_COUNT_and_the_capacity",
            out.find("new=1") != std::string::npos &&
            out.find("total=1") != std::string::npos &&
            out.find("capacity=63") != std::string::npos &&
            out.find("slots=64") != std::string::npos,
            "count and window are two facts; a reader is entitled to both");
        // A rate would divide a total loss by a window and report a small number.
        // There is no rate on this line, and if one is ever added it must go LATER.
        row("and_carries_NO_rate",
            out.find("per_sec") == std::string::npos &&
            out.find("rate") == std::string::npos &&
            out.find("%%") == std::string::npos,
            "a burst that lost everything must not be reportable as a small fraction");
        // The notice must precede the batch it affects, so a reader knows the batch
        // is incomplete BEFORE reading it.
        const size_t d = out.find("DROPPED");
        const size_t w = out.find("] WRITE ");
        row("the_notice_PRECEDES_the_batch_it_affects",
            d != std::string::npos && (w == std::string::npos || d < w),
            w == std::string::npos ? "no WRITE lines in this batch"
                                   : "DROPPED at " + std::to_string(d) +
                                     ", first WRITE at " + std::to_string(w));
    }

    // ── ANNOUNCED ONCE, then only the DELTA ──
    {
        const std::string again = drain_to_string();
        row("a_drain_with_no_NEW_loss_says_nothing",
            again.find("DROPPED") == std::string::npos,
            "the consumer remembers what a reader has been told; re-announcing an "
            "old loss on every drain would make the count unreadable");
    }
    {
        for (int i = 0; i < bridge::AiWatchRing::kCapacity + 3; ++i)
            bridge::g_ai_watch_ring.push(hit(2000 + i));
        const std::string out = drain_to_string();
        row("a_FURTHER_loss_reports_only_the_new_delta",
            out.find("new=3") != std::string::npos &&
            out.find("total=4") != std::string::npos,
            "3 more lost on top of 1: the delta is what is new, the total is what "
            "the run has lost");
    }

    // ── the ring RECOVERS its full capacity after a drain ──
    {
        reset_ring();
        for (int i = 0; i < bridge::AiWatchRing::kCapacity; ++i)
            bridge::g_ai_watch_ring.push(hit(i));
        drain_to_string();
        for (int i = 0; i < bridge::AiWatchRing::kCapacity; ++i)
            bridge::g_ai_watch_ring.push(hit(3000 + i));
        row("after_a_drain_the_FULL_capacity_is_available_again",
            bridge::g_ai_watch_ring.dropped.load(std::memory_order_relaxed) == 0,
            "the reserved slot moves with head/tail; it is not a slot that wears out");
    }

    // ck-arto: THE ROW THAT CALLED ai_watch_counters IS GONE WITH THE FUNCTION. It
    // asserted the dead formatter still mentioned `aiwatch_dropped=` — a property of a
    // string nothing ever emitted, and the only caller that function ever had was this
    // row. A test that is the sole consumer of the code it tests measures nothing about
    // the product; keeping it would have kept the function alive for its own test's
    // sake. The publication path is the DROPPED line, asserted above against the bytes
    // the drain actually writes.

    printf("\n%s (%d failing rows)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
