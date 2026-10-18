// log_dedupe.h — ck-os50: a log line that fires per OCCURRENCE becomes the log.
//
// THE DEFECT THIS CLOSES. `resolve_title_realm` emitted one line per failed
// resolve, with no dedupe, counter or rate limit. Measured share of each run's
// bridge log:
//
//   d39 s01  100771/135578  74%      d45      56074/93977   60%
//   d39 s02  139946/173658  81%      d46      10179/44606   23%
//   d42       8449/44729    19%      d47      58732/91106   64%
//   d43     195823/229852   85%      d49      51455/84821   61%
//   d44       4531/17356    26%      d50      17978/50529   36%
//
// It is NOT a crash precursor — that is how it was first noticed, and the
// cross-run rates refuted it: it starts around line 600 of every run, runs to
// the last line written, appears in both arms, and fires in runs that reached
// the horizon and sealed valid. ck-zgh4 confirmed the d50 faulting stack
// carries no bridge frames at all.
//
// WHY IT STILL MATTERS. ck-otj9 rotates the log at 64MB keeping three windows,
// and rotation is what makes `coverage_is_floor` true and starts costing
// SEALED EVIDENCE. No run has rotated yet (0 rotation banners, 0 gaps across
// seven coverage files) but d43 reached 23.13MB with 85% of it one line, so
// this single message is consuming most of the headroom a longer run needs.
//
// THE SHAPE. First occurrence per KEY is logged; the rest are counted. A
// rollup is emitted every `kRollupEvery` suppressions so the count reaches the
// log even though the bridge has no episode-close hook to hang a total on —
// the information is kept, only the repetition is dropped. Bound is therefore
// O(distinct keys + total/kRollupEvery) instead of O(total).
//
// DELIBERATELY NOT A RATE LIMIT IN TIME. A time-based limiter drops a DIFFERENT
// key's first occurrence when two fire together, which loses the one line that
// carries information. Keying on the subject keeps every distinct fact and
// drops only repeats of a fact already recorded.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_set>

namespace bridge {

//: One rollup line per this many suppressed occurrences. Large enough that the
//: rollups cannot themselves become the log, small enough that a run which
//: ends abruptly still carries a recent count.
constexpr std::size_t kRollupEvery = 1000;

// First-occurrence-per-key gate for a hot log line.
//
// Thread-safe: `resolve_title_realm` is reached from the IPC thread and from
// the game thread, so an unguarded set here would be the data race that the
// log line is meant to help diagnose.
class FirstPerKeyLog {
public:
    struct Decision {
        bool        log_line   = false;  // emit the full line for this key
        bool        log_rollup = false;  // also emit a rollup now
        std::size_t distinct   = 0;      // keys seen so far
        std::size_t suppressed = 0;      // occurrences not logged
        std::size_t total      = 0;      // occurrences seen
    };

    // Record one occurrence of `key` and say what to log.
    Decision observe(std::uint32_t key) {
        std::lock_guard<std::mutex> lk(mu_);
        ++total_;
        const bool first = seen_.insert(key).second;
        if (!first) {
            ++suppressed_;
        }
        Decision d;
        d.log_line = first;
        d.distinct = seen_.size();
        d.suppressed = suppressed_;
        d.total = total_;
        // A rollup is due on the Nth suppression exactly, never on a first
        // occurrence: a first occurrence already put a line in the log.
        d.log_rollup = (!first && suppressed_ % kRollupEvery == 0);
        return d;
    }

    std::size_t distinct() const {
        std::lock_guard<std::mutex> lk(mu_);
        return seen_.size();
    }
    std::size_t suppressed() const {
        std::lock_guard<std::mutex> lk(mu_);
        return suppressed_;
    }
    std::size_t total() const {
        std::lock_guard<std::mutex> lk(mu_);
        return total_;
    }

private:
    mutable std::mutex              mu_;
    std::unordered_set<std::uint32_t> seen_;
    std::size_t                     suppressed_ = 0;
    std::size_t                     total_      = 0;
};

}  // namespace bridge
