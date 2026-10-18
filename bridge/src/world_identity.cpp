// world_identity.cpp — the save/world identity and the restore decision
// (ck-9fc3). See include/world_identity.h for the rule and the reasoning.
//
// ENGINE-FREE BY CONSTRUCTION, like personality_world_reset.cpp next to it: two
// std::strings, a mutex, a bounded ledger and ck-hxh7's world fingerprint. No
// offsets, no character lookups, no engine calls. bridge/test/
// acceptance_save_identity links this file verbatim and drives the REAL
// decision with no CK3 binary anywhere — which is the only way the negative
// control ("a DIFFERENT save must not restore") is a proof and not a promise.

#include "world_identity.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT

#include "board.h"          // ck-81su: the board's carrier takes THIS decision
#include "game_date.h"
#include "personality_locks.h"
#include "world_epoch.h"

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

namespace bridge {

// Declared in bridge.h; redeclared so this TU stays engine-free. Null under the
// host acceptance, which is why every use is guarded.
extern FILE* g_logfile;

namespace {

std::mutex     g_mu;
WorldIdentity  g_declared CK3_EARLY_INIT;  // identity of the world now loaded
uint64_t       g_declared_epoch = 0;
WorldIdentity  g_quarantine_identity CK3_EARLY_INIT;  // world the locks came from
int32_t        g_reload_raw_date = 0;  // GAME_DATE at the reload that quarantined
bool           g_reload_seen = false;

constexpr size_t kLedgerMax = 8;
std::deque<RestoreRecord> g_ledger CK3_EARLY_INIT;

void wlog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void wlog(const char* fmt, ...) {
    if (!g_logfile) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logfile, fmt, ap);
    va_end(ap);
    fflush(g_logfile);
}

// Minimal JSON string escaping — these values are operator-supplied (an episode
// id, a hex digest), so the escape set is deliberately small and total.
std::string jstr(const std::string& v) {
    std::string o = "\"";
    for (char c : v) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (c == '"')       o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (uc < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", uc); o += b; }
        else o += c;
    }
    o += '"';
    return o;
}

std::string jdate(int32_t raw) {
    if (raw == 0) return "null";
    char buf[16];
    ck3::game_date::format_raw(raw, buf, sizeof(buf));  // ck-05mg: the one renderer
    return jstr(buf);
}

// Current world date, read through ck-hxh7's last fingerprint (a read of game
// memory taken on the game thread, not an inference from anything).
int32_t current_raw_date() {
    WorldFingerprint fp = world_last_fingerprint();
    return fp.loaded ? fp.game_date : 0;
}

uint32_t current_start_year() {
    WorldFingerprint fp = world_last_fingerprint();
    return fp.loaded ? static_cast<uint32_t>(fp.start_date_year) : 0u;
}

// Game days between the reload and `now`, or -1 when there is nothing to
// measure from. Clamped at 0: a same-save reload can legitimately rewind the
// clock, and a negative gap is not a gap.
int32_t gap_days_from(int32_t reload_raw, int32_t now_raw, bool reload_seen) {
    if (!reload_seen || reload_raw == 0 || now_raw == 0) return -1;
    int64_t d = static_cast<int64_t>(now_raw) - static_cast<int64_t>(reload_raw);
    if (d < 0) return 0;
    return static_cast<int32_t>(d / ck3::game_state::TICKS_PER_DAY);
}

RestoreRecord push_ledger(RestoreRecord rec) {
    g_ledger.push_back(rec);
    while (g_ledger.size() > kLedgerMax) g_ledger.pop_front();
    return rec;
}

}  // namespace

// ---------------------------------------------------------------------------
// WorldIdentity
// ---------------------------------------------------------------------------
std::string WorldIdentity::to_json() const {
    std::string o = "{\"episode_id\":";
    o += episode_id.empty() ? "null" : jstr(episode_id);
    o += ",\"save_sha256\":";
    o += save_sha256.empty() ? "null" : jstr(save_sha256);
    o += ",\"start_date_year\":";
    o += std::to_string(start_date_year);
    o += "}";
    return o;
}

// ---------------------------------------------------------------------------
// The rule
// ---------------------------------------------------------------------------
const char* same_save_name(SameSave v) {
    switch (v) {
        case SameSave::YES:                  return "yes";
        case SameSave::NO_DECLARED_IDENTITY: return "no_declared_identity";
        case SameSave::NO_RECORDED_IDENTITY: return "no_recorded_identity";
        case SameSave::DIFFERENT_SAVE:       return "different_save";
        case SameSave::DIFFERENT_EPISODE:    return "different_episode";
        // Concatenated onto "refused_" to form the published decision, so the
        // name is the documented vocabulary verbatim (E0 runbook §2.8).
        case SameSave::WORLD_CONTRADICTS:    return "world_contradicts_declaration";
    }
    return "unknown";
}

SameSave judge_same_save(const WorldIdentity& recorded,
                         const WorldIdentity& presented,
                         uint32_t world_start_date_year,
                         bool require_same_episode,
                         std::string* why) {
    auto say = [&](SameSave v, const std::string& msg) {
        if (why) *why = msg;
        return v;
    };

    // Order matters, and it is the order of what can be PROVEN.
    //
    // 1. Nobody vouched for the world now loaded. The default — steering stays
    //    quarantined — is not a failure, it is the guarantee.
    if (presented.empty()) {
        return say(SameSave::NO_DECLARED_IDENTITY,
                   "no save identity was declared for the world now loaded — "
                   "steering stays quarantined (declare_save_identity)");
    }

    // 2. Nobody vouched for the world the steering came from. Includes a
    //    lock file written before ck-9fc3 (no header) and an episode that
    //    steered without ever declaring.
    if (recorded.empty()) {
        return say(SameSave::NO_RECORDED_IDENTITY,
                   "the quarantined steering carries no save identity, so it "
                   "cannot be proven to belong to this save");
    }

    // 3. The world itself gets to REFUTE, never to affirm. START_DATE_YEAR is
    //    read out of the loaded world (ck-hxh7's fingerprint). 0 = unreadable,
    //    and the clause abstains rather than guessing.
    if (world_start_date_year && presented.start_date_year &&
        world_start_date_year != presented.start_date_year) {
        return say(SameSave::WORLD_CONTRADICTS,
                   "the loaded world's start year " + std::to_string(world_start_date_year) +
                   " contradicts the declared " + std::to_string(presented.start_date_year));
    }

    // 4. THE guard. A different save hash is the ck-9fc3 case outright.
    if (!recorded.same_save_as(presented)) {
        if (recorded.save_sha256 != presented.save_sha256) {
            return say(SameSave::DIFFERENT_SAVE,
                       "a DIFFERENT save is loaded (declared " + presented.save_sha256 +
                       ", steering belongs to " + recorded.save_sha256 + ")");
        }
        return say(SameSave::DIFFERENT_SAVE,
                   "the declared start year " + std::to_string(presented.start_date_year) +
                   " disagrees with the steering's " + std::to_string(recorded.start_date_year) +
                   " under a matching save hash — one of the two is wrong");
    }

    // 5. Disk carrier only: same save, different run. Two episodes may replay
    //    one save; they must not inherit each other's steering through the one
    //    /tmp path they share.
    if (require_same_episode && !recorded.same_episode_as(presented)) {
        return say(SameSave::DIFFERENT_EPISODE,
                   "the same save, but episode " + presented.episode_id +
                   " may not inherit steering recorded by episode " + recorded.episode_id);
    }

    return say(SameSave::YES, "same save: " + presented.save_sha256);
}

// ---------------------------------------------------------------------------
// RestoreRecord
// ---------------------------------------------------------------------------
std::string RestoreRecord::to_json() const {
    std::string o = "{\"epoch\":";
    o += std::to_string(epoch);
    o += ",\"decision\":" + jstr(decision);
    o += ",\"why\":" + jstr(why);
    o += ",\"restored_count\":" + std::to_string(restored_count);
    o += ",\"quarantined_count\":" + std::to_string(quarantined_count);
    o += ",\"reload_game_date\":" + jdate(reload_raw_date);
    o += ",\"decided_game_date\":" + jdate(decided_raw_date);
    o += ",\"gap_game_days\":";
    o += (gap_game_days < 0) ? "null" : std::to_string(gap_game_days);
    o += ",\"recorded\":" + recorded.to_json();
    o += ",\"presented\":" + presented.to_json();
    // ck-81su: what the SAME decision did to the board's carrier. "none" when
    // there was no carrier to resolve — which is every arm but B, every free-play
    // bridge, and every cold start.
    o += ",\"board\":{\"decision\":" + jstr(board_decision.empty() ? "none" : board_decision);
    if (!board_json.empty()) o += ",\"relaunch\":" + board_json;
    o += "}";
    o += "}";
    return o;
}

// ---------------------------------------------------------------------------
// The declaration
// ---------------------------------------------------------------------------
RestoreRecord world_identity_declare(const WorldIdentity& presented) {
    std::lock_guard<std::mutex> lock(g_mu);

    // Captured BEFORE the declaration below overwrites it: re-declaring the same
    // identity for a world that is already steered has to be recognisable as the
    // no-op it is, not reported as a refusal.
    const WorldIdentity prior_declared = g_declared;
    const uint64_t      prior_epoch    = g_declared_epoch;

    RestoreRecord rec;
    rec.epoch            = world_epoch();
    rec.presented        = presented;
    rec.decided_raw_date = current_raw_date();
    rec.reload_raw_date  = g_reload_raw_date;
    rec.gap_game_days    = gap_days_from(g_reload_raw_date, rec.decided_raw_date, g_reload_seen);

    // The declaration stands even when the restore is refused: a refused world
    // is still a KNOWN world, and the next reload must have something to record.
    if (!presented.empty()) {
        g_declared       = presented;
        g_declared_epoch = rec.epoch;
        // Push, do not let the writer pull — see set_personality_locks_identity().
        set_personality_locks_identity(presented);
    }

    const uint32_t world_year = current_start_year();
    const size_t   quarantined = personality_quarantine_count();
    std::string    why;

    // ═══ ck-81su: THE BOARD'S CARRIER, judged HERE and only here ═══════════
    // The board is arm B's treatment, and a resume that relaunched CK3 used to
    // bring it back empty with seq restarting at 0. Its posts now survive in a
    // carrier, and this is the one place that may adopt them — the SAME
    // judge_same_save(), on the SAME declaration, in the SAME breath as the
    // personality-lock restore, so there is exactly one notion of "the same
    // save" in this bridge and the board does not get a second one.
    //
    // require_same_episode is TRUE, for the disk carrier's reason: two episodes
    // may legitimately replay one save, and a second episode inheriting the
    // first's board would be a treatment record spliced across two runs.
    //
    // It runs BEFORE the branching below because every path out of this function
    // is a declaration, and the board must be resolved on all of them. Nothing
    // here can throw and nothing here touches the personality tables.
    {
        WorldIdentity board_recorded;
        if (board_carrier_pending(&board_recorded)) {
            std::string bwhy;
            const SameSave bv = judge_same_save(board_recorded, presented, world_year,
                                                /*require_same_episode=*/true, &bwhy);
            const BoardRelaunch br =
                (bv == SameSave::YES)
                    ? board_carrier_accept(presented, rec.epoch, bwhy)
                    : board_carrier_reject(presented, rec.epoch,
                                           std::string("refused_") + same_save_name(bv),
                                           bwhy);
            rec.board_decision = br.decision;
            rec.board_json     = br.json();
        }
    }

    // And stamp the carrier with the identity now declared, so the posts THIS
    // process writes are vouched for when the NEXT one finds them. Pushed, not
    // pulled, for the same lock-order reason as set_personality_locks_identity().
    if (!presented.empty())
        board_carrier_note_identity(presented, rec.epoch, board().next_seq());

    if (quarantined > 0) {
        // ── In-process resume: the locks are right here, in quarantine. ──
        rec.recorded = g_quarantine_identity;
        SameSave v = judge_same_save(g_quarantine_identity, presented, world_year,
                                     /*require_same_episode=*/false, &why);
        rec.why = why;
        if (v == SameSave::YES) {
            rec.restored_count = restore_quarantined_personality_locks();
            rec.decision       = "restored_from_quarantine";
            g_quarantine_identity = presented;   // the live set now belongs here
        } else {
            rec.decision = std::string("refused_") + same_save_name(v);
        }
        rec.quarantined_count = personality_quarantine_count();
        wlog("[CK3-Bridge] SAVE-IDENTITY: %s — %s (epoch=%llu, restored=%zu, "
             "quarantined=%zu, gap=%d game-days)\n",
             rec.decision.c_str(), rec.why.c_str(),
             static_cast<unsigned long long>(rec.epoch), rec.restored_count,
             rec.quarantined_count, rec.gap_game_days);
        return push_ledger(rec);
    }

    // ── Nothing in quarantine. A repeat declaration for a world that is
    //    already declared and already steered is idempotent, and says so. ──
    if (prior_epoch == rec.epoch && !prior_declared.empty() &&
        prior_declared.same_save_as(presented) && personality_lock_count() > 0) {
        rec.recorded = prior_declared;
        rec.decision = "nothing_to_restore";
        rec.why      = "this save is already declared for the loaded world and its steering is "
                       "live (" + std::to_string(personality_lock_count()) + " locks)";
        return push_ledger(rec);
    }

    // ── Cross-process resume: nothing in quarantine, so the only thing that
    //    could carry steering into this world is the on-disk file. ──
    WorldIdentity on_disk;
    if (!peek_personality_locks_identity(&on_disk)) {
        // No file, or a file with no header. A headerless file is the
        // pre-ck-9fc3 format and names no save: it can never match, so it is
        // reported rather than silently ignored.
        rec.decision = "nothing_to_restore";
        rec.why      = "no quarantined steering and no identified lock file at ";
        rec.why     += personality_locks_path();
        rec.quarantined_count = 0;
        return push_ledger(rec);
    }

    rec.recorded = on_disk;

    if (personality_lock_count() > 0) {
        rec.decision = "refused_live_set_not_empty";
        rec.why      = "the world now loaded is already steered (" +
                       std::to_string(personality_lock_count()) +
                       " locks); restoring the lock file would stomp it";
        wlog("[CK3-Bridge] SAVE-IDENTITY: %s — %s\n", rec.decision.c_str(), rec.why.c_str());
        return push_ledger(rec);
    }

    SameSave v = judge_same_save(on_disk, presented, world_year,
                                 /*require_same_episode=*/true, &why);
    rec.why = why;
    if (v == SameSave::YES) {
        rec.restored_count = load_personality_locks_from_disk();
        rec.decision       = "restored_from_disk";
        g_quarantine_identity = presented;
    } else {
        rec.decision = std::string("refused_") + same_save_name(v);
    }
    wlog("[CK3-Bridge] SAVE-IDENTITY: %s — %s (epoch=%llu, restored=%zu, gap=%d game-days)\n",
         rec.decision.c_str(), rec.why.c_str(),
         static_cast<unsigned long long>(rec.epoch), rec.restored_count, rec.gap_game_days);
    return push_ledger(rec);
}

WorldIdentity world_identity_declared() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_declared;
}

WorldIdentity world_identity_quarantined() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_quarantine_identity;
}

// ---------------------------------------------------------------------------
// The wire
// ---------------------------------------------------------------------------
std::string world_identity_emit_json() {
    std::lock_guard<std::mutex> lock(g_mu);

    const int32_t now_raw     = current_raw_date();
    const size_t  quarantined = personality_quarantine_count();
    const size_t  live        = personality_lock_count();

    std::string o = "{\"epoch\":" + std::to_string(world_epoch());
    o += ",\"declared\":" + g_declared.to_json();
    o += ",\"declared_at_epoch\":" + std::to_string(g_declared_epoch);
    o += ",\"world_start_date_year\":" + std::to_string(current_start_year());
    o += ",\"world_game_date\":" + jdate(now_raw);
    o += ",\"live_lock_count\":" + std::to_string(live);
    o += ",\"quarantine\":{\"count\":" + std::to_string(quarantined);
    o += ",\"epoch\":" + std::to_string(personality_quarantine_epoch());
    o += ",\"identity\":" + g_quarantine_identity.to_json();
    o += ",\"reload_game_date\":" + jdate(g_reload_raw_date);
    o += "}";

    // THE RESUME-TO-STEER GAP. Quarantined steering plus no declaration is an
    // OPEN gap: the episode is running unsteered right now, and the number grows
    // every tick until a declaration closes it. A harness samples exactly this.
    const bool open_gap = (quarantined > 0) && g_declared.empty();
    o += ",\"resume_gap\":{\"open\":";
    o += open_gap ? "true" : "false";
    o += ",\"game_days\":";
    int32_t gap = gap_days_from(g_reload_raw_date, now_raw, g_reload_seen);
    o += (gap < 0) ? "null" : std::to_string(gap);
    o += "}";

    // `last_decision` describes the world NOW LOADED, which is why an open gap
    // outranks the ledger: after a reload the newest ledger entry belongs to the
    // world that just went away, and publishing it would tell a harness that
    // steering is live when it is quarantined. The entry stays in `decisions`.
    if (open_gap || g_ledger.empty()) {
        o += ",\"last_decision\":{\"decision\":";
        o += open_gap ? "\"pending_no_declaration\"" : "\"none\"";
        o += ",\"why\":";
        o += open_gap
                 ? jstr("a reload quarantined agent steering and no save identity "
                        "has been declared for the world now loaded")
                 : jstr("no save identity has been declared in this process");
        o += ",\"gap_game_days\":";
        o += (gap < 0) ? "null" : std::to_string(gap);
        o += "}";
    } else {
        o += ",\"last_decision\":" + g_ledger.back().to_json();
    }

    o += ",\"decisions\":[";
    bool first = true;
    for (const auto& r : g_ledger) {
        if (!first) o += ",";
        first = false;
        o += r.to_json();
    }
    o += "]";

    o += "}";
    return o;
}

// ---------------------------------------------------------------------------
// World reset
// ---------------------------------------------------------------------------
void world_identity_on_world_reset() {
    std::lock_guard<std::mutex> lock(g_mu);

    // The world the steering was set in becomes the RECORDED identity, and the
    // world now loaded is unvouched-for until somebody declares. Note the
    // asymmetry with ck-hxh7's merge-on-repeat-reload quarantine: the identity
    // of the FIRST world is what the quarantined locks belong to, so a second
    // reload before any declaration must not overwrite it with an empty one.
    if (!g_declared.empty()) g_quarantine_identity = g_declared;
    g_declared = WorldIdentity{};
    g_declared_epoch = 0;
    // A file written now would name no save — correct: after a reload nobody
    // has vouched for the world the locks would be written from.
    set_personality_locks_identity(WorldIdentity{});

    // ck-hxh7 records the new sample BEFORE firing the resets, so this is the
    // date the NEW world came up — the point the resume-to-steer gap is
    // measured from.
    WorldFingerprint fp = world_last_fingerprint();
    g_reload_raw_date = fp.loaded ? fp.game_date : 0;
    g_reload_seen     = true;
}

void world_identity_reset_for_test() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_declared = WorldIdentity{};
    g_declared_epoch = 0;
    g_quarantine_identity = WorldIdentity{};
    g_reload_raw_date = 0;
    g_reload_seen = false;
    g_ledger.clear();
    set_personality_locks_identity(WorldIdentity{});
}

// Register once, at .so load — same mechanism personality_world_reset.cpp uses.
// Registration order against the personality quarantine does not matter: this
// callback touches only the identity slots and reads the fingerprint, and the
// personality callback touches only the lock tables.
[[maybe_unused]] static const WorldResetRegistrar g_world_identity_world_scoped CK3_EARLY_INIT([] {
    register_world_reset("world_identity", &world_identity_on_world_reset);
});

}  // namespace bridge
