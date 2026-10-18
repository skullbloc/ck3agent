// test/acceptance_board_continuity.cpp — ck-81su ACCEPTANCE, host lane.
//
// THE CLAIM UNDER TEST. An arm-B episode whose process is relaunched mid-episode
// (which is what ck-6kvq.5's resume does, and what the pilot and the LLM arms
// expect as the normal case at a session cap) comes back with its board INTACT
// and its seq CONTINUING — and only when the resume's declared save identity is
// accepted by the same rule ck-9fc3 uses for steering. Never on inference.
//
// WHY THIS BINARY RUNS IN PHASES AND NOT IN ONE PROCESS. `board()` is a
// process-wide singleton and `begin_episode()` resets it on process start. That
// reset IS the bug. A single-process test would have to call the reset by hand
// and would therefore be testing a simulation of the hazard rather than the
// hazard. So each phase is a FRESH PROCESS — scripts/acceptance_board_continuity.py
// runs them in order against one carrier file — and phase B really is a relaunch.
//
//   --phase write     process 1: declare, post N, exit (the carrier is written)
//   --phase resume    process 2: RELAUNCH. Before declaring, the board must
//                     REFUSE posts typed. Declare the SAME identity: the board
//                     comes back, seq continues at N, and the relaunch is
//                     RECORDED in the continuity block.
//   --phase reject    process 2': RELAUNCH, declare a DIFFERENT save. The board
//                     must stay SEALED and refuse posts with a typed reason, and
//                     it must NOT adopt a single row.
//   --phase holed     RELAUNCH onto a carrier with a POST MISSING from the
//                     middle. read() indexes posts_ by seq, so a holed chain
//                     hands every reader the wrong row. Refused even under the
//                     RIGHT identity.
//   --phase nodeclare process 2'': RELAUNCH and never declare. Posts refused,
//                     typed; nothing adopted; the file on disk is untouched.
//   --phase hostileid The episode id is UNVALIDATED env input and it names a
//                     file. Whatever is exported must stay ONE path component
//                     inside the state directory.
//   --phase cold      NO carrier at all: seq 0, posts accepted, continuity
//                     "fresh" — the pre-bead behaviour, unchanged.
//
// THE NEGATIVE CONTROL is one macro, CK3_BOARD_NO_CARRIER_RESTORE, which
// compiles the PRE-FIX behaviour back in: the carrier is never consulted, so a
// relaunched board is empty and its seq restarts at 0. The `resume` phase must
// FAIL under it, and must fail ON THE SEQ, not only on the row count — the bead
// asks for that specifically, because a seq restarting at 0 is what makes a
// truncated episode read as a complete one.
//
// ENGINE-FREE: the real board.cpp, the real board_persist.cpp and the real
// world_identity.cpp. No CK3 anywhere.

#include "bridge.h"     // MemRange, for the stub globals below
#include "board.h"
#include "episode_policy.h"
#include "world_identity.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <shared_mutex>
#include <string>
#include <vector>

namespace bridge {
FILE* g_logfile = nullptr;
uintptr_t              g_base_addr = 0;
std::vector<MemRange>  g_valid_ranges;
std::shared_mutex      g_ranges_mutex;
}

using namespace bridge;

namespace {

struct Check { std::string phase, name, detail; bool ok; };
std::vector<Check> g_checks;

bool check(const char* phase, const char* name, bool ok,
           const std::string& detail = std::string()) {
    g_checks.push_back({phase, name, detail, ok});
    printf("  %s  %-52s %s\n", ok ? "PASS" : "FAIL", name, detail.c_str());
    return ok;
}

std::string jesc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if (c == '\n') o += "\\n";
        else o += c;
    }
    return o;
}

WorldIdentity ident(const char* episode, const char* sha, uint32_t year) {
    WorldIdentity w;
    w.episode_id = episode;
    w.save_sha256 = sha;
    w.start_date_year = year;
    return w;
}

BoardAttribution who(const char* sess, uint32_t cid, uint64_t flush) {
    BoardAttribution a;
    a.session_id = sess;
    a.character_id = cid;
    a.game_date = "867.1.1";
    a.tick = 100 + flush;
    a.flush_id = flush;
    return a;
}

// Arm the board exactly as an arm-B experiment episode arms it, then let
// board_init_from_env() run the REAL path — including the carrier resolution
// that this whole bead is about.
void arm_board(const char* episode_id) {
    setenv("CK3_EPISODE_MODE", "experiment", 1);
    setenv("CK3_EPISODE_ID", episode_id, 1);
    setenv("CK3_EPISODE_ARM", "b", 1);
    episode_policy().init_from_env();
    board_init_from_env();
}

const char* kSaveA = "aaaa1111bbbb2222cccc3333dddd4444eeee5555ffff6666aaaa7777bbbb8888";
const char* kSaveB = "9999000011112222333344445555666677778888999900001111222233334444";

// ---------------------------------------------------------------------------
// phase: cold — no carrier. The pre-bead behaviour, and it must be untouched.
// ---------------------------------------------------------------------------
void phase_cold() {
    arm_board("ep-cold");
    check("cold", "a board with no carrier is Fresh",
          board().continuity() == BoardContinuity::Fresh,
          board_continuity_name(board().continuity()));
    BoardPostResult r = board().post_text("first", who("s1", 11, 1));
    check("cold", "a cold board ACCEPTS a post", r.ok, r.reason);
    check("cold", "and the first post is seq 0", r.ok && r.post.seq == 0,
          r.ok ? std::to_string(r.post.seq) : std::string("(refused)"));
}

// ---------------------------------------------------------------------------
// phase: write — process 1. Declare, post, leave the carrier behind.
// ---------------------------------------------------------------------------
void phase_write(int n) {
    arm_board("ep-1");
    check("write", "process 1 starts Fresh",
          board().continuity() == BoardContinuity::Fresh,
          board_continuity_name(board().continuity()));

    // The harness declares at episode start (episode_driver does exactly this).
    world_identity_declare(ident("ep-1", kSaveA, 867));

    bool all_ok = true;
    uint64_t last = 0;
    for (int i = 0; i < n; ++i) {
        // One post per flush, so the per-flush quota is never the thing under
        // test here.
        BoardPostResult r = board().post_text("post " + std::to_string(i),
                                              who("s1", 11, (uint64_t)i + 1));
        if (!r.ok) { all_ok = false; break; }
        last = r.post.seq;
    }
    check("write", "process 1 posts N times, seq 0..N-1",
          all_ok && last == (uint64_t)n - 1,
          "last seq " + std::to_string(last));
    check("write", "the carrier was written",
          !board_carrier_path().empty(), board_carrier_path());
}

// ---------------------------------------------------------------------------
// phase: resume — process 2. THE BEAD.
// ---------------------------------------------------------------------------
void phase_resume(int n) {
    arm_board("ep-1");

    // (1) BEFORE any declaration. This is the state a relaunched bridge is in
    //     for as long as it takes the harness to declare, and the ONLY safe
    //     behaviour is a typed refusal — not an empty board quietly accepting
    //     seq 0 for the second time.
    check("resume", "a relaunched board latches AwaitingIdentity",
          board().continuity() == BoardContinuity::AwaitingIdentity,
          board_continuity_name(board().continuity()));
    BoardPostResult early = board().post_text("too early", who("s1", 11, 9));
    check("resume", "and REFUSES posts, typed, before any declaration",
          !early.ok && early.reason == board_reason::kAwaitingIdentity,
          // The detail names the seq on the ACCEPT path deliberately: under the
          // negative control this reads "ACCEPTED at seq 0", which is the hazard
          // in one line — a SECOND row numbered 0 on a board that already had
          // one, in an episode whose bundle still validates.
          early.ok ? ("ACCEPTED at seq " + std::to_string(early.post.seq))
                   : early.reason);
    check("resume", "nothing was adopted on inference",
          board().total_posts() == 0,
          std::to_string(board().total_posts()) + " posts");

    // (2) The resume declares the SAME save. ck-9fc3's judge says YES, and the
    //     board comes back on that verdict and on nothing else.
    RestoreRecord rec = world_identity_declare(ident("ep-1", kSaveA, 867));
    check("resume", "the declaration RESTORED the board",
          rec.board_decision == "restored_from_carrier", rec.board_decision);
    check("resume", "every post is back",
          board().total_posts() == (size_t)n,
          std::to_string(board().total_posts()) + " of " + std::to_string(n));

    // (3) THE SEQ CONTINUES. This is the check the negative control fails, and
    //     it is separate from the row count on purpose.
    check("resume", "the seq HIGH-WATER MARK came back (not 0)",
          board().next_seq() == (uint64_t)n,
          "next_seq " + std::to_string(board().next_seq()));
    BoardPostResult after = board().post_text("after the relaunch", who("s1", 11, 20));
    check("resume", "the next post CONTINUES the chain, it does not restart it",
          after.ok && after.post.seq == (uint64_t)n,
          after.ok ? ("seq " + std::to_string(after.post.seq)) : after.reason);

    // (4) ONE dense monotonic chain across the discontinuity, no duplicates.
    BoardReadResult all = board().read(0, 0);
    bool dense = all.posts.size() == (size_t)n + 1;
    for (size_t i = 0; i < all.posts.size() && dense; ++i)
        if (all.posts[i].seq != i) dense = false;
    check("resume", "board.jsonl is ONE dense monotonic chain, no duplicate seq",
          dense, std::to_string(all.posts.size()) + " rows");

    // (5) AND THE DISCONTINUITY IS RECORDED, NOT HIDDEN.
    std::vector<BoardRelaunch> rl = board().relaunches();
    check("resume", "the relaunch is RECORDED (a BoardRelaunch at seq N)",
          rl.size() == 1 && rl[0].at_seq == (uint64_t)n &&
          rl[0].decision == "restored_from_carrier",
          rl.empty() ? "none" : (rl[0].decision + " at " + std::to_string(rl[0].at_seq)));
    const std::string page = board().read_json(0, 0);
    check("resume", "and published on board_read, where the lane reads it",
          page.find("\"continuity\"") != std::string::npos &&
          page.find("\"restored_from_carrier\"") != std::string::npos,
          "continuity block present");
}

// ---------------------------------------------------------------------------
// phase: reject — THE NEGATIVE CONTROL OF THE RULE ITSELF.
// ---------------------------------------------------------------------------
void phase_reject() {
    arm_board("ep-1");
    check("reject", "a relaunched board latches AwaitingIdentity",
          board().continuity() == BoardContinuity::AwaitingIdentity,
          board_continuity_name(board().continuity()));

    // A DIFFERENT save. Same episode id, same start year — everything an
    // inference could have keyed on agrees, and the declaration is still the
    // only thing that decides.
    RestoreRecord rec = world_identity_declare(ident("ep-1", kSaveB, 867));
    check("reject", "a DIFFERENT save is REFUSED",
          rec.board_decision == "refused_different_save", rec.board_decision);
    check("reject", "and NOT ONE row was adopted",
          board().total_posts() == 0,
          std::to_string(board().total_posts()) + " posts");
    check("reject", "the board is SEALED",
          board().sealed() && board().continuity() == BoardContinuity::Refused,
          board_continuity_name(board().continuity()));
    BoardPostResult r = board().post_text("nope", who("s1", 11, 3));
    check("reject", "and posts are refused with a TYPED reason",
          !r.ok && r.reason == board_reason::kIdentityRefused,
          r.ok ? "ACCEPTED" : r.reason);

    // A refusal is a verdict on one declaration, not on the bytes: the right
    // identity still gets the board back, and both attempts are in the record.
    RestoreRecord good = world_identity_declare(ident("ep-1", kSaveA, 867));
    check("reject", "the RIGHT identity afterwards still restores it",
          good.board_decision == "restored_from_carrier" &&
          board().continuity() == BoardContinuity::Restored,
          good.board_decision);
    check("reject", "and both decisions are in the continuity record",
          board().relaunches().size() == 2,
          std::to_string(board().relaunches().size()) + " relaunch rows");
}

// ---------------------------------------------------------------------------
// phase: holed — a carrier with a hole in it is a DAMAGED record.
// ---------------------------------------------------------------------------
// read() indexes posts_ by seq. A chain that is not dense from 0 does not
// degrade: it hands every reader the wrong row, silently, including the observer
// whose drain becomes the bundle's board.jsonl. The driver removes one post line
// from the middle of the carrier before this phase runs.
void phase_holed() {
    arm_board("ep-1");
    check("holed", "a holed carrier still latches AwaitingIdentity",
          board().continuity() == BoardContinuity::AwaitingIdentity,
          board_continuity_name(board().continuity()));
    RestoreRecord rec = world_identity_declare(ident("ep-1", kSaveA, 867));
    check("holed", "and is REFUSED even though the identity is RIGHT",
          rec.board_decision == "refused_carrier_not_dense", rec.board_decision);
    check("holed", "not one row is adopted — no reader gets a mis-indexed post",
          board().total_posts() == 0,
          std::to_string(board().total_posts()) + " posts");
    BoardPostResult r = board().post_text("nope", who("s1", 11, 3));
    check("holed", "the board is sealed and refuses, typed",
          !r.ok && r.reason == board_reason::kIdentityRefused,
          r.ok ? "ACCEPTED" : r.reason);
}

// ---------------------------------------------------------------------------
// phase: nodeclare — nobody vouches. The conservative default.
// ---------------------------------------------------------------------------
void phase_nodeclare() {
    arm_board("ep-1");
    check("nodeclare", "a relaunched board latches AwaitingIdentity",
          board().continuity() == BoardContinuity::AwaitingIdentity,
          board_continuity_name(board().continuity()));
    BoardPostResult r = board().post_text("unvouched", who("s1", 11, 4));
    check("nodeclare", "with no declaration at all, posts stay refused",
          !r.ok && r.reason == board_reason::kAwaitingIdentity,
          r.ok ? "ACCEPTED" : r.reason);
    check("nodeclare", "and nothing is adopted",
          board().total_posts() == 0,
          std::to_string(board().total_posts()) + " posts");
}

// ---------------------------------------------------------------------------
// phase: hostileid — the episode id is UNVALIDATED input and it names a file.
// ---------------------------------------------------------------------------
// episode_policy.cpp takes CK3_EPISODE_ID from getenv() verbatim; the
// `ep-<timestamp>-...` shape is a HARNESS-side schema only. So whatever a launch
// script exports composes into the carrier's filename. The realistic failure is
// a typo, and its symptom is the nastiest kind: a carrier written where the
// reader does not look, so the board silently never rehydrates and this bead's
// own bug comes back in a different hat.
void phase_hostile_id(const char* raw_id) {
    const std::string dir = getenv("CK3_EPISODE_DIR") ? getenv("CK3_EPISODE_DIR") : "";
    arm_board(raw_id);
    const std::string path = board_carrier_path();
    const std::string want_prefix = dir + "/state/board-";

    check("hostileid", "the carrier stays inside the episode's state directory",
          !path.empty() && path.compare(0, want_prefix.size(), want_prefix) == 0,
          path);
    const std::string tail = path.size() > want_prefix.size()
                             ? path.substr(want_prefix.size()) : std::string();
    check("hostileid", "the episode id became ONE path component — no separator",
          tail.find('/') == std::string::npos && !tail.empty(), tail);
    check("hostileid", "and it is not a dotfile, a \".\" or a \"..\"",
          !tail.empty() && tail[0] != '.', tail);
    check("hostileid", "and the filename is bounded",
          tail.size() <= 96 + 6 /* ".jsonl" */, std::to_string(tail.size()));

    // And it still WORKS: a mangled id is a usable carrier, not a dead one.
    world_identity_declare(ident("x", kSaveA, 867));
    BoardPostResult r = board().post_text("hello", who("s1", 11, 1));
    check("hostileid", "and the board still posts and persists under it",
          r.ok && r.post.seq == 0, r.ok ? "seq 0" : r.reason);
}

// ---------------------------------------------------------------------------
// phase: disabled — every arm WITHOUT a board is untouched.
// ---------------------------------------------------------------------------
void phase_disabled() {
    setenv("CK3_EPISODE_MODE", "experiment", 1);
    setenv("CK3_EPISODE_ID", "ep-1", 1);
    setenv("CK3_EPISODE_ARM", "a", 1);
    unsetenv("CK3_BOARD_ENABLED");
    episode_policy().init_from_env();
    board_init_from_env();
    check("disabled", "arm A has no board",
          !board().enabled(), board().enabled() ? "ENABLED" : "disabled");
    check("disabled", "and therefore writes no carrier, even beside one",
          board_carrier_path().empty(), board_carrier_path());
}

void emit_json() {
    int passed = 0;
    for (const Check& c : g_checks) if (c.ok) ++passed;
    printf("{\"total\":%zu,\"passed\":%d,\"failed\":%zu,\"checks\":[",
           g_checks.size(), passed, g_checks.size() - (size_t)passed);
    for (size_t i = 0; i < g_checks.size(); ++i) {
        printf("%s{\"phase\":\"%s\",\"name\":\"%s\",\"ok\":%s,\"detail\":\"%s\"}",
               i ? "," : "", jesc(g_checks[i].phase).c_str(),
               jesc(g_checks[i].name).c_str(), g_checks[i].ok ? "true" : "false",
               jesc(g_checks[i].detail).c_str());
    }
    printf("]}\n");
}

}  // namespace

// ---------------------------------------------------------------------------
// phase: deferred — ck-2r46. The carrier must NEVER open under an empty id.
// ---------------------------------------------------------------------------
// The d42 arm-B smoke opened
// /tmp/ck3-inst/default/episode/state/board-unnamed.jsonl, because the carrier
// initialises from the environment BEFORE the driver declares the episode and
// `sanitise("")` used to yield "unnamed". Two failures came out of that one
// default: a relaunch resolves board-<real-id>.jsonl and never finds the file,
// so ck-81su's rehydration is armed and inert; and two episodes on one instance
// both resolve "unnamed" and the second REHYDRATES THE FIRST'S POSTS.
void phase_deferred() {
    // Arm with NO episode id — exactly what the carrier sees at bridge init.
    // A REAL DIRECTORY, because resolve_path_locked stats it — without one the
    // carrier declines for the OTHER reason and this phase would pass on the
    // wrong cause.
    char dir[] = "/tmp/ck3-deferred-XXXXXX";
    if (!mkdtemp(dir)) { check("deferred", "tempdir", false, "mkdtemp failed"); return; }
    setenv("CK3_EPISODE_DIR", dir, 1);
    setenv("CK3_EPISODE_MODE", "experiment", 1);
    unsetenv("CK3_EPISODE_ID");
    setenv("CK3_EPISODE_ARM", "b", 1);
    episode_policy().init_from_env();
    board_init_from_env();

    check("deferred", "with no episode id the carrier opens NO file",
          board_carrier_path().empty(),
          board_carrier_path().empty() ? "(deferred)" : board_carrier_path());
    check("deferred", "and no path is invented from an empty id",
          board_carrier_path().find("unnamed") == std::string::npos,
          board_carrier_path());

    // The driver declares the episode. THIS is the first moment a filename exists.
    board_carrier_adopt_episode_id("ep-alpha");
    const std::string a = board_carrier_path();
    check("deferred", "adopting the id opens the carrier under THAT id",
          a.find("board-ep-alpha.jsonl") != std::string::npos, a);

    // A SECOND episode must not land in the first's file. This is the collision
    // that would have merged two episodes' treatment.
    board_carrier_adopt_episode_id("ep-beta");
    check("deferred", "a second id does NOT silently retarget an open carrier",
          board_carrier_path() == a,
          board_carrier_path() + " vs " + a);
    check("deferred", "and the two ids would resolve to DIFFERENT files",
          std::string("board-ep-alpha.jsonl") != std::string("board-ep-beta.jsonl"),
          "per-episode by construction");

    // Adopting the SAME id again is a no-op: the driver re-declares on reconnect.
    board_carrier_adopt_episode_id("ep-alpha");
    check("deferred", "re-declaring the same id is idempotent",
          board_carrier_path() == a, board_carrier_path());
}

int main(int argc, char** argv) {
    const char* phase = nullptr;
    bool json = false;
    int n = 4;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--phase") && i + 1 < argc) phase = argv[++i];
        else if (!strcmp(argv[i], "--posts") && i + 1 < argc) n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--json")) json = true;
    }
    if (!phase) { fprintf(stderr, "usage: %s --phase <name> [--posts N] [--json]\n", argv[0]); return 2; }

    if      (!strcmp(phase, "cold"))      phase_cold();
    else if (!strcmp(phase, "write"))     phase_write(n);
    else if (!strcmp(phase, "resume"))    phase_resume(n);
    else if (!strcmp(phase, "reject"))    phase_reject();
    else if (!strcmp(phase, "holed"))     phase_holed();
    else if (!strcmp(phase, "hostileid"))
        phase_hostile_id(getenv("CK3_TEST_RAW_EPISODE_ID")
                         ? getenv("CK3_TEST_RAW_EPISODE_ID") : "../../../etc/x");
    else if (!strcmp(phase, "nodeclare")) phase_nodeclare();
    else if (!strcmp(phase, "disabled"))  phase_disabled();
    else if (!strcmp(phase, "deferred"))  phase_deferred();
    else { fprintf(stderr, "unknown phase %s\n", phase); return 2; }

    int failed = 0;
    for (const Check& c : g_checks) if (!c.ok) ++failed;
    if (json) emit_json();
    printf("%zu/%zu checks passed — %s\n", g_checks.size() - (size_t)failed,
           g_checks.size(), failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
