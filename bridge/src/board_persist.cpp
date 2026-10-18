// board_persist.cpp — THE BOARD'S CARRIER (ck-81su).
//
// WHAT THIS FIXES. `EpisodeBoard::begin_episode()` resets seq to 0 on process
// start. ck-6kvq.5's resume relaunches CK3 in the middle of an episode, so an
// arm-B episode used to come back from a resume with an EMPTY board and a seq
// that restarted at 0. The board IS arm B's treatment — arms A and B are
// specified to differ ONLY in the channel — so that is not a short log file: it
// is an episode whose record of the independent variable was silently truncated,
// in a bundle that still validated, with a monotonic sequence that read as
// intact right across the discontinuity.
//
// WHAT THIS FILE IS. The posts and the seq high-water mark, appended to a file
// in the EPISODE DIRECTORY, so they outlive the process the way ck-9fc3's
// personality-lock carrier outlives it — and stamped with the same declared save
// identity, epoch and all, for the same reason.
//
// WHAT IT DELIBERATELY IS NOT. It is not a second notion of "the same save". It
// holds no rule. The verdict is taken once, by ck-9fc3's judge_same_save(), at
// the one site that already takes it for the personality locks
// (world_identity_declare()), and this file is handed the answer. A board that
// worked out for itself whether a resume was legitimate would be a second
// authority on the question the whole ck-9fc3 bead exists to make singular.
//
// NEVER ON INFERENCE. On process start a carrier is FOUND, not adopted. The
// board latches AwaitingIdentity and refuses every post with a typed reason
// until a declaration resolves it. The failure mode if a declaration never
// arrives is a loud refusal and an intact file on disk — never a second chain of
// seqs starting at 0.
//
// PURE. No engine dependency: stdio, the environment, and two mutexes. So
// bridge/test/acceptance_board_continuity proves the whole mechanism, negative
// control included, on a host with no CK3 anywhere.
//
// LOCK ORDER, stated once. EpisodeBoard::post_text() holds the board mutex and
// calls board_carrier_append_post(). So g_carrier_mu is a LEAF: nothing in this
// file may call into EpisodeBoard while holding it. board_carrier_accept() obeys
// that by reading the file, RELEASING the mutex, and only then adopting.

#include "board.h"
#include "episode_policy.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace bridge {

// Declared in bridge.h; redeclared so this TU stays engine-free.
extern FILE* g_logfile;

#define BLOG(fmt, ...) do { \
    if (g_logfile) { fprintf(g_logfile, "[CK3-Bridge][board] " fmt, ##__VA_ARGS__); fflush(g_logfile); } \
} while(0)

namespace {

std::mutex  g_carrier_mu CK3_EARLY_INIT;
std::string g_path CK3_EARLY_INIT;          // "" == persistence off
bool        g_path_overridden = false;

// What board_carrier_init_from_env() found, held so the declaration site can be
// handed it without re-reading the file.
bool          g_pending = false;
WorldIdentity g_pending_identity CK3_EARLY_INIT;
size_t        g_pending_posts = 0;
uint64_t      g_pending_next_seq = 0;

// ── ck-hzp7: THE CARRIER'S OWN RECORD OF WHAT IT FAILED TO WRITE ────────────
// STICKY, IN MEMORY, AND DELIBERATELY NOT ON DISK. The failure this exists to
// report is A DISK THAT WILL NOT TAKE WRITES, so an error channel that writes to
// disk shares a cause with the thing it is reporting — BLOG below is the same
// class of unchecked write to the same device. The only channel that survives
// the failure is one that needs nothing of the failing resource, so the state
// lives here and is published over stats_json on the existing socket.
//
// STICKY because a carrier that lost one record is incomplete for the rest of
// the episode: a later successful append does not restore the missing one, and a
// flag that cleared would make the artefact's last word the most optimistic one.
bool        g_degraded = false;
uint64_t    g_records_dropped = 0;
std::string g_degraded_reason CK3_EARLY_INIT;   // strerror of the FIRST failure

// An episode id becomes part of a FILENAME, so it is sanitised to ONE path
// COMPONENT before it gets there.
//
// WHY THIS IS NOT PARANOIA. The bridge does not validate CK3_EPISODE_ID at all —
// episode_policy.cpp takes getenv() verbatim, and the `ep-<timestamp>-...` shape
// is enforced by scripts/schemas/run_summary.schema.json on the HARNESS side
// only. So whatever a launch script exports lands here. The realistic failure is
// a typo, not an attack, and its symptom would be the nastiest kind: a carrier
// written somewhere the reader does not look, so the board silently never
// rehydrates and the bead's own bug comes back wearing a different hat.
//
// THE RULE: every byte outside [A-Za-z0-9-_.] becomes '_', so no separator and
// no NUL can survive and the result cannot be anything but a single component;
// a leading '.' is rewritten so the carrier can never be a dotfile or the names
// "." / ".."; and the length is bounded, because a filename is not unbounded and
// a silent ENAMETOOLONG is the same silent failure again.
//
// A collision between two sanitised ids is not a hazard: they would also be two
// episodes sharing one episode directory, which judge_same_save()'s same-episode
// clause refuses independently. (Raised by the ck-6kvq.5 author, who noticed that
// this is surface nothing upstream guards.)
constexpr size_t kMaxIdComponent = 96;

std::string sanitise(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        out += ok ? c : '_';
        if (out.size() >= kMaxIdComponent) break;
    }
    // ck-2r46: NO INVENTED NAME. This used to default an empty id to
    // "unnamed", and the d42 smoke opened
    // /tmp/ck3-inst/default/episode/state/board-unnamed.jsonl — because the
    // carrier initialises from the environment BEFORE the driver declares the
    // episode, so `episode_policy().get().episode_id` is empty at that moment.
    //
    // TWO FAILURES CAME OUT OF THAT ONE DEFAULT. A relaunch resolves
    // board-<real-id>.jsonl and never finds board-unnamed.jsonl, so ck-81su's
    // rehydration is armed and inert; and two episodes on one instance both
    // resolve "unnamed" and the second REHYDRATES THE FIRST'S POSTS into its
    // own board — cross-episode contamination of the manipulation.
    //
    // An empty id now yields an empty component, and `resolve_path_locked`
    // turns that into no path at all. The carrier waits.
    if (out.empty()) return std::string();
    if (out[0] == '.') out[0] = '_';   // no dotfile, no "." , no ".."
    return out;
}

// The episode directory, resolved EXACTLY as episode_abort.sh and session.cpp
// resolve it. No third spelling. Empty when there is no episode.
std::string episode_dir() {
    if (const char* d = getenv("CK3_EPISODE_DIR"); d && d[0]) return d;
    if (const char* inst = getenv("CK3_INSTANCE_DIR"); inst && inst[0])
        return std::string(inst) + "/episode";
    return std::string();
}

// ---------------------------------------------------------------------------
// The carrier's own shape — PRIVATE to the bridge.
// ---------------------------------------------------------------------------
// One JSON object per line, discriminated by "r":
//
//   {"r":"open",    "episode_id":..,"pid":N}
//   {"r":"identity","episode_id":..,"save_sha256":..,"start_date_year":N,
//                   "epoch":N,"at_seq":N}
//   {"r":"post",    <the wire shape of a post, verbatim>}
//   {"r":"relaunch",<a BoardRelaunch>}
//
// The LAST identity record wins: it is the identity most recently declared for
// the world these posts were written in. No identity record at all means nobody
// ever vouched for them, which judge_same_save() answers with
// NO_RECORDED_IDENTITY — the conservative default, and the same one a headerless
// personality lock file gets.
//
// The bundle's board.jsonl is still rendered from the LIVE board by the meter
// lane. This file is the bridge's private durability, not a second public shape.

// Deliberately a scanner and not a parser: this file is written by this file.
// A stricter reader here would only move the failure.
bool scan_string(const char* json, const char* key, std::string* out) {
    std::string pat = std::string("\"") + key + "\"";
    const char* k = strstr(json, pat.c_str());
    if (!k) return false;
    const char* c = strchr(k + pat.size(), ':');
    if (!c) return false;
    c++;
    while (*c == ' ' || *c == '\t') c++;
    if (*c != '"') return false;
    c++;
    out->clear();
    while (*c && *c != '"') {
        if (*c == '\\' && c[1]) {
            c++;
            switch (*c) {
                case 'n': *out += '\n'; break;
                case 't': *out += '\t'; break;
                case 'r': *out += '\r'; break;
                case 'u': {
                    // Only the \u00XX forms this file ever writes (control bytes).
                    if (c[1] && c[2] && c[3] && c[4]) {
                        char hex[5] = { c[1], c[2], c[3], c[4], '\0' };
                        *out += static_cast<char>(strtol(hex, nullptr, 16));
                        c += 4;
                    }
                    break;
                }
                default:  *out += *c; break;
            }
            c++;
            continue;
        }
        *out += *c++;
    }
    return *c == '"';
}

bool scan_uint(const char* json, const char* key, uint64_t* out) {
    std::string pat = std::string("\"") + key + "\"";
    const char* k = strstr(json, pat.c_str());
    if (!k) return false;
    const char* c = strchr(k + pat.size(), ':');
    if (!c) return false;
    c++;
    while (*c == ' ' || *c == '\t') c++;
    if (*c < '0' || *c > '9') return false;
    *out = strtoull(c, nullptr, 10);
    return true;
}

struct Carrier {
    bool                   present = false;
    WorldIdentity          identity;
    std::vector<BoardPost> posts;
    uint64_t               next_seq = 0;
};

// Read the whole carrier. Caller holds g_carrier_mu.
bool read_carrier_locked(Carrier* out) {
    if (g_path.empty()) return false;
    FILE* f = fopen(g_path.c_str(), "r");
    if (!f) return false;
    out->present = true;

    std::string line;
    int ch;
    auto handle = [&]() {
        if (line.empty()) return;
        const char* j = line.c_str();
        std::string kind;
        if (!scan_string(j, "r", &kind)) return;
        if (kind == "identity") {
            WorldIdentity id;
            scan_string(j, "episode_id", &id.episode_id);
            scan_string(j, "save_sha256", &id.save_sha256);
            uint64_t y = 0;
            if (scan_uint(j, "start_date_year", &y))
                id.start_date_year = static_cast<uint32_t>(y);
            if (!id.save_sha256.empty()) out->identity = id;   // LAST one wins
        } else if (kind == "post") {
            BoardPost p;
            uint64_t n = 0;
            if (!scan_uint(j, "seq", &n)) return;
            p.seq = n;
            scan_string(j, "post_id", &p.post_id);
            scan_string(j, "session_id", &p.who.session_id);
            if (scan_uint(j, "character_id", &n)) p.who.character_id = (uint32_t)n;
            scan_string(j, "game_date", &p.who.game_date);
            if (scan_uint(j, "tick", &n)) p.who.tick = n;
            if (scan_uint(j, "flush_id", &n)) p.who.flush_id = n;
            scan_string(j, "text", &p.text);
            // Append-only, written in seq order by one writer. A duplicate seq
            // can only come from a torn or doubled write; keep the FIRST, which
            // is the one every earlier reader already saw.
            if (!out->posts.empty() && p.seq <= out->posts.back().seq) return;
            out->posts.push_back(p);
            if (p.seq + 1 > out->next_seq) out->next_seq = p.seq + 1;
        }
    };
    while ((ch = fgetc(f)) != EOF) {
        if (ch == '\n') { handle(); line.clear(); }
        else line += static_cast<char>(ch);
    }
    handle();   // a last line with no trailing newline (a crash mid-write)
    fclose(f);
    return true;
}

// Note a record the device would not take. Caller holds g_carrier_mu.
//
// BEST-EFFORT LOGGING, LOAD-BEARING MEMORY. The BLOG goes to the same device
// that just refused a write and may itself be dropped; the flag and the count do
// not depend on it. If the log line survives, an operator sees it sooner — and
// if it does not, the artefact still says the carrier is incomplete.
void record_drop_locked(int err) {
    if (!g_degraded) {
        g_degraded        = true;
        g_degraded_reason = (err != 0) ? strerror(err) : "write refused";
    }
    ++g_records_dropped;
    BLOG("carrier: DROPPED a record (%llu so far): %s — the board's record of "
         "this episode is INCOMPLETE\n",
         (unsigned long long)g_records_dropped, g_degraded_reason.c_str());
}

// Append one already-rendered record. Caller holds g_carrier_mu.
void append_locked(const std::string& record) {
    if (g_path.empty()) return;
    FILE* f = fopen(g_path.c_str(), "a");
    if (!f) {
        // ERRNO FIRST: BLOG clobbers it, and a reason of "Success" reads as no
        // failure at all — the one string a degraded surface must never carry.
        const int err = errno;
        BLOG("carrier: cannot append to %s\n", g_path.c_str());
        // ck-t5bc, gating ck-hzp7: THE OPEN-FAILURE BRANCH IS A DROP TOO.
        // Without this, an unwritable directory (EACCES) produced a health
        // surface BYTE-IDENTICAL to a successful post — degraded:false,
        // records_dropped:0 — while no file existed at all. The bead's own
        // sentence stayed true against its own fix on this path: a dropped post
        // was indistinguishable from a post never made.
        record_drop_locked(err);
        return;
    }
    // ── ck-hzp7: EVERY WRITE IS CHECKED, NOT ONLY THE OPEN ──────────────────
    // `fopen(path, "a")` SUCCEEDS ON A FULL FILESYSTEM — the file already
    // exists, so no allocation is needed — and every write after it is refused.
    // Checking only the open meant a dropped post returned normally and the
    // caller believed it was carried, which is the one outcome this carrier
    // exists to prevent: A DROPPED POST IS INDISTINGUISHABLE FROM A POST NEVER
    // MADE, and telling those apart is the whole of ck-5gdx and ck-h9qr.
    //
    // fclose IS CHECKED AND IS THE IMPORTANT ONE. Buffered bytes are finally
    // refused there, so a failure can land in the close even when the fwrite
    // and the fflush returned clean. Checking only fwrite would miss it.
    bool ok = true;
    int  err = 0;
    if (fwrite(record.data(), 1, record.size(), f) != record.size()) {
        ok = false; err = errno;
    }
    if (ok && fputc('\n', f) == EOF) { ok = false; err = errno; }
    if (ok && fflush(f) != 0)         { ok = false; err = errno; }
    // fflush, not fsync, and BOTH sides of that are on the record because it is
    // a durability call somebody will revisit.
    //
    // FOR fflush (the choice here): the events this carrier exists to survive —
    // a crash, an OOM kill, a session-cap relaunch — are all PROCESS death, and
    // process death loses nothing once the bytes are in the kernel. Only a host
    // power-cut loses a tail, and there the episode is gone anyway. An fsync per
    // post is a stall on the flush path, which is the path a post shares with
    // the actions it is meant to be measured against.
    //
    // FOR fsync (the ck-6kvq.5 author's choice in their parallel implementation,
    // recorded because the operational fact behind it is not obvious from here):
    // THE NODE HAS BEEN OOM-KILLED BEFORE, and the per-post cost is bounded by
    // max_posts_per_episode. If the node starts losing whole machines rather
    // than whole processes, this is the line to change, and that is the reason.
    // The comment above argues fflush vs fsync — DURABILITY, i.e. whether bytes
    // ALREADY ACCEPTED will survive. It is a separate question from whether they
    // were accepted, and for a year only the first one was asked here.
    if (fclose(f) != 0 && ok) { ok = false; err = errno; }

    if (!ok) {
        record_drop_locked(err);
    }
}


// Resolve the carrier path for `episode_id`. Caller holds g_carrier_mu.
// Returns "" and creates nothing when there is no episode directory.
std::string resolve_path_locked(const std::string& episode_id) {
    // ck-2r46. AN EMPTY EPISODE ID IS NOT A FILENAME. Returning "" here is
    // what makes the carrier DEFER rather than open a shared file under a
    // made-up name; `board_carrier_adopt_episode_id` opens it the moment the
    // driver declares which episode this is.
    const std::string component = sanitise(episode_id);
    if (component.empty()) return std::string();
    const std::string dir = episode_dir();
    if (dir.empty()) return std::string();
    struct stat st;
    if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return std::string();
    const std::string state = dir + "/state";
    ::mkdir(state.c_str(), 0777);
    return state + "/board-" + component + ".jsonl";
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
// The carrier's degraded state, for the surface that publishes it (ck-hzp7).
// Takes the mutex; never called from inside this file's locked paths.
CarrierHealth board_carrier_health() {
    std::lock_guard<std::mutex> lk(g_carrier_mu);
    CarrierHealth h;
    h.degraded        = g_degraded;
    h.records_dropped = g_records_dropped;
    h.reason          = g_degraded_reason;
    return h;
}

void board_carrier_set_path_for_test(const char* path) {
    std::lock_guard<std::mutex> lk(g_carrier_mu);
    g_path            = (path && path[0]) ? path : "";
    g_path_overridden = (path && path[0]);
}

std::string board_carrier_path() {
    std::lock_guard<std::mutex> lk(g_carrier_mu);
    return g_path;
}

void board_carrier_reset_for_test() {
    std::lock_guard<std::mutex> lk(g_carrier_mu);
    g_pending          = false;
    g_pending_identity = WorldIdentity();
    g_pending_posts    = 0;
    g_pending_next_seq = 0;
}

void board_carrier_init_from_env() {
    // NO BOARD, NO CARRIER. Arms A, C, E0 and free play do not persist anything
    // and are not touched by any of this — byte for byte the pre-ck-81su bridge.
    if (!board().enabled()) return;
    board_carrier_open_with_id(episode_policy().get().episode_id);
}

// ck-2r46. The open/rehydrate path, WITH THE ID PASSED IN rather than read
// back out of global state. `adopt` knows the id before the policy does in
// some orders, and a function that re-reads a global cannot be called by
// whoever learned the fact first — that ordering dependency is exactly what
// made the first version of adopt a no-op in its own test.
void board_carrier_open_with_id(const std::string& episode_id) {
    if (!board().enabled()) return;

    EpisodePolicy p = episode_policy().get();
    p.episode_id = episode_id;

    Carrier c;
    {
        std::lock_guard<std::mutex> lk(g_carrier_mu);
        if (const char* ov = getenv("CK3_BOARD_CARRIER_PATH"); ov && ov[0]) {
            g_path            = ov;
            g_path_overridden = true;
        } else if (!g_path_overridden) {
            g_path = resolve_path_locked(p.episode_id);
        }
        if (g_path.empty()) {
            // TWO REASONS, TWO SENTENCES (ck-2r46). "No directory" and "no
            // episode id yet" are different states with different fixes, and
            // the second is EXPECTED at init: the carrier waits for the
            // driver's declaration rather than opening a shared file.
            if (sanitise(p.episode_id).empty()) {
                BLOG("carrier: DEFERRED — no episode id yet, so there is no "
                     "file to open; waiting for the episode declaration "
                     "(ck-2r46). The board is in memory and is not persisted "
                     "until then.\n");
            } else {
                BLOG("carrier: no episode directory — the board is NOT persisted, "
                     "and a relaunch of this process would lose it (ck-81su)\n");
            }
            return;
        }
        g_pending = false;
        if (!read_carrier_locked(&c) || c.posts.empty()) {
            // A cold start. Open the file (creating it) and say whose it is.
            std::string open_rec = "{\"r\":\"open\",\"episode_id\":\"" +
                                   board_detail::escape(p.episode_id) +
                                   "\",\"pid\":" + std::to_string((long long)getpid()) + "}";
            append_locked(open_rec);
            BLOG("carrier: fresh board at %s\n", g_path.c_str());
            return;
        }
        g_pending          = true;
        g_pending_identity = c.identity;
        g_pending_posts    = c.posts.size();
        g_pending_next_seq = c.next_seq;

#ifdef CK3_BOARD_NO_CARRIER_RESTORE
        // ── THE NEGATIVE CONTROL (ck-81su) ────────────────────────────────
        // The pre-fix bridge, exactly: the carrier is FOUND and IGNORED. The
        // board stays empty, its seq restarts at 0, and the next post is a
        // second row numbered 0 in an episode that still validates. The
        // acceptance's `resume` phase MUST fail here, and must fail on the SEQ
        // and not merely on the row count — a restarting seq is what makes a
        // truncated treatment record read as a complete one.
        g_pending = false;
        BLOG("carrier: NEGATIVE CONTROL — %zu posts on disk IGNORED, seq restarts "
             "at 0 (CK3_BOARD_NO_CARRIER_RESTORE)\n", c.posts.size());
        return;
#endif
    }

    // A RELAUNCH. Found, not adopted: latch and refuse until somebody declares.
    board().await_carrier(c.posts.size(), c.next_seq);
    BLOG("carrier: RELAUNCH — %zu posts and seq high-water %llu at %s; the board "
         "REFUSES posts (board_awaiting_save_identity) until the save identity is "
         "declared (ck-81su)\n",
         c.posts.size(), (unsigned long long)c.next_seq, board_carrier_path().c_str());
}

// ck-2r46. THE DRIVER HAS JUST SAID WHICH EPISODE THIS IS — open the carrier
// under that name, or rehydrate from it.
//
// NOT `board_carrier_begin`, WHICH TRUNCATES. `begin` is the host's explicit
// "discard this board" and is the one thing allowed to do that. This is the
// opposite: it is the first moment the carrier CAN know its filename, and a
// relaunch mid-episode must find the previous process's records and rehydrate
// them — truncating here would destroy exactly what ck-81su exists to keep.
//
// Idempotent, and silent when there is nothing to do: repeated declarations of
// the same id are normal (the driver re-declares on reconnect), and a carrier
// that already has its path is left alone.
void board_carrier_adopt_episode_id(const std::string& episode_id) {
    if (!board().enabled()) return;
    {
        std::lock_guard<std::mutex> lk(g_carrier_mu);
        if (g_path_overridden) return;          // a test pinned the path
        if (!g_path.empty()) return;            // already open, nothing to do
        if (resolve_path_locked(episode_id).empty()) return;   // still unnameable
    }
    // The normal open/rehydrate path, with THIS id. It takes the lock itself,
    // reads the carrier if one is there, and sets g_pending when the previous
    // process left posts behind.
    board_carrier_open_with_id(episode_id);
}

void board_carrier_begin(const std::string& episode_id) {
    std::lock_guard<std::mutex> lk(g_carrier_mu);
    g_pending          = false;
    g_pending_identity = WorldIdentity();
    g_pending_posts    = 0;
    g_pending_next_seq = 0;
    if (!g_path_overridden) {
        const std::string np = resolve_path_locked(episode_id);
        if (np.empty()) { g_path = ""; return; }
        g_path = np;
    }
    if (g_path.empty()) return;
    // TRUNCATE. An explicit host `board_admin begin` is the one thing in this
    // system allowed to discard a board, and it says so in as many words.
    //
    // ck-t5bc, same amend: THE FAILURE WAS DISCARDED BY CONSTRUCTION. The old
    // `if (FILE* f = fopen(..,"w")) fclose(f);` had nowhere to put a refusal.
    // A begin that cannot truncate leaves the PREVIOUS episode's records in
    // place and the next append lands after them — a stale board reading as
    // this episode's, which is worse than an empty one because it is
    // plausible. fclose is checked for the same reason every other write here
    // is: a close that fails is a write that did not land.
    if (FILE* f = fopen(g_path.c_str(), "w")) {
        if (fclose(f) != 0) record_drop_locked(errno);
    } else {
        const int err = errno;
        BLOG("carrier: cannot TRUNCATE %s — the previous episode's records "
             "remain and this episode's will be appended AFTER them\n",
             g_path.c_str());
        record_drop_locked(err);
    }
    append_locked("{\"r\":\"open\",\"episode_id\":\"" +
                  board_detail::escape(episode_id) + "\",\"pid\":" +
                  std::to_string((long long)getpid()) + "}");
}

// ---------------------------------------------------------------------------
// Writes
// ---------------------------------------------------------------------------
void board_carrier_append_post(const BoardPost& p) {
    std::lock_guard<std::mutex> lk(g_carrier_mu);
    if (g_path.empty()) return;
    std::string rec = "{\"r\":\"post\",";
    const std::string body = board_detail::render_post(p, std::string());
    rec += body.substr(1);   // splice: drop the body's own '{'
    append_locked(rec);
}

void board_carrier_note_identity(const WorldIdentity& id, uint64_t epoch,
                                 uint64_t at_seq) {
    if (id.empty()) return;   // an identity that names no save vouches for nothing
    std::lock_guard<std::mutex> lk(g_carrier_mu);
    if (g_path.empty()) return;
    std::string rec = "{\"r\":\"identity\",\"episode_id\":\"" +
                      board_detail::escape(id.episode_id) +
                      "\",\"save_sha256\":\"" + board_detail::escape(id.save_sha256) +
                      "\",\"start_date_year\":" + std::to_string(id.start_date_year) +
                      ",\"epoch\":" + std::to_string(epoch) +
                      ",\"at_seq\":" + std::to_string(at_seq) + "}";
    append_locked(rec);
}

// ---------------------------------------------------------------------------
// The decision seam
// ---------------------------------------------------------------------------
bool board_carrier_pending(WorldIdentity* recorded_out) {
    std::lock_guard<std::mutex> lk(g_carrier_mu);
    if (!g_pending) return false;
    if (recorded_out) *recorded_out = g_pending_identity;
    return true;
}

BoardRelaunch board_carrier_accept(const WorldIdentity& presented, uint64_t epoch,
                                   const std::string& why) {
    Carrier c;
    BoardRelaunch rec;
    {
        std::lock_guard<std::mutex> lk(g_carrier_mu);
        if (!g_pending) { rec.decision = "none"; return rec; }
        g_pending = false;
        read_carrier_locked(&c);
    }   // RELEASED before touching the board — see the lock order note up top.

    rec.at_seq         = c.next_seq;
    rec.epoch          = epoch;
    rec.carrier_posts  = c.posts.size();
    rec.episode_id     = presented.episode_id;
    rec.save_sha256    = presented.save_sha256;

    // THE CHAIN HAS TO BE DENSE FROM 0. read() indexes posts_ by seq, so a
    // carrier with a hole in it does not degrade — it hands every reader the
    // wrong row, silently, including the observer whose drain becomes the
    // bundle's board.jsonl. A holed carrier is a DAMAGED record and the honest
    // answer is to say so and seal, not to serve most of it.
    if (!board_posts_are_dense(c.posts)) {
        rec.decision = "refused_carrier_not_dense";
        rec.why      = "the carrier's seq chain is not dense from 0 (" +
                       std::to_string(c.posts.size()) + " posts, high-water " +
                       std::to_string(c.next_seq) + ") — a chain that cannot be "
                       "trusted is not partially trusted";
        board().refuse_carrier(rec);
        {
            std::lock_guard<std::mutex> lk(g_carrier_mu);
            append_locked("{\"r\":\"relaunch\"," + rec.json().substr(1));
        }
        BLOG("carrier: REFUSED — %s (ck-81su)\n", rec.why.c_str());
        return rec;
    }

    rec.decision       = "restored_from_carrier";
    rec.why            = why;
    rec.restored_posts = board().adopt_carrier(c.posts, c.next_seq, rec);

    {
        std::lock_guard<std::mutex> lk(g_carrier_mu);
        // THE DISCONTINUITY IS RECORDED, NOT HIDDEN. The board is continuous
        // across the relaunch AND the relaunch is a row in its own right, in the
        // carrier and (via board_read) in the bundle's board.jsonl.
        append_locked("{\"r\":\"relaunch\"," + rec.json().substr(1));
    }
    BLOG("carrier: RESTORED %llu posts, seq continues at %llu — %s (ck-81su)\n",
         (unsigned long long)rec.restored_posts,
         (unsigned long long)rec.at_seq, why.c_str());
    return rec;
}

BoardRelaunch board_carrier_reject(const WorldIdentity& presented, uint64_t epoch,
                                   const std::string& decision,
                                   const std::string& why) {
    BoardRelaunch rec;
    {
        std::lock_guard<std::mutex> lk(g_carrier_mu);
        if (!g_pending) { rec.decision = "none"; return rec; }
        // g_pending DELIBERATELY STAYS SET. A refusal is not a verdict on the
        // bytes, it is a verdict on one declaration: a harness that declared the
        // wrong identity first must be able to declare the right one and get its
        // board back. Every attempt is its own row in the record.
        rec.carrier_posts = g_pending_posts;
        rec.at_seq        = g_pending_next_seq;
    }
    rec.epoch       = epoch;
    rec.decision    = decision;
    rec.why         = why;
    rec.episode_id  = presented.episode_id;
    rec.save_sha256 = presented.save_sha256;

    board().refuse_carrier(rec);

    {
        std::lock_guard<std::mutex> lk(g_carrier_mu);
        append_locked("{\"r\":\"relaunch\"," + rec.json().substr(1));
    }
    BLOG("carrier: REFUSED (%s) — %s; the board is SEALED and posts are refused "
         "board_identity_refused. The %llu posts on disk are untouched (ck-81su)\n",
         decision.c_str(), why.c_str(), (unsigned long long)rec.carrier_posts);
    return rec;
}

}  // namespace bridge
