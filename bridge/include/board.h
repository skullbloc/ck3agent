// board.h — R4: THE SHARED APPEND-ONLY BOARD (ck-6kvq.12).
//
// Design of record: docs/design/event-pipeline-conductor.md §3, decisions G4
// and G5. This is the paper's TREATMENT CONDITION: arm B has a channel, arms A
// and C do not, and nothing else about the two conditions differs.
//
// WHY THE BOARD IS IN THE BRIDGE AND NOT IN ck3agent (§3.1 / G4)
// -------------------------------------------------------------
// R4 requires every post to carry {session_id, character_id, game_date, tick}
// and requires A and B to differ ONLY in the channel. Both are AUTHORITY
// claims, not formatting choices:
//
//   * session_id and character_id must be MINTED from the ownership table, not
//     self-reported, or the attribution is a courtesy and the mediator analysis
//     rests on six processes agreeing not to lie about who spoke.
//   * game_date and tick must come from the CLOCK OWNER, so a post is ordered
//     against the actions around it on a real clock rather than on wall-time.
//   * the arm gate must be enforced where the agent cannot reach it. A
//     Python-side board is a file six cooperating processes agree not to cheat
//     on; a bridge-side board is a verb that does not exist in arm A.
//
// THE FORGERY RULE, STATED ONCE
// -----------------------------
// `post()` takes the client's raw params frame AND a separately minted
// BoardAttribution. It parses exactly ONE field out of the frame — `text` — and
// it is structurally incapable of reading `session_id`, `character_id`,
// `game_date`, `tick` or `flush_id` from it, because the parse for those keys
// is not written anywhere in this module. A client that sends them is not
// refused; it is IGNORED, which is the only behaviour that stays correct when a
// future client sends them by accident.
//
// ARM GATING (§3.3 / G5, RULED A2 by the progenitor 2026-09-10)
// -------------------------------------------------------------
// Arm A registers NO board verb. `enabled()` is the single runtime authority;
// dispatch_router.cpp consults it in BOTH the dispatch chain and
// is_registered_action(), so in arm A `board_post` and `board_read` are
// indistinguishable from a typo: `unknown_action`, and absent from every
// surface an agent can enumerate. The MCP side mirrors this by not registering
// the two tools at all (ck3agent/server/mcp_server.py), and the system prompt
// never mentions a board.
//
// BOUNDS (§3.2)
// -------------
// max_post_bytes, max_posts_per_session_per_flush and max_posts_per_episode.
// Every bound REFUSES with a typed `board_quota_exceeded` naming which quota
// was hit. NOTHING IS EVER TRUNCATED and nothing is ever evicted: a board that
// silently shortens a post produces a transcript the paper cannot quote, and a
// board that drops an old post to make room is not append-only.
//
// PURITY: this translation unit has NO engine dependency — no offsets, no game
// globals, not even a stub — so bridge/test/test_board and
// bridge/test/acceptance_board compile it on a host with no CK3 anywhere. The
// calendar never appears here either: `game_date` arrives as an already-rendered
// string from the one caller that owns ck3::game_date (scripts/test_game_calendar.py
// section D is a hard gate, and this file must not open-code a date).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "world_identity.h"   // ck-81su: the carrier is judged by ck-9fc3's rule

namespace bridge {

// ============================================================================
// Configuration — host config, PRE-REGISTERED (§3.2)
// ============================================================================
// THE THREE BOUNDS BELOW ARE PRE-REGISTERED VALUES, NOT TUNABLES. They were
// adopted verbatim into the pre-registration (§3, bartertown
// docs/design/paper-ck3-preregistration.md, 2026-09-12) together with the
// frozen wire shape, the two-tool surface delta, and the property that earned
// them: a bound is a TYPED REFUSAL and never a truncation or an eviction, so
// nothing a bound touches is silently altered in the record.
//
// Changing one of these numbers mid-study changes the treatment condition
// between episodes, which is the quiet way a result stops meaning anything.
// They stay env-overridable because a host TEST must be able to drive a quota
// without posting twenty thousand times — not because an episode should. If an
// episode needs different bounds, AMEND THE PRE-REGISTRATION FIRST and say so;
// do not edit the defaults here and let the two disagree.
struct BoardConfig {
    // THE ARM GATE. False in arms A, C and E0; true in arm B and nowhere else.
    bool enabled = false;

    // A post longer than this is REFUSED, never truncated.
    // PRE-REGISTERED: 2048. Override: CK3_BOARD_MAX_POST_BYTES.
    size_t max_post_bytes = 2048;

    // Per (session, flush). A flush_id of 0 means "not inside a flush" (legacy
    // and null pipeline modes), where this bound does not apply and
    // max_posts_per_episode is the only one — see post().
    // PRE-REGISTERED: 3. Override: CK3_BOARD_MAX_POSTS_PER_FLUSH.
    size_t max_posts_per_session_per_flush = 3;

    // The whole-episode ceiling. An episode that hits it has a runaway agent,
    // and the honest outcome is a typed refusal in the record rather than an
    // unbounded vector in a 20-game-year process.
    // PRE-REGISTERED: 20000. Override: CK3_BOARD_MAX_POSTS_PER_EPISODE.
    size_t max_posts_per_episode = 20000;

    // FLUSH SCOPE (§3.2, as corrected by the 2026-09-12 vocabulary ruling).
    //
    // The bead was written against the withdrawn WINDOW scheduler and asks that
    // "a post outside a window is refused". In the shape that actually shipped
    // there are no windows: `board_post` is a mutation, so the pipeline BUFFERS
    // it and executes it inside a flush like any other write, and it carries the
    // same flush_id as the actions around it. That identity is the measurable
    // thing — reply latency and time-to-muster are counted in flushes against a
    // real clock, not in wall-seconds.
    //
    // So the surviving form of the rule is this flag. When it is set, a post
    // that arrives with NO flush behind it (flush_id 0 — served outside the
    // conductor, which in an experiment episode means something bypassed the
    // pipeline) is REFUSED rather than recorded unanchored, because an
    // unanchored post is a row the mediator analysis cannot join. The episode
    // harness sets it; free play does not, and there a post simply carries
    // flush_id 0 and the per-flush quota does not apply.
    bool require_flush_scope = false;
};

// ============================================================================
// THE BOUNDS WITNESS (ck-5gdx) — what manipulation this episode actually ran
// ============================================================================
// WHY A WITNESS AND NOT JUST THE NUMBERS. `stats_json()` below has always
// published the three EFFECTIVE bounds, and nothing carried them into a sealed
// bundle — so no finished episode said what its treatment was. That is half the
// defect. The other half is not fixed by recording the numbers, because
//
//     CK3_BOARD_MAX_POST_BYTES unset            -> 2048
//     CK3_BOARD_MAX_POST_BYTES=2048 exported    -> 2048
//
// are the SAME NUMBER and two different operator actions, and §7.1's model is
// that silence must not read the same as compliance. So the witness records the
// OVERRIDE ATTEMPT beside the value, and a reader can tell "nobody touched the
// manipulation" from "somebody set it, to the value it already had".
//
// THE FOUR STATES. `absent`, `agreed` and `disagreed` follow ck-zixt's launch
// record verbatim. The fourth is this bead's own finding: `env_size()` returns
// the DEFAULT for a value it cannot parse and for one that is <= 0, so
//
//     CK3_BOARD_MAX_POST_BYTES=banana   -> 2048, silently
//     CK3_BOARD_MAX_POSTS_PER_FLUSH=0   -> 3,    silently
//
// An operator action that had no effect and left no trace is the silent-failure
// family in its purest form, and without a fourth state it would be recorded as
// `absent` — which is the OPPOSITE fact about what somebody did. It is
// `refused`: attempted, and discarded by the bridge.
//
// AGREEMENT IS BYTE EQUALITY with the pre-registered value's canonical decimal
// spelling, for ck-zixt's reason: `2048` and `02048` mean one thing to strtoll
// and two things about what an operator typed. `02048` is therefore `disagreed`
// with an `in_force` that equals `pre_registered` — which is not a contradiction
// but the whole point: `ambient_state` is about the ACTION and the two numbers
// are about the EFFECT, and a reader that wants either can have it.
//
// THE VOCABULARY LIVES IN THE SCHEMA. These four strings are an enum in
// scripts/schemas/run_summary.schema.json, and this emission and
// scripts/registered_launch.py's `verify` are each checked against THAT rather
// than against each other. Two implementations that agree only because someone
// mirrored one into the other agree by coincidence; a third would have to be
// remembered. Conforming to a written contract is not the same claim.
namespace board_ambient {
constexpr const char* kAbsent    = "absent";     // nobody set it
constexpr const char* kAgreed    = "agreed";     // set, byte-equal to declared
constexpr const char* kDisagreed = "disagreed";  // set, honoured, different
constexpr const char* kRefused   = "refused";    // set, and DISCARDED by env_size
}  // namespace board_ambient

// One pre-registered bound, and what the environment tried to do to it.
struct BoardBoundWitness {
    std::string bound;            // "max_post_bytes" — the BoardConfig field
    std::string env_key;          // "CK3_BOARD_MAX_POST_BYTES"
    uint64_t    pre_registered = 0;  // the compiled-in declared value
    uint64_t    in_force = 0;        // what the board will actually enforce
    bool        ambient_set = false; // the variable was present in the env
    std::string ambient_raw;         // VERBATIM. Meaningless when !ambient_set;
                                     // "" is a real, different operator action
                                     // and is why the flag exists separately.
    std::string ambient_state;       // one of board_ambient::*
    bool        honoured = false;    // the override reached the config
};

// The whole witness. `observed` is false in a process where
// `board_init_from_env()` never ran — a host test that called `configure()`
// directly, or any build predating this. It is NEVER filled with defaults in
// that case: an axis nothing witnessed must not read like one that agreed.
struct BoardBoundsWitness {
    bool observed = false;
    std::vector<BoardBoundWitness> bounds;
    // `{"observed":bool,"bounds":[{...}]}`. Spliced into stats_json().
    std::string json() const;
};

// The witness this process resolved, or an unobserved one. Written exactly once
// by board_init_from_env(), before any client can connect; read afterwards.
const BoardBoundsWitness& board_bounds_witness();

// ============================================================================
// Attribution — minted by the bridge, NEVER by the client (§3.1)
// ============================================================================
struct BoardAttribution {
    std::string session_id;        // from the session registry
    uint32_t    character_id = 0;  // from the ownership table
    std::string game_date;         // from the clock owner, already rendered
    uint64_t    tick = 0;          // from the clock owner
    uint64_t    flush_id = 0;      // 0 == not inside a flush
};

struct BoardPost {
    uint64_t         seq = 0;      // monotonic per episode, FIRST POST IS 0
    std::string      post_id;      // "bp_00000000", derived from seq
    BoardAttribution who;
    std::string      text;
};

// Typed refusal reasons. Free functions rather than an enum-to-string switch so
// the wire strings are greppable from the tests that assert them.
namespace board_reason {
constexpr const char* kDisabled       = "board_disabled";
constexpr const char* kSealed         = "board_sealed";
constexpr const char* kNoSession      = "board_no_session";
constexpr const char* kEmptyText      = "board_empty_text";
constexpr const char* kQuotaExceeded  = "board_quota_exceeded";
constexpr const char* kOutsideFlush   = "board_outside_flush";
// ck-81su, the two CONTINUITY refusals. Both mean "this process found a board
// carrier for this episode and has not been told it may adopt it", and both are
// TYPED for the same reason every other refusal here is: a post that cannot be
// recorded must be visibly refused, never silently dropped into a board whose
// seq restarted at 0.
constexpr const char* kAwaitingIdentity = "board_awaiting_save_identity";
constexpr const char* kIdentityRefused  = "board_identity_refused";
}  // namespace board_reason

// Which quota a kQuotaExceeded refusal hit.
namespace board_quota {
constexpr const char* kPostBytes      = "max_post_bytes";
constexpr const char* kPostsPerFlush  = "max_posts_per_session_per_flush";
constexpr const char* kPostsPerEpisode = "max_posts_per_episode";
}  // namespace board_quota

// ============================================================================
// CONTINUITY ACROSS A PROCESS RELAUNCH (ck-81su)
// ============================================================================
// THE BUG THIS EXISTS FOR. `begin_episode()` resets seq to 0 on PROCESS START.
// ck-6kvq.5's resume relaunches CK3 mid-episode, so before this an arm-B episode
// came back from a resume with an EMPTY board and a seq restarting at 0 — the
// treatment record silently truncated, in a bundle that still validated, with a
// monotonic sequence that read as intact across the discontinuity. The board IS
// arm B's independent variable; that is worse than a lost log.
//
// THE SHAPE. Every accepted post is also appended to a CARRIER in the episode
// directory (board_persist.cpp), stamped with the save identity declared for the
// world it was written in — exactly as ck-9fc3 stamps the personality-lock
// carrier, and for exactly the same reason. On the next process start the board
// finds the carrier and does NOT adopt it: it LATCHES `AwaitingIdentity` and
// REFUSES every post with a typed reason. Nothing is restored on inference.
//
// THE ONE DECISION. The carrier is adopted only when a harness DECLARES the save
// identity of the world now loaded and ck-9fc3's judge_same_save() accepts it —
// the same rule, at the same site (world_identity_declare()), taken at the same
// moment as the personality-lock restore. A refused declaration leaves the board
// SEALED and refusing, which is a visible failure rather than a silent one.
//
// THE DISCONTINUITY IS RECORDED. Adopting the carrier does not hide the
// relaunch: it appends a BoardRelaunch to the board and to the carrier, it is
// published on `board_read` and on the stats block, and the meter lane writes it
// into the bundle's board.jsonl. "The board is continuous" and "the process
// restarted at seq N" are both true and both in the record.
enum class BoardContinuity {
    Fresh,             // no carrier: a genuinely new board, seq starts at 0
    AwaitingIdentity,  // a carrier exists and nobody has vouched for this world
    Restored,          // the carrier was adopted under an ACCEPTED declaration
    Refused,           // the declaration was refused; the board is SEALED
};
const char* board_continuity_name(BoardContinuity c);

// One discontinuity in the board's life: a process relaunched, and this is what
// the declaration decided about the board it found.
struct BoardRelaunch {
    uint64_t    at_seq = 0;           // the seq the board resumed at (0 if refused)
    uint64_t    epoch = 0;            // world epoch the decision was taken in
    uint64_t    carrier_posts = 0;    // posts the carrier held
    uint64_t    restored_posts = 0;   // posts actually adopted
    std::string decision;             // "restored_from_carrier" | "refused_<reason>"
    std::string why;                  // judge_same_save()'s sentence, verbatim
    std::string episode_id;           // the declared identity, for the record
    std::string save_sha256;
    std::string json() const;
};

struct BoardPostResult {
    bool        ok = false;
    std::string reason;   // "" on success, one of board_reason::* otherwise
    std::string quota;    // set only for kQuotaExceeded
    int64_t     limit = 0;   // the bound that was hit
    int64_t     observed = 0;// what the caller asked for
    BoardPost   post;     // fully stamped on success
};

struct BoardReadResult {
    std::vector<BoardPost> posts;
    uint64_t next_seq = 0;    // resume cursor: since_seq for the next call
    uint64_t total = 0;       // posts on the board, for a "am I behind?" check
    bool     truncated = false;  // the page hit `limit`; call again at next_seq
};

// ============================================================================
// EpisodeBoard
// ============================================================================
class EpisodeBoard {
public:
    // --- configuration / arm gate ------------------------------------------
    void        configure(const BoardConfig& c);
    BoardConfig config() const;

    // LOCK-FREE. Read on the IPC reader thread for every envelope that names a
    // board verb, and — more importantly — by is_registered_action(), which the
    // reader consults for EVERY envelope. A mutex there would tax arm A for a
    // feature arm A does not have.
    bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

    // --- per-episode lifecycle (§3.2: "created and sealed by the harness") --
    // Resets seq to 0, drops every post and every per-flush counter. A board
    // NEVER spans episodes.
    //
    // ck-81su: this is the IN-MEMORY reset and nothing else — it does not touch
    // the carrier, and it is deliberately NOT the thing that decides whether a
    // relaunched process starts empty. That decision is the continuity latch
    // below, taken after this runs.
    void begin_episode(const std::string& episode_id);

    // Refuse further posts; reads continue, so the harness can drain the board
    // into the bundle after the horizon is reached.
    void seal();
    bool sealed() const;

    std::string episode_id() const;

    // --- the two verbs ------------------------------------------------------
    // `params_json` is the client's RAW frame. Only `text` is read from it; see
    // the forgery rule at the top of this file.
    BoardPostResult post(const char* params_json, const BoardAttribution& minted);

    // Testing/overload entry point: the text already extracted. `post()` above
    // is this plus the one-field parse.
    BoardPostResult post_text(const std::string& text, const BoardAttribution& minted);

    // Everything with seq >= since_seq, up to `limit` posts (0 == config
    // default page size).
    BoardReadResult read(uint64_t since_seq, size_t limit) const;

    // --- wire + bundle shapes ----------------------------------------------
    // `{"posts":[...],"next_seq":N,"total":N,"truncated":bool}` — the body of a
    // board_read reply, minus the envelope the handler adds.
    std::string read_json(uint64_t since_seq, size_t limit) const;

    // One JSON object per line, `episode_id` stamped on EVERY record per the
    // §6.1 stamping law, joinable to actions.jsonl on
    // {episode_id, flush_id, session_id}. This is board.jsonl.
    std::string export_jsonl() const;

    // `{"enabled":..,"episode_id":..,"sealed":..,"posts":N,"next_seq":N,
    //   "refusals":N,"bounds_witness":{...},"by_session":{...}}` — the meter
    // lane's volume read, and the block get_tick_info publishes ONLY when the
    // board is enabled (arm A must not be able to learn that a board exists).
    //
    // ck-5gdx: `bounds_witness` rides here rather than in a block of its own
    // BECAUSE of that gate. The witness describes the manipulation, the
    // manipulation exists only in arm B, and a separate always-present block
    // would tell an arm-A agent that a board exists somewhere — the third
    // condition decision G5 rejected. The driver reads it off the
    // `get_tick_info` frame it already takes and carries it into the bundle.
    std::string stats_json() const;

    // --- continuity across a relaunch (ck-81su) -----------------------------
    BoardContinuity            continuity() const;
    std::vector<BoardRelaunch> relaunches() const;

    // LATCH. "A carrier for this episode exists and nobody has vouched for the
    // world now loaded." Every post is refused with board_awaiting_save_identity
    // until a declaration resolves it. Called once, from
    // board_carrier_init_from_env(), and only when the carrier holds posts.
    void await_carrier(size_t carrier_posts, uint64_t carrier_next_seq);

    // ADOPT. Installs the carrier's rows AND its seq HIGH-WATER MARK, so the
    // next post continues the chain rather than restarting it, and records the
    // discontinuity. Returns how many rows were adopted.
    //
    // CALL THIS FROM ONE PLACE ONLY: board_carrier_accept(), which is reached
    // only from world_identity_declare() after judge_same_save() said YES. The
    // guard is deliberately not repeated here — putting it in two places is how
    // two rules are born.
    size_t adopt_carrier(std::vector<BoardPost> posts, uint64_t next_seq,
                         const BoardRelaunch& rec);

    // REFUSE. Seals the board and records why. Reads still work, so the harness
    // can still drain whatever this process wrote, and the carrier on disk is
    // untouched — a refusal loses no bytes, it only refuses to claim them.
    void refuse_carrier(const BoardRelaunch& rec);

    // --- counters -----------------------------------------------------------
    size_t   total_posts() const;
    size_t   total_refusals() const;
    uint64_t next_seq() const;

private:
    mutable std::mutex mu_;
    std::atomic<bool>  enabled_{false};   // mirror of cfg_.enabled, lock-free

    BoardConfig            cfg_;
    std::string            episode_id_;
    bool                   sealed_ = false;
    std::vector<BoardPost> posts_;
    uint64_t               next_seq_ = 0;
    size_t                 refusals_ = 0;

    // ck-81su continuity state. Fresh unless a carrier was found.
    BoardContinuity            continuity_ = BoardContinuity::Fresh;
    size_t                     carrier_posts_ = 0;
    uint64_t                   carrier_next_seq_ = 0;
    std::vector<BoardRelaunch> relaunches_;

    // (session_id, flush_id) -> posts already accepted in that flush. Cleared
    // per episode; bounded by pruning every key from an older flush the moment
    // a newer flush posts, so a 20-game-year episode does not accumulate one
    // entry per flush per session.
    std::map<std::pair<std::string, uint64_t>, size_t> per_flush_;
    uint64_t highest_flush_seen_ = 0;

    // Caller must hold mu_.
    BoardPostResult refuse_locked(const char* reason, const char* quota,
                                  int64_t limit, int64_t observed);
    std::string     continuity_json_locked() const;
};

// Process-wide board. One per process, reset per episode.
EpisodeBoard& board();

// Resolve the board's configuration from the environment. Called once at bridge
// init, AFTER episode_policy().init_from_env() (it reads the resolved arm).
//   CK3_EPISODE_ARM        e0 | a | b | c   — the board exists iff this is `b`
//   CK3_BOARD_ENABLED      0 | 1            — explicit override, for tests
//   CK3_BOARD_MAX_POST_BYTES
//   CK3_BOARD_MAX_POSTS_PER_FLUSH
//   CK3_BOARD_MAX_POSTS_PER_EPISODE
void board_init_from_env();

// True when `action_type` is one of the two board verbs. Used by the dispatch
// router's arm gate, where it must answer the same way for the dispatch chain
// and for is_registered_action().
bool is_board_action(const char* action_type);

// THE ARM GATE, AS ONE FUNCTION — and the reason it is a function.
//
// The gate has to be applied in TWO places in dispatch_router.cpp: the dispatch
// chain (so a board verb in arm A is refused) and is_registered_action() (so a
// board verb in arm A is refused BY THE READER THREAD, before any handler, with
// the same `unknown_action` a typo gets). Two copies of a rule is two copies to
// get wrong, and neither copy would be testable without a CK3 binary, because
// dispatch_router.cpp cannot be linked on a host. So the rule lives here, pure
// and host-tested, and both sites delegate.
//
// Returns:
//    1  a board verb, REGISTERED on this bridge  (arm B)
//    0  a board verb, NOT REGISTERED             (arms A / C / E0 / free play)
//   -1  not a board verb at all — the caller's own registry decides
//
// `board_admin` is gated on the board AND on a host token being configured in
// the game process's environment (see do_board_admin), so a free-play bridge has
// no board lifecycle verb either.
int board_verb_registration(const char* action_type);

// THE BOARD'S STORAGE INVARIANT, stated once and owned by the board.
//
// EpisodeBoard::read() treats `since_seq` as an INDEX into posts_ — it relies on
// posts_[i].seq == i, says so in a comment, and test_board asserts it. So a
// restored chain that is not DENSE FROM 0 does not degrade gracefully: every
// reader, including the observer whose drain becomes the bundle's board.jsonl,
// silently gets the wrong row. A carrier with a hole in it (a torn write, a
// truncated tail) is therefore REFUSED outright rather than loaded best-effort.
// A chain that cannot be trusted is not partially trusted.
bool board_posts_are_dense(const std::vector<BoardPost>& posts);

// INTERNAL, shared between board.cpp and board_persist.cpp: one renderer for a
// post row, so the carrier on disk and the wire can never drift apart.
namespace board_detail {
std::string escape(const std::string& s);
std::string render_post(const BoardPost& p, const std::string& episode_id);
}  // namespace board_detail

// Parse ONLY `text` out of a client frame, resolving flat-vs-`params` framing
// (ck-6kvq.15). Exposed so the forgery property is directly testable: hand it a
// frame full of forged attribution and observe that nothing but `text` comes
// back.
bool board_parse_post_text(const char* params_json, std::string* text_out);

// ============================================================================
// THE CARRIER (board_persist.cpp) — ck-81su
// ============================================================================
// The board's posts and its seq high-water mark, on disk, in the EPISODE
// DIRECTORY, so they outlive the process the way ck-9fc3's lock file outlives
// it. Append-only JSONL in the bridge's own private shape; the bundle's
// board.jsonl is still rendered from the live board by the meter lane, so there
// is exactly one public shape for a post.
//
//   $CK3_EPISODE_DIR/state/board-<episode_id>.jsonl
//
// resolved the way episode_abort.sh and session.cpp resolve it
// (CK3_EPISODE_DIR, else $CK3_INSTANCE_DIR/episode) — no third spelling. WITH NO
// EPISODE DIRECTORY THERE IS NO CARRIER: free play and any host without one
// behave byte for byte as they did before this bead, and so does every arm
// without a board, because none of this is reached when the board is disabled.
//
// PER EPISODE ID, in the filename. Two episodes sharing one episode directory
// can never read each other's board, and the same-episode clause below is then
// a second lock on a door that is already shut.

// Resolve the carrier path and, if a carrier for THIS episode already holds
// posts, latch the board AwaitingIdentity. Called from board_init_from_env(),
// after the board is configured and the episode has begun.
// ── ck-hzp7: WHAT THE CARRIER FAILED TO WRITE ───────────────────────────────
// A POSITIVE RECORD OF INCOMPLETENESS. Its ABSENCE IS NOT EVIDENCE OF
// COMPLETENESS: a bundle sealed before this bead carries no such block, and on a
// device so full that the BUNDLE cannot be written either, nothing is recorded
// at all. That second case is honest-no-bundle rather than degraded-with-count,
// and a missing bundle is loud where a bundle missing ROWS is quiet — which is
// why this only claims to convert an invisible loss into a stated one WHENEVER
// THE BUNDLE CAN STILL BE WRITTEN.
//
// Held in memory and published over the socket, never written to disk, because
// the failure it reports is a disk that will not take writes: an error channel
// that shares a cause with the error cannot report it.
// ck-9erk. READING THIS STRUCT: `degraded` IS NOT A SUMMARY OF
// `records_dropped`, and a readback that asserts on it alone can be green while
// a whole write path goes unchecked. MEASURED (ck-hzp7 §F), by reverting ONLY
// the begin-truncate check and leaving the append check in place:
//
//     shipping            degraded=true   records_dropped=2
//     truncate-unchecked  degraded=true   records_dropped=1
//
// `degraded` is TRUE IN BOTH, because the append that follows the failed
// truncate fails too and sets the sticky flag anyway. ONLY THE COUNT CAN SEE
// THE DIFFERENCE. So a check that a refusal was RECORDED must assert the count
// — and on a path with more than one write, the count it EXPECTS rather than
// merely non-zero.
//
// This is written here because the boolean LOOKS like the summary of the count
// and is not, which is exactly what a later reader deletes as redundant.
struct CarrierHealth {
    bool        degraded = false;        // a record was refused, sticky
    uint64_t    records_dropped = 0;     // how many — THE DISCRIMINATING FIELD
    std::string reason;                  // strerror of the FIRST refusal
};
CarrierHealth board_carrier_health();

void board_carrier_init_from_env();

// Path of the carrier; "" when persistence is off (no episode dir, or the board
// is disabled). Tests point it elsewhere with set_; nullptr/"" disables.
void        board_carrier_set_path_for_test(const char* path);
std::string board_carrier_path();

// Append one accepted post. Called from EpisodeBoard::post_text under the board
// mutex — so nothing in board_persist.cpp may call back into EpisodeBoard while
// holding the carrier mutex. The carrier mutex is a LEAF.
void board_carrier_append_post(const BoardPost& p);

// Stamp the identity declared for the world these posts are being written in.
// PUSHED by world_identity_declare(), never pulled by the writer — the same
// lock-order argument as set_personality_locks_identity().
void board_carrier_note_identity(const WorldIdentity& id, uint64_t epoch,
                                 uint64_t at_seq);

// Start a carrier for a new episode (board_admin op=begin). Rotates the path to
// the new episode id and truncates it: an explicit host "begin" is the one thing
// that is allowed to discard a board.
// ck-2r46. Open the carrier once the driver declares the episode id. Unlike
// `board_carrier_begin` this NEVER truncates — a relaunch mid-episode must
// rehydrate the previous process's posts, not discard them.
void board_carrier_adopt_episode_id(const std::string& episode_id);

// ck-2r46. The open/rehydrate path with the episode id passed in.
void board_carrier_open_with_id(const std::string& episode_id);

void board_carrier_begin(const std::string& episode_id);

// --- the decision seam --------------------------------------------------
// Is there a carrier waiting to be judged, and what identity does it carry?
// Returns false when there is nothing to decide (no carrier, or the board has
// already adopted or refused one).
bool board_carrier_pending(WorldIdentity* recorded_out);

// ACCEPT / REJECT. The VERDICT is not taken here: judge_same_save() takes it in
// world_identity_declare(), and these two record what it decided. Splitting it
// this way is the whole point — the board does not get a second notion of "the
// same save", it gets the answer to the one that already exists.
BoardRelaunch board_carrier_accept(const WorldIdentity& presented, uint64_t epoch,
                                   const std::string& why);
BoardRelaunch board_carrier_reject(const WorldIdentity& presented, uint64_t epoch,
                                   const std::string& decision,
                                   const std::string& why);

// Test seam: forget the resolved path and the pending carrier (does NOT delete
// the file, and does NOT touch the board).
void board_carrier_reset_for_test();

}  // namespace bridge
