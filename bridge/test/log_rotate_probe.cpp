// ck-otj9 — does rotation KEEP the previous window, and say so when it cannot?
//
// The old code called freopen(path, "w") on the live FILE*: same inode,
// truncated, nothing retained. A test could only have grepped for that, and a
// grep cannot tell a rename from a truncate. rotate_log() is a function so this
// can drive it against real files.
//
// Exit 0 pass / 1 FAIL / 3 NOT EVALUABLE.
#include "log_rotate.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
static void row(const char* name, bool ok, const char* detail) {
    printf("%-46s %s  %s\n", name, ok ? "PASS" : "FAIL", detail);
    if (!ok) failures++;
}

static std::string slurp(const std::string& p) {
    FILE* f = fopen(p.c_str(), "r");
    if (!f) return "<absent>";
    char b[4096]; size_t n = fread(b, 1, sizeof b - 1, f); b[n] = 0; fclose(f);
    return std::string(b);
}
static bool exists(const std::string& p) { return access(p.c_str(), F_OK) == 0; }

// Remove exactly what this probe created — named files, then the two
// directories. NOT a recursive delete: the path comes from mkdtemp and is
// almost certainly fine, but a test that cleans up with `rm -rf $dir` is one
// bad expansion away from deleting something else, and nothing here needs that
// power.
static void cleanup(const std::string& dir) {
    const std::string log = dir + "/bridge.log";
    unlink(log.c_str());
    for (int i = 1; i <= 4; ++i) unlink((log + "." + std::to_string(i)).c_str());
    const std::string rolog = dir + "/ro/bridge.log";
    for (int i = 1; i <= 4; ++i) unlink((rolog + "." + std::to_string(i)).c_str());
    unlink(rolog.c_str());
    rmdir((dir + "/ro").c_str());
    rmdir(dir.c_str());
}

int main() {
    // Respect TMPDIR rather than hardcoding /tmp, so a sandboxed or
    // scratchpad-scoped run puts its litter where that run expects it.
    const char* tmp = getenv("TMPDIR");
    std::string tmpl = std::string(tmp && *tmp ? tmp : "/tmp") + "/ck3-otj9-XXXXXX";
    std::vector<char> dirbuf(tmpl.begin(), tmpl.end());
    dirbuf.push_back('\0');
    if (!mkdtemp(dirbuf.data())) { printf("NOT EVALUABLE: mkdtemp failed\n"); return 3; }
    const std::string dir(dirbuf.data());
    const std::string path = dir + "/bridge.log";

    FILE* fp = fopen(path.c_str(), "w");
    if (!fp) { printf("NOT EVALUABLE: cannot create the log\n"); cleanup(dir); return 3; }

    // 1. UNDER the cap: nothing happens. A rotation that fires early would
    //    shred a healthy log, so this is the guard on the guard.
    fputs("first window\n", fp); fflush(fp);
    bridge::RotateResult r = bridge::rotate_log(path.c_str(), &fp, 1000000, 3);
    row("under_cap_does_not_rotate", !r.rotated && !exists(path + ".1"),
        "small log left alone");

    // 2. OVER the cap: the previous window SURVIVES, and the live file is empty.
    //    This is the whole bead — the old code failed exactly here.
    r = bridge::rotate_log(path.c_str(), &fp, 4, 3);
    row("over_cap_rotates", r.rotated, "threshold crossed");
    row("previous_window_is_ARCHIVED_not_truncated",
        r.archived && slurp(path + ".1").find("first window") != std::string::npos,
        "the old bytes are at .1 — freopen(\"w\") alone lost them forever");
    row("live_log_is_empty_after_rotation", slurp(path) == "",
        "writing continues into a fresh file");

    // 2b. rc2 B1: the fresh file is 0600 whatever the umask (freopen took the
    //     umask: 0666 here), the FILE* other threads hold is the same object,
    //     and writes land in the NEW file, not the archive.
    {
        const mode_t old_umask = umask(0);
        FILE* before = fp;
        fputs("second window\n", fp); fflush(fp);
        bridge::rotate_log(path.c_str(), &fp, 4, 3);
        umask(old_umask);
        struct stat st;
        row("fresh_log_is_0600_not_umask",
            stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600,
            "reopened with an explicit 0600, not freopen's umask-derived mode");
        row("fresh_log_keeps_the_FILE_pointer", fp == before,
            "dup2 onto the live fd; a reopened FILE* would dangle in other threads");
        fputs("third\n", fp); fflush(fp);
        row("writes_land_in_the_fresh_file",
            slurp(path) == "third\n" &&
            slurp(path + ".1").find("third") == std::string::npos,
            "the cached offset of the old file is dropped");
        // Put the ring back the way section 3 expects: .1 = first window.
        rename((path + ".2").c_str(), (path + ".1").c_str());
        fclose(fp);
        fp = fopen(path.c_str(), "w");
        if (!fp) { printf("NOT EVALUABLE: cannot recreate the log\n"); cleanup(dir); return 3; }
    }

    // 3. THE RING SHIFTS THE RIGHT WAY. .1 must be the NEWEST archive. A ring
    //    shifted the wrong way silently overwrites the newest with the oldest —
    //    and would still pass rows 2 and 4.
    fputs("second window\n", fp); fflush(fp);
    r = bridge::rotate_log(path.c_str(), &fp, 4, 3);
    row("newest_archive_is_dot_one",
        slurp(path + ".1").find("second window") != std::string::npos,
        ".1 holds the most recent window");
    row("older_window_shifted_to_dot_two",
        slurp(path + ".2").find("first window") != std::string::npos,
        ".2 holds the one before it");

    // 4. THE RING IS BOUNDED — disk safety was the original reason for the cap,
    //    and archiving must not trade a bounded log for an unbounded directory.
    fputs("third window\n", fp);  fflush(fp);
    bridge::rotate_log(path.c_str(), &fp, 4, 3);
    fputs("fourth window\n", fp); fflush(fp);
    bridge::rotate_log(path.c_str(), &fp, 4, 3);
    row("ring_is_bounded_at_the_archive_count",
        exists(path + ".3") && !exists(path + ".4"),
        "3 archives kept, the eldest falls off");

    // 4b. ck-zgh4 — THE RING'S OWN TAIL LOSS MUST BE LEGIBLE. A full set of
    //     archives whose oldest does not begin at tick 1 reads exactly like
    //     complete coverage. The count of files cannot tell you; only a
    //     recorded start marker can.
    row("eldest_drop_is_flagged_when_ring_is_full",
        ({ fputs("fifth window\n", fp); fflush(fp);
           bridge::RotateResult rd = bridge::rotate_log(path.c_str(), &fp, 4, 3);
           rd.eldest_dropped; }),
        "the 4th+ rotation reports that it overwrote .3");
    {
        // The FIRST rotations must NOT claim a drop — otherwise the flag is
        // always true and says nothing.
        const std::string d2 = dir + "/second";
        mkdir(d2.c_str(), 0755);
        const std::string p2 = d2 + "/bridge.log";
        FILE* f2 = fopen(p2.c_str(), "w");
        if (!f2) { printf("NOT EVALUABLE: cannot create the second log\n");
                   cleanup(dir); return 3; }
        fputs("w1\n", f2); fflush(f2);
        bridge::RotateResult a = bridge::rotate_log(p2.c_str(), &f2, 2, 3);
        row("first_rotation_claims_no_drop", !a.eldest_dropped,
            "ring not yet full -> nothing was discarded");

        // A rotation at tick T ENDS the live window and archives it; that
        // window BEGAN at the previous rotation's tick, or at 0 for the first.
        // So rotations at 100/200/300 leave archives covering [200,300),
        // [100,200) and [0,100) — the earliest retained window begins at 0, the
        // start of the run. My first draft of these two rows asserted 100 and
        // 200, one window off, and the probe disagreed with the code. The CODE
        // was right; I fixed the expectations after re-deriving what a rotation
        // marker means, not by adjusting whichever side was easier.
        bridge::LogCoverage cov; cov.archives = 3;
        cov.note_rotation(100); cov.note_rotation(200); cov.note_rotation(300);
        row("coverage_starts_at_run_start_while_ring_fills",
            cov.earliest_retained() == 0 && !cov.head_of_run_lost(),
            "3 rotations, 3 archives -> the oldest retained window began at 0");
        cov.note_rotation(400);
        row("coverage_start_ADVANCES_when_head_is_lost",
            cov.earliest_retained() == 100 && cov.head_of_run_lost(),
            "4th rotation -> the [0,100) window is gone; coverage starts at 100");
        cov.note_rotation(500);
        row("coverage_start_keeps_advancing",
            cov.earliest_retained() == 200 && cov.head_of_run_lost(),
            "5th -> starts at 200; the reported start tracks the real loss");

        fclose(f2);
        unlink(p2.c_str());
        for (int i = 1; i <= 4; ++i) unlink((p2 + "." + std::to_string(i)).c_str());
        rmdir(d2.c_str());
    }

    // 5. A FAILED ARCHIVE MUST SAY SO. If rename cannot run, the caller has
    //    truncated without keeping anything — and must not report success, or a
    //    destroyed window looks like a retained one (the ck-duv5 rule).
    fclose(fp);
    const std::string rodir = std::string(dir) + "/ro";
    mkdir(rodir.c_str(), 0755);
    const std::string ropath = rodir + "/bridge.log";
    FILE* rf = fopen(ropath.c_str(), "w");
    if (!rf) { printf("NOT EVALUABLE: cannot create the read-only-dir log\n");
               chmod(rodir.c_str(), 0755); cleanup(dir); return 3; }
    fputs("doomed window\n", rf); fflush(rf);
    if (chmod(rodir.c_str(), 0500) != 0) {
        printf("NOT EVALUABLE: chmod failed; cannot exercise the rename-failure path\n");
        fclose(rf); cleanup(dir); return 3;
    }
    // WHETHER THIS PATH IS EXERCISABLE IS DECIDED BY geteuid(), NOT BY THE
    // RESULT. Inferring "must be root" from rr.archived being true makes a
    // mutant that SWALLOWS the failure indistinguishable from a legitimate
    // root run — it returned NOT EVALUABLE instead of a named red row, which is
    // non-zero and so still caught, but names nothing. A precondition must be
    // established independently of the outcome it gates, or the outcome can
    // excuse itself.
    const bool is_root = (geteuid() == 0);
    bridge::RotateResult rr = bridge::rotate_log(ropath.c_str(), &rf, 4, 3);
    chmod(rodir.c_str(), 0755);
    if (is_root) {
        printf("NOT EVALUABLE: running as root — rename succeeds in a 0500 dir, "
               "so the archive-failure path cannot be exercised here\n");
        fclose(rf); cleanup(dir); return 3;
    }
    row("failed_archive_is_reported_not_swallowed",
        rr.rotated && !rr.archived && rr.err != 0,
        rr.archived ? "archived=true in an UNWRITABLE dir — a discarded window "
                      "is being reported as a kept one"
                    : "rotated=true archived=false with an errno — the caller "
                      "can say the window was DISCARDED, not claim it was kept");

    fclose(rf);
    cleanup(dir);
    printf("\n%s (%d failing row%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
