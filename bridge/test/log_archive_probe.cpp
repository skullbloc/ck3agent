// ck-bwz1 — every rotated window is archived, and the set TILES.
//
// Drives the real archiver against real files in a temp directory, the way
// ck-otj9's probe drives rotate_log — a grep cannot tell a gap from a
// complete set, and counting files is exactly the inference this bead refuses.
#include "log_archive.h"
#include "log_rotate.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/stat.h>

static int failures = 0;
static void row(const char* n, bool ok, const std::string& d) {
    printf("%-54s %s  %s\n", n, ok ? "PASS" : "FAIL", d.c_str());
    if (!ok) failures++;
}

int main() {
    char tmpl[] = "/tmp/ck3_bwz1_XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) { printf("NOT EVALUABLE: mkdtemp failed\n"); return 3; }
    const std::string arc = std::string(dir) + "/archive";
    ::mkdir(arc.c_str(), 0755);

    // ── naming carries the range, so coverage is readable from a listing ──
    char b[256];
    bridge::archive_basename(b, sizeof b, "bridge", 0, 430);
    row("the_name_carries_the_tick_range", strcmp(b, "bridge.t0-t430.log.gz") == 0, b);
    unsigned long long s = 9, e = 9;
    row("the_range_round_trips_out_of_the_name",
        bridge::parse_archive_basename(b, &s, &e) && s == 0 && e == 430,
        std::to_string(s) + ".." + std::to_string(e));
    row("a_foreign_filename_is_NOT_given_a_range",
        !bridge::parse_archive_basename("bridge.log.1", &s, &e),
        "guessing a range for a file we did not write would manufacture coverage");
    row("a_backwards_range_is_refused",
        !bridge::parse_archive_basename("bridge.t900-t100.log.gz", &s, &e),
        "a window cannot end before it starts");

    // ── CONTIGUITY is the acceptance ──
    bridge::ArchiveWindow w[4];
    w[0] = {0, 430, ""}; w[1] = {430, 860, ""}; w[2] = {860, 1290, ""};
    row("a_tiling_set_has_no_gap", bridge::first_gap(w, 3) == -1, "[0,430)[430,860)[860,1290)");
    w[2] = {900, 1290, ""};
    row("a_GAP_is_found_and_located", bridge::first_gap(w, 3) == 2,
        "860..900 is missing and the set must not read as complete");
    w[2] = {800, 1290, ""};
    row("an_OVERLAP_is_also_a_failure", bridge::first_gap(w, 3) == 2,
        "two files claiming the same ticks cannot be replayed in order");
    // THE DEFECT ITSELF: three files that look like a set but start late.
    bridge::ArchiveWindow ring[3] = {{1290, 1720, ""}, {1720, 2150, ""}, {2150, 2580, ""}};
    row("a_late_starting_set_is_internally_contiguous",
        bridge::first_gap(ring, 3) == -1,
        "which is why contiguity ALONE is not coverage — the start must be checked too");

    // ── the queue: fixed, and a drop is counted, never silent ──
    bridge::ArchiveQueue q;
    bridge::ArchiveWindow one{0, 1, ""};
    int pushed = 0;
    while (q.push(one)) ++pushed;
    row("the_queue_is_bounded_and_says_when_it_drops",
        pushed == bridge::ArchiveQueue::kSlots - 1 && q.dropped == 1,
        "pushed=" + std::to_string(pushed) + " dropped=" + std::to_string(q.dropped));

    // ── the real thing: compress an actual retired window ──
    const std::string src = std::string(dir) + "/bridge.log.1";
    {
        FILE* f = fopen(src.c_str(), "w");
        if (!f) { printf("NOT EVALUABLE: cannot write the fixture\n"); return 3; }
        for (int i = 0; i < 20000; ++i) fprintf(f, "DIAG tick=%d filler line\n", i);
        fclose(f);
    }
    bridge::ArchiveQueue rq;
    bridge::ArchiveWindow rw{0, 430, ""};
    snprintf(rw.src, sizeof rw.src, "%s", src.c_str());
    rq.push(rw);
    for (int i = 0; i < 400 && (rq.archived + rq.failed) == 0; ++i) {
        bridge::archive_pump(rq, arc.c_str());
        usleep(10000);
    }
    const std::string out = arc + "/bridge.t0-t430.log.gz";
    struct stat st{};
    const bool exists = (::stat(out.c_str(), &st) == 0);
    row("a_real_window_is_compressed_into_the_archive",
        rq.archived == 1 && exists && st.st_size > 0,
        "archived=" + std::to_string(rq.archived) +
        " bytes=" + std::to_string((long)st.st_size));
    struct stat ss{};
    ::stat(src.c_str(), &ss);
    row("it_is_actually_COMPRESSED",
        exists && ss.st_size > 0 && st.st_size < ss.st_size / 2,
        std::to_string((long)st.st_size) + " vs " + std::to_string((long)ss.st_size) + " raw");
    row("the_source_window_is_LEFT_IN_PLACE",
        ::access(src.c_str(), R_OK) == 0,
        "the ring still owns /tmp retention; archiving must not disturb it");
    // No half-written archive may sit under a name that claims a range.
    const std::string part = out + ".part";
    row("no_part_file_is_left_behind",
        ::access(part.c_str(), F_OK) != 0,
        "a reader must never see an incomplete file under a complete name");

    // ── archiving not configured: the window STAYS QUEUED, is not dropped ──
    bridge::ArchiveQueue nq;
    nq.push(rw);
    bridge::archive_pump(nq, "");
    row("an_unconfigured_archive_dir_does_not_discard_the_window",
        nq.depth() == 1 && nq.archived == 0 && nq.failed == 0,
        "depth=" + std::to_string(nq.depth()) + " — not configured is not a failure");

    // ── END TO END: N rotations, N archives, and the set must TILE FROM 0 ──
    // This is the acceptance bt-bbd specified. It is deliberately driven
    // through the REAL rotate_log, so the tick ranges come from the same
    // bookkeeping the bridge uses rather than from numbers this test invents.
    {
        const std::string live = std::string(dir) + "/run.log";
        const std::string arc2 = std::string(dir) + "/archive2";
        ::mkdir(arc2.c_str(), 0755);
        FILE* lf = fopen(live.c_str(), "w");
        bridge::LogCoverage cov;
        cov.archives = 3;
        bridge::ArchiveQueue q2;
        const int kRot = 5;
        const unsigned long long kStep = 430;
        bool all_started = true;

        for (int r = 1; r <= kRot; ++r) {
            for (int i = 0; i < 4000; ++i) fprintf(lf, "DIAG r=%d i=%d filler\n", r, i);
            fflush(lf);
            const unsigned long long win_start = cov.window_start[0];
            const unsigned long long now = kStep * r;
            // max_bytes 0 so every call rotates; the size policy is ck-otj9's.
            bridge::RotateResult rr = bridge::rotate_log(live.c_str(), &lf, 0, 3);
            if (!rr.archived) { all_started = false; break; }
            cov.note_rotation(now);
            bridge::ArchiveWindow aw;
            aw.start_tick = win_start;
            aw.end_tick   = now;
            snprintf(aw.src, sizeof aw.src, "%s.1", live.c_str());
            q2.push(aw);
            // Drain to completion before the next rotation shifts the ring.
            for (int k = 0; k < 400 && !q2.empty(); ++k) {
                bridge::archive_pump(q2, arc2.c_str());
                usleep(5000);
            }
            for (int k = 0; k < 400 && bridge::archive_busy(); ++k) {
                bridge::archive_pump(q2, arc2.c_str());
                usleep(5000);
            }
        }
        if (lf) fclose(lf);

        // Read the ranges back OUT OF THE DIRECTORY, as a reader would.
        bridge::ArchiveWindow found[16];
        size_t nf = 0;
        for (int r = 1; r <= kRot && nf < 16; ++r) {
            char nb[256];
            bridge::archive_basename(nb, sizeof nb, "bridge",
                                     kStep * (r - 1) == 0 && r == 1 ? 0 : kStep * (r - 1),
                                     kStep * r);
            const std::string f = arc2 + "/" + nb;
            unsigned long long a = 0, bb = 0;
            if (::access(f.c_str(), R_OK) == 0 &&
                bridge::parse_archive_basename(nb, &a, &bb)) {
                found[nf].start_tick = a; found[nf].end_tick = bb; ++nf;
            }
        }
        row("every_rotation_produced_an_archive",
            all_started && nf == (size_t)kRot && q2.archived == (unsigned long long)kRot,
            std::to_string(nf) + " of " + std::to_string(kRot) +
            " windows on disk, archived=" + std::to_string(q2.archived));
        row("the_archived_windows_TILE_with_no_gap",
            nf >= 2 && bridge::first_gap(found, nf) == -1,
            "contiguous across " + std::to_string(nf) + " windows");
        row("coverage_begins_at_the_START_OF_THE_RUN",
            nf > 0 && found[0].start_tick == 0,
            "first window starts at t" + std::to_string(nf ? found[0].start_tick : 999) +
            " — a contiguous set that starts late is the ck-otj9 defect over again");
        row("the_archive_outlives_the_THREE_slot_ring",
            nf > 3,
            std::to_string(nf) + " archived vs a ring that retains 3 — "
            "this is the whole bead");
    }

    char cb[256];
    bridge::format_archive_counters(cb, sizeof cb, rq);
    row("the_counters_state_every_outcome",
        strstr(cb, "log_archive_pushed=1") && strstr(cb, "log_archive_done=1") &&
        strstr(cb, "log_archive_dropped=0"), cb);

    ::unlink(src.c_str()); ::unlink(out.c_str());
    ::rmdir(arc.c_str()); ::rmdir(dir);
    printf("\n%s (%d failing row%s)\n", failures ? "FAIL" : "PASS", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
