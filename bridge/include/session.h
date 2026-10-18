// session.h — M1 multi-agent identity + ownership layer.
//
// Server-minted sessions carry a per-client control context.  A session can
// CLAIM characters; the global ownership table (character_id -> session_id)
// lets the dispatch choke point refuse cross-session actor hijacking.
//
// Legacy N=1 behaviour is preserved: a client that uses the old handshake
// gets an implicit control session, and set_controlled_character claims the
// target character for that implicit session.

#pragma once

// ck-6kvq.5: the resume decision is taken with ck-9fc3's identity rule, NOT a
// second copy of it.  world_identity.h says so explicitly ("one identity, two
// consumers ... do not fork it"): this is the CLAIMS consumer, and the steering
// consumer is `declare_save_identity`.
#include "skip_held_heir.h"   //: ck-7yrp ReleaseSubjectState
#include "world_identity.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace bridge {

// Forward declaration from ipc_server.cpp internals.
struct ClientSlot;

// Upper bound on the legacy controlled-character watchlist (g_controlled_chars).
// This is a snapshot-memory guard, not an ownership policy — N=6 plus an observer
// fits comfortably.  It lives here (rather than privately in dispatch_router.cpp)
// because the registry is the single enforcement point: a claim that would push
// the derived watchlist past the cap is REFUSED, so the ownership table and the
// watchlist can never silently disagree (ck-6kvq.11, design §1.5).
constexpr size_t MAX_CONTROLLED_WATCHLIST = 10;

// Upper bound on the client-supplied handshake name a session will store.
// client_name is taken VERBATIM from the client's frame, and since ck-6kvq.11
// every session's name is echoed in every get_bridge_state reply — so an
// oversized name would be paid for by every client on every state read, not
// just by the one that sent it.  Truncated, never refused: the name is a label,
// and refusing a handshake over a long one would be worse than shortening it.
constexpr size_t MAX_CLIENT_NAME = 128;

// ============================================================================
// Session — server-minted identity for one connected client.
// ============================================================================
class Session {
public:
    enum class Mode {
        CONTROL,   // may claim characters and issue mutations
        OBSERVE,   // read-only: queries + state reads, no writes
    };

    // ── Session lifetime is DECOUPLED FROM THE SOCKET (ck-6kvq.5, §6.2/§7.1) ──
    // M1's session lifetime WAS the socket's: a drop destroyed the session and
    // freed its claims (§1.4 defect 1).  In an experiment episode that is an
    // uncontrolled change to the frozen arm, caused by a network event.
    //
    //   ATTACHED  a live slot is bound; ordinary operation.
    //   DETACHED  the socket is gone, the session and its claims are NOT.  The
    //             clock keeps running (G3' reversed, §2.7) and the gap is
    //             RECORDED as unavailability — a detached agent is a measured
    //             hole in the record, not an absent participant.
    //   LOST      DETACHED for longer than max_detach_s.  STILL HOLDS ITS
    //             CLAIMS; the state exists to be REPORTED (episode_abort.sh
    //             aborts `session_lost`), never to silently release anything.
    enum class State {
        ATTACHED,
        DETACHED,
        LOST,
    };

    std::string session_id;
    std::string client_name;
    Mode mode = Mode::CONTROL;
    uint64_t created_at_ms = 0;

    // ── Resume (ck-6kvq.5) ──────────────────────────────────────────────────
    // SERVER-MINTED, like session_id.  §7's "nothing client-supplied is
    // identity" bites HARDER here than it does for session_id, because this
    // token is an AUTHORISATION OVER A CLAIM SET: if the client chose it, any
    // client could seize another session's claimed earl by guessing a name.
    // Handed back in handshake_ack; presented on reconnect.  Guarded by
    // SessionRegistry::mutex_.
    std::string resume_token;
    State    state = State::ATTACHED;
    uint64_t state_since_ms = 0;   // when this session entered `state`

    // The slot this session is bound to, and the slot GENERATION at bind time.
    // The generation is the stale-close guard: a detach request carrying an
    // older binding than the session's current one is refused outright, so a
    // late peer-pid reap of a socket that is already dead cannot take a
    // reconnected session's claims offline.  -1 / 0 means "bound to no slot".
    int      bound_slot_idx = -1;
    uint32_t bound_slot_gen = 0;

    // The save/world this session's claims were taken against (declared, never
    // derived — world_identity.h), and ck-hxh7's world epoch at attach time
    // (0 = "the detector never spoke", the seam's unavailable sentinel).
    WorldIdentity world;
    uint64_t attach_epoch = 0;

    // Per-session mutable state (protected by mutex).
    mutable std::mutex mutex;
    uint32_t primary_character_id = 0;      // "my main actor"
    // ck-7rk6: the last character this session STEERED, never cleared by a
    // release. The succession handoff keys on primary_character_id, and
    // release_character() zeroes that when the claim set empties — so an
    // agent releasing its own CORPSE blinded the handoff entirely: the loop
    // `continue`d on ctrl == 0, the redirect never ran, and the seat was lost
    // silently with skip_held_heir reading 0 for a collision that happened.
    // Measured in arma6: the agent DOES release its dead character (after 624
    // failed handoff attempts there, so that episode is not the bad case — but
    // the path is real). This field lets the handoff still see the death.
    uint32_t last_steered_character_id = 0;
    // ck-7yrp: WAS THE RELEASED SUBJECT ALIVE OR DEAD **AT RELEASE**?
    //
    // last_steered_character_id alone cannot answer it: release_character sets
    // that unconditionally, alive or dead, so nothing downstream could tell a
    // deliberate drop of a living character from a seat releasing its own corpse.
    // ep-03 released 36552 alive, it died 3100 ticks later, and the recovery
    // branch seized it. Default Unknown, which handoff_subject treats as NO
    // RECOVERY — see skip_held_heir.h for why absence must not look like an answer.
    ReleaseSubjectState last_release_state = ReleaseSubjectState::Unknown;

    // ck-48z8: WHAT THE CLAIM ASKED FOR, so a handoff can preserve it.
    //
    // The succession handoff used to disable the heir's native AI whenever
    // g_agent_mode was true — and g_agent_mode is rebuilt as `primary != 0`, so it
    // is TRUE in every arm episode that has any control session at all, regardless
    // of what the claim requested. The registered design is STEER mode
    // ("the last personality steer persists; the native AI executes as it always
    // does") and do_set_controlled_character defaults disable_native_ai=false, so
    // every succession in set28 silently promoted the heir into Drive mode.
    //
    // g_agent_mode CANNOT carry this: it answers "is anyone steering anything",
    // and the question at a handoff is "did THIS session's claim disable the AI".
    // Those differ for every default take, which is all of them.
    bool     claim_disabled_native_ai = false;
    uint32_t heir_character_id = 0;         // cached primary-title heir
    uint32_t primary_title_id = 0;          // cached highest-tier title of primary
    std::set<uint32_t> claimed_character_ids;

    // Monotonic claim sequence — the order in which this session acquired its
    // CURRENT run of claims.  Assigned by SessionRegistry from a monotonic
    // counter on the transition from "holds nothing" to "holds something", and
    // reset to 0 when the last claim is released.  0 means "holds nothing".
    //
    // This is what makes the global primary deterministic: rebuild_legacy_mirrors
    // picks the control session with the LOWEST nonzero claim_seq, which is
    // stable under rehash, insertion order, and session-id formatting.  Guarded
    // by SessionRegistry::mutex_, NOT by Session::mutex.
    uint64_t claim_seq = 0;

    //: ck-j6lk: HAS THIS SESSION EVER HELD A CLAIM — monotonic, never cleared.
    //:
    //: It exists because nothing else could answer that question. claim_seq is
    //: RESET to 0 by note_session_release_locked the moment the claimed set
    //: empties (deliberately: a session that gives up its last claim gives up its
    //: claim-order slot, so re-claiming goes to the BACK rather than silently
    //: stealing the primary back), claimed_character_ids is cleared beside it, and
    //: primary_character_id is zeroed. So a session that claimed, released and
    //: then detached was bit-for-bit identical to one that never claimed at all —
    //: and R3's claim pin has to separate exactly those two.
    //:
    //: last_steered_character_id survives a release but is NOT a substitute: it is
    //: set only when primary_character_id == char_id at release, and ck-qv51
    //: deliberately leaves it alone for an UndoneClaim. A session that released a
    //: non-primary claim, or whose claim was rolled back, reads 0 there — so
    //: keying on it would classify a REAL SEAT as never-claimed and destroy it,
    //: which is the one direction the ruling forbids.
    //:
    //: A COUNT rather than a bool so a row can say "1 claim, then released"
    //: instead of just "true" (bt-bbd's choice). Incremented in
    //: note_session_claim_locked, which ALL THREE claim paths reach — both
    //: claim_character paths and swap_character, whose own comment notes it calls
    //: that function for the manifest binding even though the empty->held
    //: transition does not fire. A flag keyed on claim_seq MOVING would have
    //: missed a session whose only claim arrived by swap.
    //:
    //: ATOMIC on purpose: it is written under the registry mutex and read from the
    //: slot-release path under g_client_mutex, which are different locks. An
    //: atomic makes the read safe without inventing a lock order between them.
    std::atomic<uint64_t> claims_ever{0};

    //: ck-j6lk state (b): THE SESSION DECLARED ITSELF A SEAT, at handshake, by
    //: sending `"episode_seat": true`. DECLARED, never inferred.
    //:
    //: It exists for ck-3ru8's window: the world loads ~17s after the first
    //: connection, so a real agent has a stretch in which claims_ever is still 0.
    //: Keying the pin on claims alone would destroy such a seat if it died there,
    //: and a genuine agent failure would leave NO unavailability behind — the
    //: defect this bead fixes, pointed the other way.
    //:
    //: The bridge cannot work this out for itself. manifest_primary_session_ is
    //: bound by the CLAIM (note_session_claim_locked looks up
    //: ownership_[manifest_primary_char_]), so before the first claim the registry
    //: knows which character is designated but not which session will take it.
    //: The only other pre-claim facts are client_name, mode, agent_version and
    //: capabilities — and in the shakedown-2 log the real agent did differ from the
    //: helpers structurally (capabilities ["observe","act","pause"] against
    //: ["observe","act"]). That is a PROXY: correct until a helper gains `pause`,
    //: and one rename from the name-matching this repo forbids outright. So the
    //: fact is DECLARED, exactly as `episode_observer` already is.
    //:
    //: Atomic for the same reason as claims_ever: written on the handshake path,
    //: read on the slot-release path under a different lock.
    std::atomic<bool> declared_seat{false};

    Session() = default;
    Session(std::string id, std::string name, Mode m, uint64_t ts)
        : session_id(std::move(id)),
          client_name(std::move(name)),
          mode(m),
          created_at_ms(ts) {}

    bool is_control() const { return mode == Mode::CONTROL; }
    bool is_observe() const { return mode == Mode::OBSERVE; }

    // A session with no live socket.  BOTH DETACHED AND LOST STILL HOLD CLAIMS.
    bool is_attached() const { return state == State::ATTACHED; }

    // Claim a character as owned by this session.  Returns true on first claim.
    // If the character is already claimed by this session, returns true and
    // sets it as primary (idempotent).
    bool claim(uint32_t char_id);

    // Release a character.  Returns true if the session owned it.
    bool release(uint32_t char_id);

    // Set the primary actor.  The character must be claimed; returns false if
    // it is not currently owned by this session.
    bool set_primary(uint32_t char_id);

    // Clear everything (used on disconnect / legacy release-all).
    void clear();

    // JSON snapshot of this session (for get_bridge_state).  Caller supplies a
    // ResponseBuilder-compatible sink via a small lambda is not used here to
    // keep this header dependency-light; instead we expose the raw fields above
    // and a helper that returns a JSON string.
    std::string to_json() const;

    // Convenience: release a set of characters atomically.
    std::vector<uint32_t> release_all();
};

// ============================================================================
// ClientContext — transient per-request routing context.
// ============================================================================
struct ClientContext {
    Session* session = nullptr;        // never null when installed
    ClientSlot* slot = nullptr;        // optional back-pointer to IPC slot
    const char* action_type = nullptr; // current action being dispatched
};

// Thread-local current request context.  Set by ClientContextGuard around the
// dispatch choke point and around game-thread drains of queued actions/intents.
extern thread_local ClientContext* g_current_client_context;

// RAII guard: installs a ClientContext for the current thread and restores the
// previous one on destruction.  Defined inline so it is cheap to use everywhere.
class ClientContextGuard {
public:
    explicit ClientContextGuard(ClientContext* ctx) : prev_(g_current_client_context) {
        g_current_client_context = ctx;
    }
    ~ClientContextGuard() { g_current_client_context = prev_; }

    ClientContextGuard(const ClientContextGuard&) = delete;
    ClientContextGuard& operator=(const ClientContextGuard&) = delete;
private:
    ClientContext* prev_;
};

// Helper to fetch the current session (may return nullptr outside of dispatch).
inline Session* current_session() {
    return g_current_client_context ? g_current_client_context->session : nullptr;
}

// ============================================================================
// SessionRegistry — global session + ownership table.
// ============================================================================
class SessionRegistry {
public:
    // Create a new session.  mode is CONTROL unless explicitly requested.
    Session* create(const std::string& client_name,
                    Session::Mode mode = Session::Mode::CONTROL);

    // Look up a session by id.
    Session* find(const std::string& session_id);

    // Destroy a session and release all its ownership claims.
    bool destroy(const std::string& session_id);

    // Ownership lookup: which session owns this character?  Null if unowned.
    Session* owner_of(uint32_t char_id);

    // Claim ownership on behalf of a session.  Returns false if already owned
    // by a different session.
    bool claim_character(Session* session, uint32_t char_id, std::string* reason_out);

    // Release ownership from a session.  Returns false if the session did not
    // own the character.
    //: ck-7yrp: the state DEFAULTS to Unknown rather than being required, because
    //: the registry cannot see aliveness and most callers (tests, internal paths)
    //: genuinely do not know. Unknown means NO RECOVERY, so a caller that omits it
    //: gets the conservative answer instead of a plausible-looking wrong one.
    bool release_character(Session* session, uint32_t char_id,
                           ReleaseSubjectState state_at_release
                               = ReleaseSubjectState::Unknown);
    //: ck-swap: move a claim from `from` to `to` in ONE step. Net zero against
    //: MAX_CONTROLLED_WATCHLIST when `from` is held, so the cap cannot refuse a
    //: switch; claim_seq is never touched, so §7.3i never sees a move. When
    //: `from` is NOT held this is a net +1 and delegates to claim_character so
    //: the cap is enforced — a swap is not a back door past it.
    bool swap_character(Session* session, uint32_t from, uint32_t to,
                        std::string* reason_out);

    // Snapshot of all sessions (JSON array), used by get_bridge_state.
    std::string sessions_json() const;

    // Rebuild the legacy global mirrors (g_controlled_primary, g_controlled_chars,
    // g_controlled_heir).  Called after any claim/release/primary change.
    //
    // The global primary is chosen DETERMINISTICALLY (ck-6kvq.11, design §1.4
    // defect 2 / §7 item 2):
    //   1. the episode-manifest designation, if one is set and still resolvable;
    //   2. otherwise the control session with the lowest nonzero claim_seq
    //      (i.e. claim order).
    // It is never chosen by hash order of sessions_.
    //
    // Takes mutex_ itself.  (It used to require the caller to already hold it,
    // while three external call sites — dispatch_router.cpp and bridge_init.cpp
    // — called it unlocked and raced sessions_.  ck-6kvq.11.)
    void rebuild_legacy_mirrors();

    // --- Episode-manifest designation of the global primary (design §7 item 2) ---
    //
    // The episode manifest names ONE claimed character as the global primary (for
    // the 1066 episode, the English king).  The designation is by character, but
    // it BINDS to the owning session once that character is claimed, so a
    // succession handoff (dead king -> heir, bridge_init.cpp check_controlled_handoff)
    // keeps the designation with the same session instead of dangling on a corpse.
    //
    // Pass 0 to clear the designation and fall back to claim order.
    void set_manifest_primary(uint32_t char_id);
    uint32_t manifest_primary() const;

    // The session the manifest designation is currently bound to ("" if none).
    std::string manifest_primary_session() const;

    // Ownership/watchlist invariant check.  Returns true when a divergence has
    // been detected by the most recent rebuild_legacy_mirrors() — i.e. the legacy
    // mirrors do not agree with the ownership table.  *detail receives a
    // human-readable description.  Callers must treat true as an ERROR, not a
    // log line (design §1.5).
    bool ownership_divergence(std::string* detail) const;

    // Number of distinct characters claimed across all CONTROL sessions — i.e.
    // the size the derived watchlist will have.
    size_t controlled_claim_count() const;
    //: ck-bzcw: caller holds mutex_. THE cap computation — claim_character
    //: and get_bridge_state both reach it through here, so the number the
    //: driver records is the number that refuses.
    size_t controlled_claim_count_locked() const;

    // Iterate over all sessions.  Callback receives a const Session&.
    template<typename Fn>
    void for_each(Fn&& fn) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& kv : sessions_) {
            fn(*kv.second);
        }
    }

    // Return a copy of all session ids (snapshot).
    std::vector<std::string> ids() const;

    // ========================================================================
    // DETACH / RESUME (ck-6kvq.5, design §6.2)
    // ========================================================================
    // The through line of every function below: A DETACHED SESSION IS STILL A
    // SESSION.  It holds its claims, it holds its ownership entries, it keeps
    // accruing unavailability, and nothing here ever re-enables `ai_ctrl+0x28`.
    // The only thing it has lost is a way to talk.

    // Bind a session to an IPC slot and mark it ATTACHED.  Called on handshake
    // and on a successful resume.
    void attach_to_slot(Session* session, int slot_idx, uint32_t slot_gen,
                        uint64_t now_ms);

    // Slot release.  The session is DETACHED, not destroyed.
    //
    // THE STALE-GENERATION GUARD.  `slot_gen` is the generation the CALLER saw
    // when it read the binding, i.e. the binding this close is entitled to tear
    // down.  If the session has since rebound to another slot, or to a newer
    // generation of the same slot index, the client has already reconnected and
    // this close is a ghost: it returns false and changes NOTHING.
    //
    // Pass slot_idx < 0 to force a detach regardless (shutdown paths that are
    // not slot-scoped).
    bool detach_from_slot(const std::string& session_id, int slot_idx,
                          uint32_t slot_gen, uint64_t now_ms);

    struct ResumeOutcome {
        // The rebound session on success.  ALWAYS NULL on failure — and on
        // "world_changed" the pointer is not merely unset, the session has been
        // ERASED (see resume_by_token).
        Session* session = nullptr;
        bool ok = false;
        // Stable and greppable; echoed in handshake_ack.resume_status.
        //   "ok" | "unknown_token" | "session_attached" | "world_changed"
        const char* status = "unknown_token";
        // Why the identity clause ruled as it did.  Always set, always a string
        // literal (it is copied into a C-format ack).
        const char* identity_reason = "";
        // Non-empty only on "world_changed": the characters whose claims were
        // released and whose steering must be purged.  Ownership is already
        // erased by the time this is returned.
        std::vector<uint32_t> discarded_claims;
    };

    // Rebind a detached session to a new slot, presenting its resume_token.
    //
    // `presented` is the identity the client vouches for (empty = vouched for
    // nothing); `current_epoch` is ck-hxh7's world_epoch() (0 = the detector is
    // unavailable).  The rule, in full, is in resume_identity_verdict():
    //
    //   * same world epoch as at attach  -> ALLOW.  The world never changed, so
    //     there is nothing for an identity to disagree about.
    //   * the world changed and a declaration PROVES the same save -> ALLOW.
    //   * the world changed and nobody vouched -> DISCARD.  §6.2: "an UNVOUCHED
    //     world change must discard" — the bias here is the opposite of
    //     ck-hxh7's, because a false negative rewrites a stranger's personality
    //     for the rest of the episode and a false positive costs one reconnect.
    //
    // On DISCARD the claims are released, the steering-purge hook is fired, the
    // session is ERASED and the resume is REFUSED, so the client must handshake
    // fresh.  The erase is not tidiness: a claimless orphan left in the registry
    // is a phantom English agent whose unavailability span grows to the horizon
    // and invalidates the episode against X (§2.7, §6.2) — 5% of the declared
    // horizon, ck-2ngg; 365 at the registered one — for a client that
    // has already been told to start over.
    ResumeOutcome resume_by_token(const std::string& token,
                                  const WorldIdentity& presented,
                                  uint64_t current_epoch,
                                  int slot_idx, uint32_t slot_gen,
                                  uint64_t now_ms);

    // Stamp the save/world a session's claims are taken against, and the world
    // epoch they were taken in.  Empty / zero arguments leave the field alone.
    void set_session_world(Session* session, const WorldIdentity& world,
                           uint64_t epoch);

    // THE SECOND ENTRY POINT.  A resume can be presented BEFORE the harness has
    // declared (the client may declare_save_identity as a separate call, which
    // is the shape ck-9fc3 shipped and the observer lane needs).  In that case
    // the resume abstains, and this is where the answer lands when it arrives:
    // called from `declare_save_identity` with the identity now declared, it
    // discards every session whose RECORDED world the declaration refutes.
    //
    // Sessions that recorded no world are left alone: nobody vouched for the
    // world they claimed in, so nothing can refute it.  A declaration can only
    // ever say NO (world_identity.h).
    //
    // Returns the ids of the sessions it erased; `discarded_out` (optional)
    // receives every character id whose steering must be purged with them.
    std::vector<std::string> reconcile_declared_world(
            const WorldIdentity& declared, uint64_t current_epoch,
            std::vector<uint32_t>* discarded_out);

    // max_detach_s (design §6.2, ck-6kvq.1's abort ladder).  Promote every
    // DETACHED session detached longer than `max_detach_ms` to LOST, and return
    // their ids ONCE — on the sweep that promotes them, not on every sweep
    // after.  `max_detach_ms == 0` disables the bound.
    //
    // THIS RELEASES NOTHING.  Expiry is a REPORT: the episode is aborted from
    // outside with reason `session_lost`.  Releasing the claims here would be
    // precisely the mid-episode arm change R3 forbids, performed at the worst
    // possible moment — with nobody connected to notice.
    std::vector<std::string> sweep_detached(uint64_t now_ms,
                                            uint64_t max_detach_ms);

    // Every session currently in LOST (snapshot, ids ascending).
    std::vector<std::string> lost_sessions() const;

    // Fired when a resume or a reconciliation discards a session, with the
    // character ids whose per-character steering state must go with it — today
    // g_personality_locks (ck-9fc3), keyed by character id, which would
    // otherwise be reapplied 3x/tick to whoever now holds that id.
    //
    // Invoked with mutex_ NOT held, after ownership has been erased, so the
    // hook may call back into the registry.
    using SteeringPurgeHook =
        std::function<void(const std::string& session_id,
                           const std::vector<uint32_t>& char_ids)>;
    void set_steering_purge_hook(SteeringPurgeHook hook);

private:
    // Caller must hold mutex_.  The body of rebuild_legacy_mirrors().
    void rebuild_legacy_mirrors_locked();
    // Caller must hold mutex_.  Recomputes claim_seq bookkeeping for a session.
    void note_session_claim_locked(Session* session);
    void note_session_release_locked(Session* session);
    // Caller must hold mutex_.  Resolves the manifest-designated session, or
    // nullptr when no designation is set / it is not resolvable.
    Session* resolve_manifest_session_locked();

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<Session>> sessions_;
    std::unordered_map<uint32_t, std::string> ownership_; // char_id -> session_id
    std::atomic<uint64_t> next_counter_{1};

    // Deterministic-primary state (guarded by mutex_).
    uint64_t next_claim_seq_ = 1;
    uint32_t manifest_primary_char_ = 0;
    std::string manifest_primary_session_;  // bound owner, survives succession

    // Divergence state (guarded by mutex_).  Set by rebuild_legacy_mirrors when
    // the derived mirrors do not satisfy the ownership invariants.
    std::string divergence_;

    // Resume state (guarded by mutex_).  resume_token -> session_id.
    std::unordered_map<std::string, std::string> resume_tokens_;
    SteeringPurgeHook steering_purge_hook_;

    // Caller must hold mutex_.  Releases a session's claims, drops its token and
    // its manifest binding, and erases it.  `out_claims` receives the character
    // ids that were released, for the purge hook the CALLER must fire outside
    // the lock.  Used by both discard paths.
    void discard_session_locked(Session* session, std::vector<uint32_t>* out_claims);
};

// Global registry.  Defined in session.cpp.
extern SessionRegistry g_session_registry;

// ============================================================================
// THE ABORT LADDER'S DETACH REPORT (ck-6kvq.1 <-> ck-6kvq.5)
// ============================================================================
// ck-6kvq.1 shipped the `session_lost` rung and its READER, and the producer
// was never written — its fixtures fake the file by hand.  This is the producer,
// and it conforms to that shipped reader exactly rather than inventing a name
// (scripts/episode_abort.sh, DETACH_F):
//
//   path      $CK3_EPISODE_DIR/state/detached-since
//             (default $CK3_INSTANCE_DIR/episode/state/detached-since, exactly
//              as episode_abort.sh resolves EPISODE_DIR)
//   content   ONE integer: WALL-CLOCK EPOCH SECONDS at which the longest-running
//             current detach began.  Nothing else — the reader is
//             `since=$(tr -d '[:space:]' < "$DETACH_F")` and bash arithmetic.
//   absence   "no verdict".  The reader's test is `[ -s "$DETACH_F" ]`, so a
//             missing OR EMPTY file skips the rung entirely.
//   lifecycle written the instant a CONTROL session detaches, REMOVED the
//             instant the last one reattaches — the same discipline the script
//             already uses for STALL_F and GATE_F.  Leaving a stale timestamp
//             behind would abort a HEALTHY episode `session_lost` once
//             max_detach_s elapsed, which is the exact failure the rung exists
//             to catch, arriving as a false positive.
//   scope     CONTROL SESSIONS ONLY (ck-bw1s).  See below.
//
// It reports the EARLIEST outstanding detach, because the rung asks "has anyone
// been gone too long", not "has the most recent drop been long".
//
// CONTROL SESSIONS ONLY (ck-bw1s).  An OBSERVE session may still DETACH — the
// registry state is how get_bridge_state stays honest about a socket that went
// away — but its detach is NOT a verdict and never reaches this file.  An
// observer is not a participant in the arm: no claims, no row in the
// unavailability ledger (pipeline_note_session_open skips non-control sessions),
// nothing about the experimental condition changes when it goes.  Counting one
// here would let a dropped observer abort a HEALTHY episode `session_lost` — the
// same false positive as a stale timestamp, through a different door.
//
// THE OPERATOR RULE THIS REPLACES, kept because it explains the predicate
// (ck-zgh4, ck-bw1s): "any observe session in an experiment episode must be
// long-lived — a per-read observer manufactures one detach event per probe."
// THE CAREFUL THING TO DO WAS THE THING THAT BROKE IT: an author opens a fresh
// observe session per probe precisely so as not to hold state, and that is what
// triggered it.  The meter lane (ck-6kvq.7) is an observer by design.
//
// No-op unless the episode directory already EXISTS: no episode directory means
// no episode, and a free-play bridge must not start creating files for a ladder
// that is not watching.
void session_publish_detach_state();

// Test seam for the above: override the episode directory without touching the
// environment.  nullptr / "" restores the $CK3_EPISODE_DIR resolution.
void session_set_episode_dir_for_test(const char* dir);

// Generate a fresh session id.
std::string generate_session_id();

// Generate a fresh resume token.  Server-minted, like the session id.
std::string generate_resume_token();

// Session state <-> string, for handshake_ack / get_bridge_state.
const char* session_state_to_string(Session::State state);

// Convert mode to/from string (for handshake JSON).
const char* session_mode_to_string(Session::Mode mode);
Session::Mode session_mode_from_string(const char* s, Session::Mode default_mode);

// ============================================================================
// Dispatch helpers used by the action registry wrapper.
// ============================================================================

// Returns true if a ClientContext is currently installed.  If false and reason is
// non-null, it receives "no_client_context".  Used by the action registry wrapper
// so a handler dispatched outside of any session context fails loudly instead of
// silently inheriting a stale global primary.
bool require_dispatch_context(const char* action_type, std::string* reason);

// Returns true if the action type is a mutation (write / claim / intent enqueue)
// that should be refused while the caller is in OBSERVE mode.
bool action_is_mutation(const char* action_type);

// ── ck-obsv: MAY THIS SESSION DECLARE ITSELF THE EPISODE OBSERVER? ──────────
// A CONTROL session never may: the episode observer is THE READER, and the
// meter lane handshakes observe by design.
//
// A FLOOR UNDER AN EXPLICIT DECLARATION, NOT MODE-KEYED CONFERRAL, and the
// distinction is what makes it admissible. Conferral by mode would be
// non-specific — every observe session would drive the flush cadence, so any
// attached diagnostic would perturb the world it measures. This GRANTS NOTHING:
// the privilege is still taken only by explicit first-wins declaration, and
// this narrows WHO MAY DECLARE.
//
// WHAT IT CATCHES. First-wins is safe only if EXACTLY ONE client ever sends the
// flag, and an episode has several sessions. If two send it the privilege goes
// to whoever handshakes first, and THE WRONG WINNER FAILS SILENTLY: the
// read-lane trigger sits on a session that does no reading, reads keep stalling
// exactly as before the fix, and the recorded observer names a plausible
// session. This makes that case loud.
//
// THE CALLER MUST PASS THE MINTED MODE, never the requested one: null mode
// downgrades every client to observe, so judging the request would refuse a
// session the bridge then made read-only.
//
// Pure and total, so a host test can ask it without a socket or a pipeline.
bool episode_observer_may_declare(Session::Mode minted_mode);

// Who holds the privilege after `candidate` declares, given `current`. FIRST
// DECLARATION WINS: a second claimant is refused THE PRIVILEGE, not the
// session. Pure, so the policy is testable apart from the plumbing that applies
// it — two privileged readers would be two things driving the cadence, and the
// episode would have no single answer to "who drove it".
std::string episode_observer_winner(const std::string& current,
                                    const std::string& candidate);

// ── ck-m3n5: THE WHOLE DECISION, IN ONE PURE FUNCTION ──────────────────────
// The two policies above answer halves; this answers the question the bridge
// actually asks — SHOULD THIS DECLARATION CONFER THE PRIVILEGE, and if not, WHY
// — so that the applier in pipeline.cpp has no policy of its own to get wrong.
//
// WHY IT EXISTS. The mode floor used to be a branch at the ipc_server call site,
// which made the guarantee a property of ONE CALLER. ck-t5bc measured the cost:
// deleting the call redded the lane, but KEEPING THE SPELLING AND DECLARING
// ANYWAY passed 9/9 — a build that logged REFUSED and then handed the read-lane
// trigger to a control session, which is the exact defect the floor exists to
// prevent. A check that reds when a call is deleted and passes when it is
// neutered is asserting source text, not behaviour.
//
// So the decision moved to the conferral point and then into here, where a host
// probe can DRIVE it with no socket, no pipeline and no game. Pure and total.
enum class ObserverDecision {
    Confer,              // the privilege is this candidate's
    RefusedByMode,       // a CONTROL session may not hold it at all
    RefusedByFirstWins,  // somebody else declared first
};
ObserverDecision episode_observer_decision(const std::string& current,
                                           const std::string& candidate,
                                           Session::Mode minted_mode);

// Returns true if the action type is read-only (queries and pure bridge-state
// reads).  Observe-mode sessions may issue these freely.
bool action_is_readonly(const char* action_type);

// Validate that the resolved actor belongs to the current session.  If no
// session is installed, this is a no-op (legacy path).  If the actor is 0
// (actorless), the caller should resolve to the session primary instead.
//
// Returns true when the action may proceed.  On false, *reason receives a
// structured rejection reason (owned_by_other, no_actor, observe_mode, ...).
bool validate_actor_ownership(uint32_t actor_id, const char* action_type,
                              std::string* reason);

} // namespace bridge
