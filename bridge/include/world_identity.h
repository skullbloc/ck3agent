#pragma once
// world_identity.h — the SAVE/WORLD IDENTITY, and the rule that decides whether
// quarantined agent steering may be restored after a save is loaded into a live
// CK3 process (ck-9fc3, epic ck-6kvq R7).
//
// ═══════════════════════════════════════════════════════════════════════════
// THE QUESTION THIS ANSWERS
// ═══════════════════════════════════════════════════════════════════════════
// Two events look identical from inside the process:
//
//   (a) the SAME save was reloaded   -> agent steering must SURVIVE  (resume, R7)
//   (b) a DIFFERENT save was loaded  -> agent steering must be DISCARDED (ck-9fc3)
//
// (b) is not cosmetic. g_personality_locks is keyed by CHARACTER ID and
// apply_personality_locks() pokes the int8 axes three times per on_tick, and
// character ids are REUSED across saves. Guess (b) wrong and the bridge rewrites
// a stranger's personality several times a second for the rest of the process,
// with values belonging to a world that no longer exists — silently, and in a
// bundle that still looks clean.
//
// ck-hxh7 made the wrong answer UNREACHABLE by emptying the live write set on
// every detected reload and MOVING the entries to a quarantine. That killed the
// P1 and left exactly one thing open: quarantined locks are never re-applied, so
// a legitimate same-save resume loses its steering. This header is the missing
// half — the identity that says which of (a) and (b) just happened.
//
// ═══════════════════════════════════════════════════════════════════════════
// WHY THE IDENTITY IS DECLARED AND NOT INFERRED
// ═══════════════════════════════════════════════════════════════════════════
// "Is this the same save?" is a fact about a FILE. The bridge only ever sees a
// WORLD. Every in-process observable is either unstable across a legitimate
// reload or constant across an illegitimate one:
//
//   GAME_STATE_ROOT pointer  changes on EVERY load, a reload of the very same
//                            file included        -> cannot prove SAMENESS
//   START_DATE_YEAR          1066 for every 1066 save -> cannot prove DIFFERENCE
//   GAME_DATE                ditto at the bookmark date
//   character names / ids    explicitly untrustworthy — that is ck-hxh7 itself,
//                            and inferring identity from ids is the specific
//                            mistake this bead forbids
//
// So the bridge is TOLD, by the process that issues the load (the episode
// harness), through `declare_save_identity`. The identity is deliberately the
// one that already exists twice in this repository rather than a third one:
//
//   ck-6kvq.2  run_summary.json  "episode_id"  and  "save": {"sha256": ...}
//   ck3agent/server/session_memory.py's save key (playthrough_id > save stem)
//
// UNIFICATION NOTE, read before editing. ck-6kvq.5 (branch
// `ck-6kvq.5-session-resume`, OPEN and unmerged at the time of writing) defines
// the same record, under the same name, at this same path, for the CLAIMS half
// of resume. That is the point: one identity, two consumers. Neither branch may
// depend on the other (main is canonical), so the record is stated here
// independently and the two definitions are expected to MERGE into one — the
// field set and the `same_save_as` rule below are byte-compatible with it on
// purpose. Whoever merges second keeps a single definition; do not fork it.
//
// The two branches also carry the identity over the wire differently, and that
// is a merge decision, not a disagreement: ck-6kvq.5 presents it on the v2
// HANDSHAKE (where it authorises a claim set), this one on a `declare_save_identity`
// action (where it authorises restoring steering, which a session need not exist
// to do — an observer-mode harness declares before any control session attaches).
// Both are entry points to ONE decision. When the handshake lands, its `world`
// field must call world_identity_declare() rather than keep a parallel store;
// declaring twice with the same identity is idempotent apart from a second
// ledger row, which is by design.
//
// ═══════════════════════════════════════════════════════════════════════════
// WHAT THE WORLD IS STILL GOOD FOR: REFUTATION
// ═══════════════════════════════════════════════════════════════════════════
// A declaration is trusted to AFFIRM and never to overrule the world. The
// bridge reads START_DATE_YEAR out of the loaded world (ck-hxh7's fingerprint,
// which is a read of game memory, not an inference from any id) and refuses the
// match when the world visibly disagrees with what was declared. So the split
// is: the world can only ever say NO; only a declaration can say YES; and the
// default, when nobody has vouched for anything, is that the locks stay
// quarantined. A wrong refusal costs one re-declaration. A wrong restore
// corrupts the experiment.

#include <cstdint>
#include <string>

namespace bridge {

// ===========================================================================
// WorldIdentity — declared by the harness, one per loaded save.
// ===========================================================================
struct WorldIdentity {
    // ck-6kvq.2 run_summary.json "episode_id". Identifies a RUN, not a save.
    std::string episode_id;
    // ck-6kvq.2 run_summary.json save.sha256 — THE save key, over the bytes of
    // the save file. The value session_memory.py's save key is reaching for.
    std::string save_sha256;
    // Corroboration, cheap to cross-check against the live world. Never
    // sufficient on its own: every 1066 save carries 1066.
    uint32_t start_date_year = 0;

    // "Nobody vouched for a world." An identity is usable only when it carries
    // the save key; an episode_id alone names a run.
    bool empty() const { return save_sha256.empty(); }

    // Same SAVE. episode_id is NOT part of this on purpose: two episodes may
    // legitimately replay one save, and a resume inside one episode must not be
    // refused because the harness reminted its run id.
    bool same_save_as(const WorldIdentity& o) const {
        if (empty() || o.empty()) return false;
        if (save_sha256 != o.save_sha256) return false;
        // A start-year disagreement under a matching hash means one of the two
        // is lying about what it loaded. Refuse rather than trust.
        if (start_date_year && o.start_date_year &&
            start_date_year != o.start_date_year) return false;
        return true;
    }

    bool same_episode_as(const WorldIdentity& o) const {
        return !episode_id.empty() && episode_id == o.episode_id;
    }

    std::string to_json() const;
};

// ===========================================================================
// The verdict on "is the world now loaded the same save?"
// ===========================================================================
enum class SameSave {
    YES,
    NO_DECLARED_IDENTITY,   // nobody has vouched for the world now loaded
    NO_RECORDED_IDENTITY,   // nobody vouched for the world the locks came from
    DIFFERENT_SAVE,         // save_sha256 disagrees — THE ck-9fc3 case
    DIFFERENT_EPISODE,      // same save, different run (the disk carrier only)
    WORLD_CONTRADICTS,      // the loaded world disagrees with the declaration
};
const char* same_save_name(SameSave v);

// PURE decision function — the entire rule, in one place, no state, no I/O.
//
//   recorded                identity of the world the steering was set in
//   presented               identity declared for the world now loaded
//   world_start_date_year   START_DATE_YEAR read out of the live world; 0 means
//                           "unreadable", and the refutation clause abstains
//   require_same_episode    true for the on-disk carrier (see below), false for
//                           the in-process quarantine
//   why                     receives a stable, greppable sentence. Always set.
SameSave judge_same_save(const WorldIdentity& recorded,
                         const WorldIdentity& presented,
                         uint32_t world_start_date_year,
                         bool require_same_episode,
                         std::string* why);

// ===========================================================================
// The published decision
// ===========================================================================
// Every declaration produces exactly one of these, it is published on
// get_tick_info, and the harness reads `gap_game_days` off it to record the
// resume-to-steer gap (R7: how long an episode ran unsteered after a reload).
//
// `decision` is a CLOSED vocabulary; a consumer may switch on it:
//   pending_no_declaration                 locks are quarantined, nobody has
//                                          declared yet — the gap is OPEN and
//                                          `gap_game_days` grows every tick
//   restored_from_quarantine               same save, in-process resume
//   restored_from_disk                     same save + same episode, across a
//                                          process restart
//   nothing_to_restore                     no quarantine and no lock file
//   refused_no_declared_identity           declare carried no save_sha256
//   refused_no_recorded_identity           the carrier names no save
//   refused_different_save                 THE guard doing its job
//   refused_different_episode              disk carrier, another run's steering
//   refused_world_contradicts_declaration  the live world disagrees
//   refused_live_set_not_empty             the new world is already steered;
//                                          restoring would stomp it
struct RestoreRecord {
    uint64_t      epoch = 0;              // world epoch the decision was made in
    std::string   decision = "none";
    std::string   why;
    size_t        restored_count = 0;     // entries moved into the live write set
    size_t        quarantined_count = 0;  // entries still held in quarantine
    int32_t       reload_raw_date = 0;    // GAME_DATE at the reload that quarantined
    int32_t       decided_raw_date = 0;   // GAME_DATE when this decision was made
    int32_t       gap_game_days = -1;     // -1 = not computable (no reload seen)
    WorldIdentity recorded;
    WorldIdentity presented;

    // ck-81su: what this same decision did to the shared board's carrier.
    // "" / "none" when there was no carrier to resolve — every arm but B, every
    // free-play bridge, and every cold start. `board_json` is a BoardRelaunch,
    // pre-rendered rather than typed, so this header does not have to include
    // board.h (which includes this one).
    std::string board_decision;
    std::string board_json;

    std::string to_json() const;
};

// ---------------------------------------------------------------------------
// The declaration entry point. Called from the `declare_save_identity` IPC
// action. Records `presented` as the identity of the world now loaded, then
// decides — once — what happens to the quarantined steering.
// ---------------------------------------------------------------------------
RestoreRecord world_identity_declare(const WorldIdentity& presented);

// Identity currently declared for the loaded world (empty after a reload, until
// the harness declares again).
WorldIdentity world_identity_declared();

// Identity recorded against the quarantined lock set (empty when the world the
// locks came from was never vouched for).
WorldIdentity world_identity_quarantined();

// The wire fragment: a JSON OBJECT, spliced into get_tick_info under
// "save_identity". Always emitted, always well-formed, never throws.
std::string world_identity_emit_json();

// Registered with ck-hxh7's world-reset registry. Moves the declared identity
// to the quarantine slot and leaves the declared identity EMPTY — after a
// reload nobody has vouched for the new world, which is the whole point.
void world_identity_on_world_reset();

// Test seam: back to pristine (no declaration, no quarantine identity, empty
// ledger). Does NOT touch the personality tables.
void world_identity_reset_for_test();

}  // namespace bridge
