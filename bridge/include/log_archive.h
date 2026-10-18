// log_archive.h — ck-bwz1. Keep EVERY rotated window, not the last three.
//
// THE DEFECT. ck-otj9 made rotation preserve the retired window under
// <log>.1/.2/.3 instead of truncating it, and says out loud when the ring is
// full so a reader cannot mistake three files for full coverage. That is
// correct and it is still a BOUND. diag25 rotated TWELVE times: the counters
// and the absence of refusals were witnessed end-to-end by the rotation-proof
// tailer, but the per-line trace of DIAG ticks 1..3899 — about the first half
// of the run — is gone.
//
// So the ring stays at three in /tmp, where the quota hazard lives, and every
// retired window is ALSO archived, gzipped, unbounded by count, into a
// directory the operator names.
//
// WHY THE WORK IS NOT DONE WHERE THE ROTATION HAPPENS. rotate_log runs in the
// tick hook, on the GAME THREAD, every hundredth tick. Compressing 64MB there
// would stall the simulation for a second or more, twelve times a run — and
// this bridge's own episodes measure game-time progression, so a stall is not
// a free diagnostic, it is a perturbation of the thing being measured. The
// game thread therefore does O(1) work: it records the retired window's path
// and tick range in a fixed-size queue and returns. The accept thread, which
// already wakes periodically, drains it.
//
// The ring's depth is what makes that safe: at ~0.148 MB/tick a 64MB window is
// ~430 ticks, so a retired window sits at .1/.2/.3 for well over a thousand
// ticks before it falls off. The archiver has that long to copy it, and if it
// somehow does not, the window is counted as lost rather than presumed saved.
//
// CONTIGUITY IS THE ACCEPTANCE, which is why the tick range is in the NAME.
// A directory of archives is only evidence of complete coverage if the windows
// TILE — [a,b) [b,c) [c,d) with no gap. Counting files proves nothing, and
// that is the same mistake ck-otj9 refused to let the .1/.2/.3 count make.
#pragma once

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <fcntl.h>

namespace bridge {

// A retired window: the bytes are at `src`, and they cover [start_tick, end_tick).
// `end_tick` is the tick at which the NEXT window began, so adjacent windows
// share that number and contiguity is an equality, not an off-by-one guess.
struct ArchiveWindow {
    unsigned long long start_tick = 0;
    unsigned long long end_tick   = 0;
    char               src[512]   = {0};
};

// "<stem>.t<start>-t<end>.log.gz". The range is in the name so a reader — and
// the acceptance — can establish coverage from a directory listing alone,
// without opening 200MB of logs.
inline void archive_basename(char* out, size_t n, const char* stem,
                             unsigned long long start, unsigned long long end) {
    if (!out || n == 0) return;
    snprintf(out, n, "%s.t%llu-t%llu.log.gz",
             (stem && stem[0]) ? stem : "bridge", start, end);
}

// Recover the range from a basename produced above. False when the name is not
// one of ours — a directory may hold anything, and guessing a range for a file
// we did not write would manufacture coverage.
inline bool parse_archive_basename(const char* name,
                                   unsigned long long* start,
                                   unsigned long long* end) {
    if (!name || !start || !end) return false;
    const char* t = strstr(name, ".t");
    if (!t) return false;
    unsigned long long s = 0, e = 0;
    // sscanf, not hand-rolled: the failure mode of a hand parser here is a
    // PLAUSIBLE number, which would read as coverage.
    if (sscanf(t, ".t%llu-t%llu.log.gz", &s, &e) != 2) return false;
    if (e < s) return false;          // a window cannot end before it starts
    *start = s; *end = e;
    return true;
}

// Index of the first window that does NOT begin where its predecessor ended,
// or -1 when the set tiles. `w` must be sorted by start_tick.
//
// A GAP AND AN OVERLAP ARE BOTH FAILURES and both are reported here: a gap is
// missing evidence, an overlap means two files claim the same ticks and the
// set cannot be replayed in order.
inline int first_gap(const ArchiveWindow* w, size_t n) {
    if (!w || n < 2) return -1;
    for (size_t i = 1; i < n; ++i)
        if (w[i].start_tick != w[i - 1].end_tick) return static_cast<int>(i);
    return -1;
}

// A fixed queue. FIXED because the producer is the game thread inside the tick
// hook: no allocation, no lock, no path where a full queue blocks the
// simulation. A full queue DROPS and COUNTS the drop — never silently, because
// a dropped window is exactly the evidence this bead exists to keep.
// ck-8hn3: THE INDICES ARE ATOMIC BECAUSE THE THREADS ARE DIFFERENT ONES.
//
// The first version of this ring had plain `int head, tail` and plain counters.
// The producer is the GAME THREAD inside the tick hook; the consumer is the
// ACCEPT THREAD. `slot[tail] = w; tail = next;` with no release is free to be
// reordered, so the consumer can observe a published index whose payload has
// not been written yet — and two threads reading and writing a plain int is a
// data race, which is undefined rather than merely unlikely.
//
// That is the SAME DEFECT CLASS AS ck-2gtk, which is a non-atomic
// read-modify-write on a slot the engine publishes with `xchg`, and I wrote
// this one in the same session as that fix. Knowing the shape did not stop me
// producing it; what caught it was re-reading this file after a reviewer asked
// an unrelated question about the queue.
//
// Single producer, single consumer, so the discipline is the textbook one:
// the producer writes the SLOT and then RELEASES `tail`; the consumer ACQUIRES
// `tail` before reading the slot, and releases `head` after. Nothing stronger
// is needed and nothing weaker is correct.
//
// FIXED size because the producer is in the tick hook: no allocation, no lock,
// no path where a full queue blocks the simulation. A full queue DROPS and
// COUNTS the drop — never silently, because a dropped window is exactly the
// evidence this exists to keep.
//
// Still constant-initialisable: `std::atomic<int>`'s default constructor is
// constexpr, so `constexpr ArchiveQueue{}` compiles and the global needs no
// dynamic initializer — the property ck-t5bc verified, with the control that
// `constexpr std::string{}` does NOT compile.
struct ArchiveQueue {
    static constexpr int kSlots = 16;
    ArchiveWindow slot[kSlots];
    std::atomic<int> head{0}, tail{0};
    //: pushed/dropped are written by the PRODUCER, archived/failed by the
    //: CONSUMER, and all four are read by the heartbeat on the consumer's
    //: thread. Relaxed is right: they are counters for a log line, never a
    //: synchronisation signal, and no decision is taken on their values.
    std::atomic<unsigned long long> pushed{0}, dropped{0}, archived{0}, failed{0};

    bool empty() const {
        return head.load(std::memory_order_acquire) ==
               tail.load(std::memory_order_acquire);
    }
    //: APPROXIMATE across threads by construction, and only ever reported.
    int depth() const {
        return (tail.load(std::memory_order_relaxed) -
                head.load(std::memory_order_relaxed) + kSlots) % kSlots;
    }

    // PRODUCER ONLY (game thread).
    bool push(const ArchiveWindow& w) {
        const int t = tail.load(std::memory_order_relaxed);   // only we write it
        const int next = (t + 1) % kSlots;
        if (next == head.load(std::memory_order_acquire)) {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        slot[t] = w;
        // RELEASE: everything written to slot[t] above happens-before any
        // consumer that acquires this value of `tail`.
        tail.store(next, std::memory_order_release);
        pushed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // CONSUMER ONLY (accept thread).
    bool pop(ArchiveWindow* out) {
        const int h = head.load(std::memory_order_relaxed);   // only we write it
        if (h == tail.load(std::memory_order_acquire)) return false;
        if (out) *out = slot[h];
        head.store((h + 1) % kSlots, std::memory_order_release);
        return true;
    }
};

// ── ck-lockfree-assert: THE CONTROL, not the observation ─────────────────────
//
// ck-yg1s REPORTED that these atomics are lock-free and that the property was
// static_asserted. The measurement was real; the assert was not. It lived in a
// throwaway probe under /tmp that nothing in this tree compiles, so the claim
// could not have been contradicted by any build, any gate or any reviewer — a
// control that exists only in /tmp is a control that does not exist.
//
// WHY IT IS LOAD-BEARING. `push()` runs on the GAME THREAD inside the tick hook.
// A std::atomic<T> that is NOT lock-free is backed by a mutex, so on a platform
// where these are not lock-free the archive queue would take a lock in the
// simulation's hot path — the exact hazard the whole bead exists to avoid, and
// it would be silent. These asserts turn that platform into a BUILD FAILURE.
//
// At namespace scope deliberately: every translation unit that includes this
// header checks them, so the control cannot be bypassed by a caller who does not
// happen to instantiate the queue.
static_assert(std::atomic<int>::is_always_lock_free,
              "ArchiveQueue::head/tail are written from the tick hook; a "
              "mutex-backed atomic would block the game thread");
static_assert(std::atomic<unsigned long long>::is_always_lock_free,
              "ArchiveQueue::pushed/dropped are written from the tick hook; a "
              "mutex-backed atomic would block the game thread");

inline const char* format_archive_counters(char* buf, size_t n,
                                           const ArchiveQueue& q) {
    snprintf(buf, n,
             "log_archive_pushed=%llu log_archive_done=%llu "
             "log_archive_failed=%llu log_archive_dropped=%llu depth=%d",
             q.pushed.load(std::memory_order_relaxed),
             q.archived.load(std::memory_order_relaxed),
             q.failed.load(std::memory_order_relaxed),
             q.dropped.load(std::memory_order_relaxed), q.depth());
    return buf;
}

//: The one queue. Written by the GAME THREAD at rotation, drained by the
//: ACCEPT THREAD. A fixed ring with no allocation, so the producer side is
//: bounded work inside the tick hook.
inline ArchiveQueue g_log_archive_q;

// ── the mechanism, in log_archive.cpp (needs a process, not just a header) ──
bool archive_window_begin(const ArchiveWindow& w, const char* dir);
int  archive_window_reap();
bool archive_busy();
void archive_pump(ArchiveQueue& q, const char* dir);

}  // namespace bridge
