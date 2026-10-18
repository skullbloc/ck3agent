// ck-otj9 — one spelling of "the log got too big", drivable by a test.
//
// THE DEFECT. Rotation used to be freopen(path, "w") on the live FILE*: the same
// inode, truncated in place, no copy retained. The only survivor was line 1 of
// the new file.
//
// That was written for an untraced log where discarding the old window costs
// nothing — and ck-zgh4 measured that it costs nothing because IT NEVER FIRES
// THERE: across 11 sealed bundles an untraced bridge.log is 250KB-1.1MB and not
// one had ever rotated. With CK3_BRIDGE_TRACE armed it is ~0.148 MB/tick, so one
// episode blows a 64MB cap repeatedly. The rotation's only real-world effect was
// on traced runs — precisely the runs whose evidence we cannot replace. In arma8
// it deleted an observed Backpressure drop line before anyone could read it.
//
// A diagnostic that erases its own output is worse than no diagnostic: the short
// log left behind reads like a quiet run rather than like a missing one.
//
// THIS LIVES IN A HEADER, AS A FUNCTION, ON PURPOSE. Written inline in the tick
// hook it was a behaviour a test could only grep for — and a grep cannot tell a
// rename from a truncate, nor notice the ring shifting the wrong way. Extracted,
// a probe drives it against real files in a temp directory.
#pragma once

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace bridge {

struct RotateResult {
    bool rotated  = false;   // the size threshold was crossed and we acted
    bool archived = false;   // the previous window survives under <path>.1
    int  err      = 0;       // errno from the failed rename, when !archived
    // ck-otj9 (ck-zgh4): the ring was already FULL, so the eldest archive was
    // overwritten and the head of the run is gone. A bounded ring is still a
    // bound that discards, and a complete-looking set of archives whose oldest
    // does not begin at tick 1 reads exactly like full coverage. This is the
    // same defect the archiving fixed, one level up — so it gets the same
    // treatment: say it, do not let the file count imply coverage.
    bool eldest_dropped = false;
};

// Tracks WHERE the retained set actually begins, so a reader is told rather
// than left to infer it from how many files exist.
//
// Sized by the measurement that prompted it: a traced episode writes 158-266MB
// (two independent estimates from arma8 — 0.148 MB/tick from 45MB at tick 304,
// 0.088 MB/tick from 210s-apart snapshots, over ~1795 ticks). A 3-deep 64MB
// ring retains 192MB. At the low estimate that covers a run; at the high one
// the head falls off. Rather than pick a depth against an estimate that spans a
// factor of 1.7, the loss is REPORTED and the depth stays bounded.
struct LogCoverage {
    static constexpr int kMaxArchives = 8;
    int      archives  = 3;
    int      rotations = 0;
    // [0] is the live window; [i] is the window now at <path>.<i>.
    unsigned long long window_start[kMaxArchives + 1] = {0};

    void note_rotation(unsigned long long now_marker) {
        for (int i = (archives < kMaxArchives ? archives : kMaxArchives); i >= 1; --i)
            window_start[i] = window_start[i - 1];
        window_start[0] = now_marker;
        ++rotations;
    }
    // The marker at which the OLDEST SURVIVING window begins. Before the ring
    // fills this is the start of the run; after, it is where coverage now
    // starts and everything earlier is gone.
    unsigned long long earliest_retained() const {
        const int deepest = rotations < archives ? rotations : archives;
        return window_start[deepest];
    }
    bool head_of_run_lost() const { return rotations > archives; }
};

// Rename the ring down (.2 -> .3, .1 -> .2), move the live log to .1, then
// reopen it empty. `*fp` is updated in place. Returns what actually happened —
// never a bare bool, because "rotated" and "rotated WITHOUT keeping the old
// window" are different events and a caller must be able to say which.
inline RotateResult rotate_log(const char* path, FILE** fp,
                               long max_bytes, int archives) {
    RotateResult r;
    if (!path || !fp || !*fp) return r;
    const long sz = ftell(*fp);
    if (sz <= max_bytes) return r;
    r.rotated = true;

    char from[1200], to[1200];
    // Detect the overwrite BEFORE it happens: if the deepest slot is occupied,
    // the shift below destroys it.
    snprintf(to, sizeof to, "%s.%d", path, archives);
    r.eldest_dropped = (access(to, F_OK) == 0);

    for (int i = archives - 1; i >= 1; --i) {
        snprintf(from, sizeof from, "%s.%d", path, i);
        snprintf(to,   sizeof to,   "%s.%d", path, i + 1);
        rename(from, to);          // ENOENT expected until the ring fills
    }
    snprintf(to, sizeof to, "%s.1", path);
    errno = 0;
    r.archived = (rename(path, to) == 0);
    if (!r.archived) r.err = errno;

    FILE* nf = freopen(path, "w", *fp);
    if (nf) *fp = nf;
    return r;
}

} // namespace bridge
