// log_archive.cpp — ck-bwz1. The mechanism; the policy is in log_archive.h.
//
// Runs on the ACCEPT THREAD, never the game thread. See the header for why.
#include "log_archive.h"

#include <cerrno>
#include <cstdlib>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace bridge {

namespace {

//: The child currently compressing a window, and which window it is. ONE at a
//: time on purpose: rotations are ~430 ticks apart and a window sits in the
//: ring for over a thousand, so there is no throughput problem to solve, and a
//: single child means no pid table to leak.
pid_t       g_gzip_pid = -1;
char        g_gzip_dst[768];
ArchiveWindow g_gzip_win;

}  // namespace

// Start compressing `w` into `dir`. Returns false when it could not be
// started; the caller counts that rather than assuming the window is safe.
//
// posix_spawn, NOT fork+exec and NOT popen with a shell:
//   - fork() in this process copies the page tables of a multi-gigabyte game;
//     posix_spawn uses the vfork-like path and does not.
//   - a shell would mean quoting a path we did not choose. There is no shell
//     here, so there is nothing to quote wrong.
// gzip reads the source itself and we redirect its stdout, so the bridge never
// copies 64MB through its own address space.
bool archive_window_begin(const ArchiveWindow& w, const char* dir) {
    if (!dir || !dir[0] || g_gzip_pid > 0) return false;
    if (w.src[0] == '\0') return false;
    if (::access(w.src, R_OK) != 0) return false;   // already fell off the ring

    char base[256];
    archive_basename(base, sizeof base, "bridge", w.start_tick, w.end_tick);
    snprintf(g_gzip_dst, sizeof g_gzip_dst, "%s/%s", dir, base);

    posix_spawn_file_actions_t fa;
    if (posix_spawn_file_actions_init(&fa) != 0) return false;
    // Write to a .part name first: a reader must never see a half-written
    // archive under a name that claims a complete tick range. Renamed on exit.
    char part[832];
    snprintf(part, sizeof part, "%s.part", g_gzip_dst);
    posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, part,
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);

    char  a0[] = "gzip";
    char  a1[] = "-c";
    char* argv[] = {a0, a1, const_cast<char*>(w.src), nullptr};
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, "gzip", &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0 || pid <= 0) { ::unlink(part); return false; }

    g_gzip_pid = pid;
    g_gzip_win = w;
    return true;
}

// Reap without blocking. Returns 1 archived, -1 failed, 0 still running/none.
int archive_window_reap() {
    if (g_gzip_pid <= 0) return 0;
    int status = 0;
    const pid_t r = ::waitpid(g_gzip_pid, &status, WNOHANG);
    if (r == 0) return 0;                       // still compressing
    g_gzip_pid = -1;

    char part[832];
    snprintf(part, sizeof part, "%s.part", g_gzip_dst);
    const bool ok = (r > 0) && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok) { ::unlink(part); return -1; }

    // Only now does the file get the name that asserts its tick range.
    if (::rename(part, g_gzip_dst) != 0) { ::unlink(part); return -1; }
    return 1;
}

bool archive_busy() { return g_gzip_pid > 0; }

// Drain one step. Called from the accept thread's periodic wake; does at most
// one spawn and one reap per call, so a wake is bounded work.
void archive_pump(ArchiveQueue& q, const char* dir) {
    const int done = archive_window_reap();
    if (done == 1) q.archived.fetch_add(1, std::memory_order_relaxed);
    else if (done == -1) q.failed.fetch_add(1, std::memory_order_relaxed);

    if (archive_busy()) return;
    if (!dir || !dir[0]) return;        // archiving not configured: leave queued
    ArchiveWindow w;
    if (!q.pop(&w)) return;
    if (!archive_window_begin(w, dir)) q.failed.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace bridge
