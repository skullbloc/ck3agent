// test/acceptance_session_resume.cpp — ck-6kvq.5 ACCEPTANCE (R7c / R2a).
//
// THE CLAIM UNDER TEST, in one sentence: a control session SURVIVES a dropped
// socket with its claims intact, comes back on a resume token, gets its agent
// steering back when — and only when — the loaded world is provably the same
// save, and the game-days it spent away are RECORDED as unavailability rather
// than quietly erased by the reconnect.
//
// WHY IT MATTERS. M1 tied session lifetime to the socket: a drop destroyed the
// session and released its claims (design §1.4 defect 1, ipc_server.cpp:195-197).
// In an experiment episode that is an uncontrolled change to the FROZEN ARM,
// produced by a network event — a subscription-cap disconnect silently hands a
// claimed English earl back to the native AI, and the episode goes on looking
// clean. ck-6kvq.10 stopped the destroy under `episode_pins_claims()`; this is
// the other half, and without it a dropped socket leaves a session nobody can
// ever reach again.
//
// NO GAME. This is a host binary. It links the REAL session.cpp, the REAL
// world_identity.cpp (ck-9fc3 — ONE identity rule, not a second copy), the REAL
// world_epoch.cpp (ck-hxh7) and the REAL unavailability.cpp / episode_policy.cpp
// (ck-6kvq.10), and drives world reloads through the documented
// world_epoch_observe() seam. Every decision under test is the shipped one.
//
// THE NEGATIVE CONTROLS ARE THE POINT — there are two, and each fails on its own
// lanes. An acceptance that cannot fail is not evidence.
//
//   -DCK3_RESUME_NO_IDENTITY_CHECK   the resume identity clause always allows,
//     and the ck-9fc3 symptom comes straight back: a session keeps its claims,
//     and therefore its personality steering, over characters in a world it
//     never saw. Fails the same-save / different-save lanes.
//
//   -DCK3_DETACH_REPORT_COUNTS_OBSERVERS   (ck-bw1s) the detach report counts
//     EVERY non-attached session, which is what it did before this bead. Fails
//     lane 11: a dropped OBSERVE session publishes state/detached-since, the
//     abort ladder reads it as reason=session_lost, and a healthy arm episode is
//     aborted because an observer blinked.

#include "bridge.h"
#include "session.h"
#include "personality_locks.h"
#include "unavailability.h"
#include "episode_policy.h"
#include "world_epoch.h"
#include "world_identity.h"
#include "game_date.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <shared_mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// Stub globals referenced by bridge.h / session.cpp when compiled standalone
// (the test_session_registry + acceptance_save_identity pattern).
namespace bridge {
FILE*                  g_logfile = nullptr;
uintptr_t              g_base_addr = 0;
std::vector<MemRange>  g_valid_ranges;
std::shared_mutex      g_ranges_mutex;
std::mutex             g_controlled_mutex;
std::vector<uint32_t>  g_controlled_chars;
std::atomic<uint32_t>  g_controlled_primary{0};
std::atomic<uint32_t>  g_controlled_heir{0};
std::atomic<bool>      g_agent_mode{false};
}

using namespace bridge;

// ---------------------------------------------------------------------------
// Reporting: report every check, never abort on the first. The negative control
// is read by counting failures, so an early abort would hide most of them.
// ---------------------------------------------------------------------------
static int g_checks = 0;
static int g_fails  = 0;

static void check(const char* lane, bool ok, const char* what,
                  const std::string& detail = "") {
    g_checks++;
    if (!ok) g_fails++;
    printf("  [%-10s] %-4s %s%s%s\n", lane, ok ? "ok" : "FAIL", what,
           detail.empty() ? "" : " — ", detail.c_str());
}

static void check_eq_sz(const char* lane, size_t actual, size_t expected,
                        const char* what) {
    char d[128];
    snprintf(d, sizeof(d), "expected %zu, got %zu", expected, actual);
    check(lane, actual == expected, what, actual == expected ? "" : d);
}

static void check_eq_i64(const char* lane, int64_t actual, int64_t expected,
                         const char* what) {
    char d[128];
    snprintf(d, sizeof(d), "expected %lld, got %lld",
             (long long)expected, (long long)actual);
    check(lane, actual == expected, what, actual == expected ? "" : d);
}

// ---------------------------------------------------------------------------
// Fixtures. Two DIFFERENT saves of the SAME 1066 bookmark — the hard case, and
// the reason the identity cannot be inferred from the world: a start-year check
// alone cannot tell these apart.
// ---------------------------------------------------------------------------
static const char* kSaveA    = "0c94b59880e0549a5a0c8fbe4dc675a3de171f28d79f7628ad765116de782d5d";
static const char* kSaveB    = "7f21aa0c1d3e4b5968708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6";
static const char* kEpisodeA = "ep-20260912T033051Z-e0-paper-9eae1e0e";
static const char* kEpisodeB = "ep-20260912T104500Z-a-pilot-11112222";

// The English side's characters, as the 1066 arm claims them.
static const uint32_t kEarlA = 6001;
static const uint32_t kEarlB = 6002;

static WorldIdentity ident(const char* episode, const char* sha, uint32_t year = 1066) {
    WorldIdentity id;
    id.episode_id      = episode ? episode : "";
    id.save_sha256     = sha ? sha : "";
    id.start_date_year = year;
    return id;
}

// Dates go through the ONE renderer's own inverse (ck-05mg), so a fixture can
// never drift from what the bridge will print.
static int32_t raw_for(int y, int m, int d) { return ck3::game_date::ymd_to_raw(y, m, d); }

static WorldFingerprint world_fp(uintptr_t gs, int32_t raw) {
    WorldFingerprint f;
    f.loaded          = true;
    f.game_state_ptr  = gs;
    f.start_date_year = 1066;
    f.game_date       = raw;
    return f;
}

static PersonalityLock lock_with(int8_t v) {
    PersonalityLock pl{};
    for (int i = 0; i < 10; i++) { pl.axes[i] = v; pl.locked[i] = true; }
    return pl;
}

static void set_live_locks(const std::vector<std::pair<uint32_t, int8_t>>& entries) {
    std::unique_lock<std::shared_mutex> lk(g_personality_lock_mutex);
    g_personality_locks.clear();
    g_quarantined_personality_locks.clear();
    for (const auto& e : entries) g_personality_locks[e.first] = lock_with(e.second);
}

static bool live_has(uint32_t cid, int8_t v) {
    std::shared_lock<std::shared_mutex> lk(g_personality_lock_mutex);
    auto it = g_personality_locks.find(cid);
    return it != g_personality_locks.end() && it->second.axes[0] == v && it->second.locked[0];
}

static bool quarantine_has(uint32_t cid) {
    std::shared_lock<std::shared_mutex> lk(g_personality_lock_mutex);
    return g_quarantined_personality_locks.count(cid) != 0;
}

// ---------------------------------------------------------------------------
// Slate management. Every lane starts from nothing: the registry is a process
// global and a leftover session would make a later lane's count meaningless.
// ---------------------------------------------------------------------------
static std::string g_tmp_lockfile;
static std::string g_tmp_episode_dir;

static void wipe_registry() {
    for (const std::string& sid : g_session_registry.ids())
        g_session_registry.destroy(sid);
    g_session_registry.set_manifest_primary(0);
}

static void clean_slate() {
    wipe_registry();
    world_epoch_reset_for_test();
    world_identity_reset_for_test();
    set_live_locks({});
    unavailability().reset();
}

// The purge hook the bridge installs from bridge_init() (personality.cpp's
// install_steering_purge_hook). Reproduced here rather than linked, because
// personality.cpp is an ENGINE TU and this binary must build on a host with no
// CK3: what is under test is that the registry FIRES the hook with the right
// character ids, which is the half that can be wrong.
static std::atomic<int> g_purge_calls{0};
static void install_test_purge_hook() {
    g_purge_calls = 0;
    g_session_registry.set_steering_purge_hook(
        [](const std::string&, const std::vector<uint32_t>& ids) {
            g_purge_calls++;
            std::unique_lock<std::shared_mutex> lk(g_personality_lock_mutex);
            for (uint32_t cid : ids) {
                g_personality_locks.erase(cid & 0xFFFFFF);
                g_quarantined_personality_locks.erase(cid & 0xFFFFFF);
            }
        });
}

// A session that has claimed its characters and is bound to a slot — the state
// every lane starts a drop from.
struct Bound {
    Session* s = nullptr;
    std::string sid;
    std::string token;
};
static Bound bind_session(const char* name, int slot, uint32_t gen,
                          const std::vector<uint32_t>& claims,
                          const WorldIdentity& world, uint64_t epoch,
                          uint64_t now_ms,
                          Session::Mode mode = Session::Mode::CONTROL) {
    Bound b;
    b.s = g_session_registry.create(name, mode);
    b.sid   = b.s->session_id;
    b.token = b.s->resume_token;
    for (uint32_t c : claims) g_session_registry.claim_character(b.s, c, nullptr);
    g_session_registry.attach_to_slot(b.s, slot, gen, now_ms);
    g_session_registry.set_session_world(b.s, world, epoch);
    return b;
}

// ===========================================================================
// 1. DETACH, NOT DESTROY — the M1 defect, closed.
// ===========================================================================
static void lane_detach() {
    printf("\n1. DETACH, NOT DESTROY — a dropped socket keeps the session and its claims\n");
    const char* LANE = "detach";
    clean_slate();

    Bound b = bind_session("english-1", /*slot*/3, /*gen*/7, {kEarlA, kEarlB},
                           ident(kEpisodeA, kSaveA), /*epoch*/1, /*now*/1000);
    g_session_registry.claim_character(b.s, kEarlA, nullptr);   // primary = kEarlA
    b.s->set_primary(kEarlA);
    const uint64_t claim_seq_before = b.s->claim_seq;

    check(LANE, b.s->state == Session::State::ATTACHED, "a fresh session is ATTACHED");
    check(LANE, !b.token.empty(), "and carries a SERVER-MINTED resume token");
    check(LANE, b.token.rfind("rsm_", 0) == 0,
          "whose shape is the server's, not the client's (rsm_ prefix)");

    // THE DROP.
    const bool detached = g_session_registry.detach_from_slot(b.sid, 3, 7, 2000);
    check(LANE, detached, "the slot release DETACHES the session");

    // The whole point: nothing was destroyed and nothing was released.
    check_eq_sz(LANE, g_session_registry.ids().size(), 1,
                "the session is STILL IN THE REGISTRY (destroy-on-drop is gone)");
    check(LANE, g_session_registry.find(b.sid) != nullptr, "and still findable by id");
    check(LANE, b.s->state == Session::State::DETACHED, "its state is DETACHED");
    check_eq_sz(LANE, b.s->claimed_character_ids.size(), 2, "both claims RETAINED");
    check(LANE, g_session_registry.owner_of(kEarlA) == b.s,
          "the ownership entry for the earl is retained");
    check(LANE, g_session_registry.owner_of(kEarlB) == b.s,
          "and for the second claimed character");
    check(LANE, b.s->primary_character_id == kEarlA, "the primary is retained");
    check_eq_i64(LANE, (int64_t)b.s->claim_seq, (int64_t)claim_seq_before,
                 "and claim_seq is untouched, so the global primary cannot move on a socket event");
    check(LANE, b.s->bound_slot_idx == -1, "the slot binding is dropped (nothing to send to)");
}

// ===========================================================================
// 2. THE STALE-GENERATION GUARD — a dead socket cannot close a live session.
// ===========================================================================
static void lane_stale_generation() {
    printf("\n2. STALE GENERATION — a late close from a dead socket changes NOTHING\n");
    const char* LANE = "stale-gen";
    clean_slate();

    Bound b = bind_session("english-1", 3, 7, {kEarlA},
                           ident(kEpisodeA, kSaveA), 1, 1000);
    g_session_registry.detach_from_slot(b.sid, 3, 7, 2000);

    // The client reconnects on a DIFFERENT slot.
    auto r = g_session_registry.resume_by_token(b.token, ident(kEpisodeA, kSaveA),
                                                /*epoch*/1, /*slot*/5, /*gen*/2, 3000);
    check(LANE, r.ok, "the client reconnects and resumes on a new slot");

    // NOW the old socket's reap arrives — a peer-pid probe or a keepalive sweep
    // firing on a connection that is already gone. Honouring it would take a
    // LIVE session's claims offline: the precise failure §6.2 names.
    const bool late = g_session_registry.detach_from_slot(b.sid, 3, 7, 4000);
    check(LANE, !late, "a late close carrying the OLD slot binding is REFUSED");
    check(LANE, b.s->state == Session::State::ATTACHED,
          "the session is still ATTACHED");
    check(LANE, b.s->bound_slot_idx == 5 && b.s->bound_slot_gen == 2,
          "still bound to the slot it reconnected on");
    check(LANE, g_session_registry.owner_of(kEarlA) == b.s, "and still owns its earl");

    // Same slot index, older generation — the slot was recycled underneath.
    const bool recycled = g_session_registry.detach_from_slot(b.sid, 5, 1, 5000);
    check(LANE, !recycled,
          "a close on the RIGHT slot but an older generation is refused too");
    check(LANE, b.s->state == Session::State::ATTACHED, "still ATTACHED after it");

    // And the binding it actually holds does close.
    check(LANE, g_session_registry.detach_from_slot(b.sid, 5, 2, 6000),
          "the close that matches the CURRENT binding is honoured");
}

// ===========================================================================
// 3. RESUME BY TOKEN — the claim set comes back byte-identical.
// ===========================================================================
static void lane_resume() {
    printf("\n3. RESUME BY TOKEN — claims, primary and claim order survive the gap\n");
    const char* LANE = "resume";
    clean_slate();

    Bound b = bind_session("english-1", 0, 1, {kEarlA, kEarlB},
                           ident(kEpisodeA, kSaveA), 1, 1000);
    b.s->set_primary(kEarlB);
    // A second session, so "the primary did not move" is a claim with teeth.
    Bound other = bind_session("english-2", 1, 1, {7001},
                               ident(kEpisodeA, kSaveA), 1, 1000);
    const uint64_t seq_before = b.s->claim_seq;
    const std::set<uint32_t> claims_before = b.s->claimed_character_ids;
    const uint32_t primary_before = b.s->primary_character_id;

    g_session_registry.detach_from_slot(b.sid, 0, 1, 2000);

    // Wrong token: refused, and nothing is disturbed.
    auto bad = g_session_registry.resume_by_token("rsm_not_a_real_token",
                                                  ident(kEpisodeA, kSaveA), 1, 4, 1, 3000);
    check(LANE, !bad.ok, "an unknown token is refused");
    check(LANE, std::string(bad.status) == "unknown_token", "with status unknown_token");
    check(LANE, b.s->state == Session::State::DETACHED,
          "and the real session is untouched by the failed attempt");

    // An ATTACHED session is NOT resumable: two clients holding one claim set is
    // a split brain, and the second is far likelier to be a confused retry.
    auto split = g_session_registry.resume_by_token(other.token,
                                                    ident(kEpisodeA, kSaveA), 1, 6, 1, 3100);
    check(LANE, !split.ok, "a session that is still ATTACHED cannot be resumed");
    check(LANE, std::string(split.status) == "session_attached",
          "with status session_attached (split brain refused)");

    // The real thing.
    auto r = g_session_registry.resume_by_token(b.token, ident(kEpisodeA, kSaveA),
                                                1, /*slot*/4, /*gen*/1, 4000);
    check(LANE, r.ok, "the stored token rebinds the session to the new slot");
    check(LANE, r.session == b.s, "and it is the SAME session object, not a fresh one");
    check(LANE, b.s->state == Session::State::ATTACHED, "which is ATTACHED again");
    check(LANE, b.s->bound_slot_idx == 4 && b.s->bound_slot_gen == 1,
          "bound to the slot it reconnected on");
    check(LANE, b.s->claimed_character_ids == claims_before,
          "THE CLAIM SET IS BYTE-IDENTICAL across the interruption");
    check_eq_i64(LANE, (int64_t)b.s->primary_character_id, (int64_t)primary_before,
                 "the session primary is restored");
    check_eq_i64(LANE, (int64_t)b.s->claim_seq, (int64_t)seq_before,
                 "claim order is unchanged, so the GLOBAL primary did not move");
    check_eq_sz(LANE, g_session_registry.ids().size(), 2, "and no session was leaked");
}

// ===========================================================================
// 4. THE SAME SAVE — steering comes back.
// ===========================================================================
static void lane_same_save() {
    printf("\n4. SAME SAVE — a reload of the same file gives the steering back\n");
    const char* LANE = "same-save";
    clean_slate();
    install_test_purge_hook();

    // The episode is running: the harness declared save A, and the agent steered.
    world_identity_declare(ident(kEpisodeA, kSaveA));
    set_live_locks({{kEarlA, 90}, {kEarlB, 40}});
    world_epoch_observe(world_fp(0xA000, raw_for(1066, 9, 15)));
    const uint64_t epoch_a = world_epoch();

    Bound b = bind_session("english-1", 0, 1, {kEarlA, kEarlB},
                           ident(kEpisodeA, kSaveA), epoch_a, 1000);

    // The socket drops, and then the SAME save is reloaded into the live process.
    g_session_registry.detach_from_slot(b.sid, 0, 1, 2000);
    world_epoch_observe(world_fp(0xB000, raw_for(1066, 9, 15)));   // pointer moved: a load
    const uint64_t epoch_b = world_epoch();
    check(LANE, epoch_b != epoch_a, "ck-hxh7 sees the reload (the epoch advanced)");
    check(LANE, !live_has(kEarlA, 90),
          "and the live write set was emptied — the ck-hxh7 P1 fix still holds");
    check(LANE, quarantine_has(kEarlA), "the entries are in QUARANTINE, not destroyed");

    // The client reconnects and DECLARES the save it loaded — the client half of
    // the resume path (§6.2: the bridge cannot derive this, it must be told).
    auto r = g_session_registry.resume_by_token(b.token, ident(kEpisodeA, kSaveA),
                                                epoch_b, 4, 1, 3000);
    check(LANE, r.ok, "the resume is ALLOWED: the declaration proves the same save");
    check(LANE, std::string(r.identity_reason).find("same_save") != std::string::npos,
          "for the stated reason", r.identity_reason);
    check_eq_sz(LANE, b.s->claimed_character_ids.size(), 2, "with its claims intact");
    check_eq_i64(LANE, g_purge_calls.load(), 0, "and nothing was purged");

    RestoreRecord rec = world_identity_declare(ident(kEpisodeA, kSaveA));
    check(LANE, rec.decision == "restored_from_quarantine",
          "declare_save_identity RESTORES the quarantined steering", rec.decision);
    check(LANE, live_has(kEarlA, 90), "the earl's locks are live again");
    check(LANE, live_has(kEarlB, 40), "and the second character's");

    // The episode id may legitimately be reminted (two episodes may replay one
    // save); that must not refuse a resume.
    g_session_registry.detach_from_slot(b.sid, 4, 1, 4000);
    auto r2 = g_session_registry.resume_by_token(b.token, ident(kEpisodeB, kSaveA),
                                                 epoch_b, 5, 1, 5000);
    check(LANE, r2.ok,
          "a reminted episode_id over the SAME save still resumes (episode_id is "
          "excluded from same_save_as on purpose)");
}

// ===========================================================================
// 5. A DIFFERENT SAVE — the resume is REFUSED and the session is ERASED.
//    THIS IS THE LANE THE NEGATIVE CONTROL FAILS.
// ===========================================================================
static void lane_different_save() {
    printf("\n5. DIFFERENT SAVE — claims are released, the session erased, steering purged\n");
    const char* LANE = "diff-save";
    clean_slate();
    install_test_purge_hook();

    world_identity_declare(ident(kEpisodeA, kSaveA));
    set_live_locks({{kEarlA, 90}, {kEarlB, 40}});
    world_epoch_observe(world_fp(0xA000, raw_for(1066, 9, 15)));
    const uint64_t epoch_a = world_epoch();

    Bound b = bind_session("english-1", 0, 1, {kEarlA, kEarlB},
                           ident(kEpisodeA, kSaveA), epoch_a, 1000);
    const std::string sid = b.sid;

    g_session_registry.detach_from_slot(sid, 0, 1, 2000);
    // A DIFFERENT save is loaded — character ids are reused, so kEarlA now names
    // somebody else entirely.
    world_epoch_observe(world_fp(0xC000, raw_for(1066, 9, 15)));
    const uint64_t epoch_b = world_epoch();

    auto r = g_session_registry.resume_by_token(b.token, ident(kEpisodeA, kSaveB),
                                                epoch_b, 4, 1, 3000);
    check(LANE, !r.ok, "THE RESUME IS REFUSED");
    check(LANE, std::string(r.status) == "world_changed", "with status world_changed",
          r.status);
    check(LANE, r.session == nullptr, "and hands back no session pointer");
    check_eq_sz(LANE, r.discarded_claims.size(), 2, "both claims are RELEASED");
    check(LANE, g_session_registry.owner_of(kEarlA) == nullptr,
          "the ownership entry is gone, so the id is free for the new world");
    check(LANE, g_session_registry.find(sid) == nullptr,
          "THE SESSION IS ERASED, not left behind as a claimless orphan "
          "(§6.2: an orphan holds an unavailability span to the horizon)");
    check_eq_sz(LANE, g_session_registry.ids().size(), 0, "the registry is empty");
    check_eq_i64(LANE, g_purge_calls.load(), 1, "the steering-purge hook fired once");
    check(LANE, !live_has(kEarlA, 90) && !quarantine_has(kEarlA),
          "and the previous world's locks for that id are GONE — this is the "
          "ck-9fc3 hazard (a stranger's personality rewritten 3x/tick) closed "
          "at the claims door");

    // The token cannot be replayed to resurrect anything.
    auto again = g_session_registry.resume_by_token(b.token, ident(kEpisodeA, kSaveB),
                                                    epoch_b, 4, 2, 4000);
    check(LANE, !again.ok && std::string(again.status) == "unknown_token",
          "and the resume token died with the session");
}

// ===========================================================================
// 6. AN UNVOUCHED WORLD CHANGE — §6.2's bias, inverted from ck-hxh7's.
// ===========================================================================
static void lane_unvouched() {
    printf("\n6. UNVOUCHED WORLD CHANGE — no declaration means DISCARD, not benefit of the doubt\n");
    const char* LANE = "unvouched";
    clean_slate();
    install_test_purge_hook();

    world_identity_declare(ident(kEpisodeA, kSaveA));
    world_epoch_observe(world_fp(0xA000, raw_for(1066, 9, 15)));
    Bound b = bind_session("english-1", 0, 1, {kEarlA},
                           ident(kEpisodeA, kSaveA), world_epoch(), 1000);
    g_session_registry.detach_from_slot(b.sid, 0, 1, 2000);

    world_epoch_observe(world_fp(0xD000, raw_for(1066, 9, 15)));   // something loaded
    auto r = g_session_registry.resume_by_token(b.token, WorldIdentity{},
                                                world_epoch(), 4, 1, 3000);
    check(LANE, !r.ok,
          "a resume that vouches for NOTHING into a world that demonstrably "
          "changed is refused");
    check(LANE, std::string(r.identity_reason) == "world_changed_no_declared_identity",
          "for the stated reason", r.identity_reason);
    check(LANE, g_session_registry.ids().empty(), "and the session is erased");

    // The mirror image: the world did NOT change, so no identity is needed at
    // all. A socket drop is not a world event, and this is the common case — a
    // subscription-cap disconnect and a reconnect nine game-days later.
    clean_slate();
    world_epoch_observe(world_fp(0xA000, raw_for(1066, 9, 15)));
    Bound c = bind_session("english-1", 0, 1, {kEarlA}, WorldIdentity{},
                           world_epoch(), 1000);
    g_session_registry.detach_from_slot(c.sid, 0, 1, 2000);
    auto ok = g_session_registry.resume_by_token(c.token, WorldIdentity{},
                                                 world_epoch(), 4, 1, 3000);
    check(LANE, ok.ok,
          "but a resume into the SAME world epoch needs no identity at all");
    check(LANE, std::string(ok.identity_reason) == "same_world_epoch",
          "for the stated reason", ok.identity_reason);
}

// ===========================================================================
// 7. THE SECOND DOOR — declare_save_identity reconciles a resume that abstained.
// ===========================================================================
static void lane_reconcile() {
    printf("\n7. THE SECOND DOOR — a later declaration refutes a session that resumed unvouched\n");
    const char* LANE = "reconcile";
    clean_slate();
    install_test_purge_hook();

    world_epoch_observe(world_fp(0xA000, raw_for(1066, 9, 15)));
    const uint64_t epoch_a = world_epoch();
    Bound b = bind_session("english-1", 0, 1, {kEarlA},
                           ident(kEpisodeA, kSaveA), epoch_a, 1000);
    // Same epoch: the resume allows without asking for an identity.
    g_session_registry.detach_from_slot(b.sid, 0, 1, 2000);
    auto r = g_session_registry.resume_by_token(b.token, WorldIdentity{},
                                                epoch_a, 4, 1, 3000);
    check(LANE, r.ok, "the resume abstains and allows (the world never changed)");

    // A declaration of the SAME save changes nothing.
    auto keep = g_session_registry.reconcile_declared_world(ident(kEpisodeA, kSaveA),
                                                            epoch_a, nullptr);
    check(LANE, keep.empty(), "declaring the same save erases nobody");
    check(LANE, g_session_registry.owner_of(kEarlA) == b.s, "and the claim stands");

    // A declaration naming a DIFFERENT save refutes it. This is the path a
    // client takes when it declares through the standalone ck-9fc3 action
    // instead of on the handshake frame.
    std::vector<uint32_t> purged;
    auto erased = g_session_registry.reconcile_declared_world(ident(kEpisodeA, kSaveB),
                                                              epoch_a, &purged);
    check_eq_sz(LANE, erased.size(), 1, "declaring a DIFFERENT save erases the session");
    check_eq_sz(LANE, purged.size(), 1, "and releases its claim");
    check(LANE, g_session_registry.owner_of(kEarlA) == nullptr, "ownership is cleared");
    check_eq_i64(LANE, g_purge_calls.load(), 1, "the steering-purge hook fired");

    // A session that vouched for nothing is left alone: a declaration can only
    // ever say NO, and there is nothing here for it to contradict.
    clean_slate();
    install_test_purge_hook();
    world_epoch_observe(world_fp(0xA000, raw_for(1066, 9, 15)));
    Bound q = bind_session("english-2", 0, 1, {kEarlB}, WorldIdentity{},
                           world_epoch(), 1000);
    auto none = g_session_registry.reconcile_declared_world(ident(kEpisodeA, kSaveB),
                                                            world_epoch(), nullptr);
    check(LANE, none.empty(),
          "a session that recorded NO world is not refuted by any declaration");
    check(LANE, g_session_registry.owner_of(kEarlB) == q.s, "and keeps its claim");
}

// ===========================================================================
// 8. UNAVAILABILITY — the gap is RECORDED, and a resume never resets it.
// ===========================================================================
static void lane_unavailability() {
    printf("\n8. UNAVAILABILITY — the gap the drop cost is in the record, not erased by the reconnect\n");
    const char* LANE = "unavail";
    clean_slate();

    // ck-ijfa: the gate is in WALL-CLOCK (360 s = 3 R3 heartbeats) and X is in
    // GAME-DAYS. A drop lasting minutes is unavailability; ordinary turn-taking
    // is not, however many game-days a fast clock runs through meanwhile.
    unavailability().configure(/*anomaly_wall_s*/360,
                               /*exclusion_days*/kExclusionDaysAtRegisteredHorizon,
                               /*exclusion_armed*/true);   // ck-2ngg: the
    // RULE's answer at the registered 20-game-year horizon, stated as such.
    // Armed explicitly, because ck-2ngg made that a required question: an
    // unarmed ledger judges nothing, and a resume lane that inherited a
    // silent default would be asserting against a threshold it never set.

    // The wall clock this lane models: monotonic ms, at the slow condition
    // (~0.076 game-days/wall-second) so a gap can sit under X and still be
    // anomalous. Stated because a threshold quoted without its rate is the
    // defect ck-ijfa was filed for.
    const uint64_t W0 = 5'000'000;

    const std::string sid = "sess-english-1";
    unavailability().note_session(sid, 100, W0);       // handshake on game-day 100
    unavailability().note_activity(sid, 100, W0);
    unavailability().note_day(110, W0 + 100'000);

    // THE DROP. Accrual continues: under the claim pin a detached session still
    // owns its character, so its silence is still a hole in the record.
    unavailability().note_attached(sid, false);
    unavailability().note_day(260, W0 + 2'100'000);    // 160 game-days / 2100 s later

    {
        auto snap = unavailability().snapshot();
        check_eq_sz(LANE, snap.size(), 1, "the session is tracked");
        check_eq_i64(LANE, snap[0].last_activity_day, 100,
                     "its last activity is still game-day 100 while detached");
        check_eq_i64(LANE, snap[0].unavailable_game_days, 160,
                     "and the drop is ANOMALOUS (2100 wall-seconds, far past the "
                     "360-second gate), so the 160 GAME-DAYS it spans have accrued");
        check(LANE, !snap[0].attached, "the record knows the socket is gone");
    }

    // THE RESUME. This is the exact call the bridge makes on a rebind
    // (pipeline_note_session_open -> note_session), and ck-6kvq.10 wrote it to
    // be an ATTACH, never a reset.
    unavailability().note_session(sid, 260, W0 + 2'100'000);
    {
        auto snap = unavailability().snapshot();
        check(LANE, snap[0].attached, "the resume marks the session attached again");
        check_eq_i64(LANE, snap[0].last_activity_day, 100,
                     "A RESUME IS AN ATTACH, NOT A RESET: the gap is not erased");
        check_eq_i64(LANE, snap[0].unavailable_game_days, 160,
                     "the accrued span survives the reconnect");
    }

    // Speaking closes the span, and the closed span is itemised — in BOTH units.
    unavailability().note_activity(sid, 261, W0 + 2'110'000);
    {
        auto snap = unavailability().snapshot();
        check_eq_sz(LANE, snap[0].span_count, 1, "the gap is recorded as ONE anomalous span");
        check_eq_i64(LANE, snap[0].unavailable_game_days, 161,
                     "of 161 game-days, which is what the episode's covariate reports");
        check_eq_i64(LANE, (int64_t)(snap[0].unavailable_wall_seconds + 0.5), 2110,
                     "and of 2110 WALL-seconds, which is what the gate judged — "
                     "every gap is recorded in both units (ck-ijfa)");
        check(LANE, snap[0].spans.size() == 1 && snap[0].spans[0].anomalous,
              "and the itemised span carries the verdict");
        check(LANE, !snap[0].over_threshold,
              "161 is under X=365, so this interruption does not invalidate the episode");
    }

    // A HEALTHY BEAT AFTER THE RESUME ADDS NOTHING, even though the world moves
    // on: a reconnected agent on the heartbeat is available, and the covariate
    // must say so. This is the discriminator between the shipped gate and the
    // retired game-day one, on the resume path specifically.
    unavailability().note_activity(sid, 265, W0 + 2'220'000);   // 110 s, 4 game-days
    {
        auto snap = unavailability().snapshot();
        check_eq_i64(LANE, snap[0].unavailable_game_days, 161,
                     "a 110-wall-second turn after the resume adds NOTHING "
                     "(under the 360-second gate), so a recovered session stops "
                     "accruing the moment it speaks again");
        check_eq_sz(LANE, snap[0].gap_count, 2,
                    "though the raw record still counts every gap — the anomalous "
                    "one and this healthy one — for re-derivation");
    }

    // A refused resume must INFLATE the covariate, not vanish from it: a phantom
    // English agent is exactly what §2.7 exists to make visible.
    unavailability().note_session_ended(sid);
    unavailability().note_day(700, W0 + 8'000'000);
    {
        auto snap = unavailability().snapshot();
        check_eq_sz(LANE, snap.size(), 1,
                    "an ENDED session keeps its record (a phantom agent stays visible)");
        check(LANE, snap[0].over_threshold,
              "and keeps accruing past X, so the episode is flagged invalid");
    }
}

// ===========================================================================
// 9. max_detach_s — REPORTED, never released.
// ===========================================================================
static void lane_max_detach() {
    printf("\n9. max_detach_s — expiry is a REPORT; nothing is released and nobody is handed back\n");
    const char* LANE = "max-detach";
    clean_slate();

    Bound b = bind_session("english-1", 0, 1, {kEarlA, kEarlB},
                           ident(kEpisodeA, kSaveA), 1, 1000);
    g_session_registry.detach_from_slot(b.sid, 0, 1, 10000);

    const uint64_t max_ms = 900 * 1000ull;   // episode_abort.sh's default

    auto none = g_session_registry.sweep_detached(10000 + max_ms - 1, max_ms);
    check(LANE, none.empty(), "a detach inside the bound is not reported");
    check(LANE, b.s->state == Session::State::DETACHED, "and stays DETACHED");

    auto lost = g_session_registry.sweep_detached(10000 + max_ms, max_ms);
    check_eq_sz(LANE, lost.size(), 1, "crossing the bound reports the session ONCE");
    check(LANE, lost[0] == b.sid, "by id");
    check(LANE, b.s->state == Session::State::LOST, "its state is LOST");

    auto again = g_session_registry.sweep_detached(10000 + max_ms * 3, max_ms);
    check(LANE, again.empty(),
          "and a later sweep does NOT report it again (reported on the sweep that "
          "promotes it, not on every sweep after)");
    check_eq_sz(LANE, g_session_registry.lost_sessions().size(), 1,
                "but it is still readable as LOST by the abort ladder");

    // The whole point of LOST: it releases NOTHING.
    check_eq_sz(LANE, b.s->claimed_character_ids.size(), 2,
                "THE CLAIMS ARE RETAINED — releasing them here would be the "
                "mid-episode arm change R3 forbids, with nobody connected to see it");
    check(LANE, g_session_registry.owner_of(kEarlA) == b.s,
          "and ownership still names the same session");

    // A LOST session is still resumable: the agent came back late, and there is
    // something real to come back to.
    auto r = g_session_registry.resume_by_token(b.token, ident(kEpisodeA, kSaveA),
                                                1, 4, 1, 99999);
    check(LANE, r.ok, "a LOST session can still be resumed (late is not gone)");
    check_eq_sz(LANE, b.s->claimed_character_ids.size(), 2, "with its claims");

    // Bound disabled.
    clean_slate();
    Bound c = bind_session("english-1", 0, 1, {kEarlA}, ident(kEpisodeA, kSaveA), 1, 1000);
    g_session_registry.detach_from_slot(c.sid, 0, 1, 1000);
    check(LANE, g_session_registry.sweep_detached(1000 + max_ms * 10, 0).empty(),
          "max_detach_ms == 0 disables the bound entirely");
}

// ===========================================================================
// 10. THE DETACH REPORT — the producer ck-6kvq.1's session_lost rung never had.
// ===========================================================================
static void lane_detach_report() {
    printf("\n10. THE DETACH REPORT — state/detached-since, in the shape episode_abort.sh parses\n");
    const char* LANE = "report";
    clean_slate();

    char dir[128];
    snprintf(dir, sizeof(dir), "/tmp/ck3-acceptance-episode-%d", (int)getpid());
    g_tmp_episode_dir = dir;
    const std::string state_f = g_tmp_episode_dir + "/state/detached-since";

    // No episode directory -> no episode -> the bridge must not create files.
    session_set_episode_dir_for_test(dir);
    session_publish_detach_state();
    struct stat st;
    check(LANE, stat(state_f.c_str(), &st) != 0,
          "with no episode directory the bridge writes NOTHING "
          "(free play must not litter /tmp for a ladder that is not watching)");

    ::mkdir(dir, 0777);

    Bound b = bind_session("english-1", 0, 1, {kEarlA}, ident(kEpisodeA, kSaveA), 1, 1000);
    session_publish_detach_state();
    check(LANE, stat(state_f.c_str(), &st) != 0,
          "an ATTACHED session produces no verdict (absence means 'no verdict' — "
          "the reader's test is [ -s \"$DETACH_F\" ])");

    // Detach with a state_since in the recent past, so the epoch stamp is a real
    // interval and not just "now".
    struct timespec mono;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    const uint64_t now_mono_ms =
        (uint64_t)mono.tv_sec * 1000ull + mono.tv_nsec / 1000000ull;
    g_session_registry.detach_from_slot(b.sid, 0, 1, now_mono_ms - 120000);   // 120s ago
    session_publish_detach_state();

    check(LANE, stat(state_f.c_str(), &st) == 0 && st.st_size > 0,
          "a detached session publishes a NON-EMPTY state/detached-since");

    long long since = 0;
    {
        FILE* f = fopen(state_f.c_str(), "r");
        char buf[64] = {0};
        if (f) { if (fgets(buf, sizeof(buf), f) == nullptr) buf[0] = 0; fclose(f); }
        since = atoll(buf);
        // Exactly what `since=$(tr -d '[:space:]' < "$DETACH_F")` then bash
        // arithmetic will see: one integer and nothing else.
        bool digits_only = buf[0] != 0;
        for (const char* p = buf; *p; p++)
            if (!isdigit((unsigned char)*p) && *p != '\n' && *p != '\r') digits_only = false;
        check(LANE, digits_only,
              "whose content is ONE integer and nothing else (the reader's parse "
              "is unguarded bash arithmetic)");
    }
    const long long held = (long long)time(nullptr) - since;
    check(LANE, held >= 115 && held <= 135,
          "and the integer is WALL-CLOCK EPOCH SECONDS at which the detach began",
          std::string("held=") + std::to_string(held) + "s, expected ~120s");

    // The earliest outstanding detach wins: the rung asks "has anyone been gone
    // too long", not "has the most recent drop been long".
    Bound c = bind_session("english-2", 1, 1, {kEarlB}, ident(kEpisodeA, kSaveA), 1, 1000);
    g_session_registry.detach_from_slot(c.sid, 1, 1, now_mono_ms);   // just now
    session_publish_detach_state();
    {
        FILE* f = fopen(state_f.c_str(), "r");
        char buf[64] = {0};
        if (f) { if (fgets(buf, sizeof(buf), f) == nullptr) buf[0] = 0; fclose(f); }
        if (f) { }
        const long long held2 = (long long)time(nullptr) - atoll(buf);
        check(LANE, held2 >= 115,
              "with two detaches outstanding it reports the EARLIEST",
              std::string("held=") + std::to_string(held2) + "s");
    }

    // Removed the instant the last one comes back. Leaving a stale timestamp
    // would abort a HEALTHY episode session_lost once max_detach_s elapsed —
    // the exact failure the rung exists to catch, arriving as a false positive.
    g_session_registry.resume_by_token(b.token, ident(kEpisodeA, kSaveA), 1, 4, 1, now_mono_ms);
    session_publish_detach_state();
    check(LANE, stat(state_f.c_str(), &st) == 0,
          "one of two reattaching leaves the report standing");
    g_session_registry.resume_by_token(c.token, ident(kEpisodeA, kSaveA), 1, 5, 1, now_mono_ms);
    session_publish_detach_state();
    check(LANE, stat(state_f.c_str(), &st) != 0,
          "and the LAST reattach REMOVES it (no stale verdict left behind)");

    session_set_episode_dir_for_test(nullptr);
}

// ===========================================================================
// 11. THE OBSERVER (ck-bw1s) — a dropped OBSERVE session is NOT a verdict.
// ===========================================================================
// WHY THIS LANE EXISTS. Lane 10 proves the report's SHAPE with control sessions
// only, because ck-6kvq.5's subject is claims and observers have none. That left
// the report counting every non-attached session, so an observe-mode drop wrote
// state/detached-since — and episode_abort.sh reads that file as evidence of a
// dying session. A diagnostic tool that connects in observe mode and disconnects
// could abort a HEALTHY episode with reason=session_lost.
//
// The observer is not a participant in the arm: no claims, no row in the
// unavailability ledger, and its absence changes nothing about the experimental
// condition. So its detach is registry state (get_bridge_state stays honest) and
// nothing more.
static void lane_observer_report() {
    printf("\n11. THE OBSERVER (ck-bw1s) — an observe-mode drop publishes NO detach verdict\n");
    const char* LANE = "observer";
    clean_slate();

    char dir[128];
    snprintf(dir, sizeof(dir), "/tmp/ck3-acceptance-episode-%d", (int)getpid());
    g_tmp_episode_dir = dir;
    const std::string state_f = g_tmp_episode_dir + "/state/detached-since";
    session_set_episode_dir_for_test(dir);
    ::mkdir(dir, 0777);
    ::unlink(state_f.c_str());

    struct timespec mono;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    const uint64_t now_mono_ms =
        (uint64_t)mono.tv_sec * 1000ull + mono.tv_nsec / 1000000ull;

    // The meter lane's shape (ck-6kvq.7): OBSERVE mode, no claims, read-only.
    Bound obs = bind_session("meter-lane", /*slot*/2, /*gen*/1, {},
                             ident(kEpisodeA, kSaveA), /*epoch*/1, /*now*/1000,
                             Session::Mode::OBSERVE);
    check(LANE, obs.s->is_observe(), "the meter lane attaches in OBSERVE mode");
    check_eq_sz(LANE, obs.s->claimed_character_ids.size(), 0,
                "and holds no claims (nothing about the arm depends on it)");

    // It drops 300s ago — well past any max_detach_s an episode would run with.
    const bool obs_detached = g_session_registry.detach_from_slot(
        obs.sid, 2, 1, now_mono_ms - 300000);
    check(LANE, obs_detached,
          "a dropped observer is still DETACHED in the registry "
          "(get_bridge_state must stay honest about a socket that went away)");
    check(LANE, obs.s->state == Session::State::DETACHED, "its state is DETACHED");

    session_publish_detach_state();
    struct stat st;
    check(LANE, stat(state_f.c_str(), &st) != 0,
          "but it publishes NO state/detached-since — an observer drop is not "
          "evidence about the experimental condition, and the abort ladder would "
          "read it as reason=session_lost on a HEALTHY episode");

    // A CONTROL session drops BESIDE it, MORE RECENTLY. The file must appear, and
    // it must name the CONTROL session's detach, not the earlier observer's — an
    // "earliest outstanding detach" that counted the observer would backdate the
    // verdict by the observer's whole absence and fire the rung immediately.
    Bound ctl = bind_session("english-1", /*slot*/3, /*gen*/1, {kEarlA},
                             ident(kEpisodeA, kSaveA), /*epoch*/1, /*now*/1000);
    g_session_registry.detach_from_slot(ctl.sid, 3, 1, now_mono_ms - 60000);  // 60s ago
    session_publish_detach_state();
    check(LANE, stat(state_f.c_str(), &st) == 0 && st.st_size > 0,
          "a CONTROL session dropping beside it DOES publish a verdict");

    {
        FILE* f = fopen(state_f.c_str(), "r");
        char buf[64] = {0};
        if (f) { if (fgets(buf, sizeof(buf), f) == nullptr) buf[0] = 0; fclose(f); }
        const long long held = (long long)time(nullptr) - atoll(buf);
        check(LANE, held >= 50 && held <= 80,
              "and the timestamp is the CONTROL session's detach (~60s), NOT the "
              "earlier observer's (~300s) — the report's 'earliest outstanding' "
              "ranges over control sessions only",
              std::string("held=") + std::to_string(held) + "s, expected ~60s");
    }

    // The control session comes back; the observer is still gone. The file must
    // be REMOVED, or a healthy episode carries a stale verdict for as long as
    // nobody restarts the meter lane — and past max_detach_s that verdict is an
    // abort.
    g_session_registry.resume_by_token(ctl.token, ident(kEpisodeA, kSaveA), 1, 6, 1, now_mono_ms);
    session_publish_detach_state();
    check(LANE, obs.s->state == Session::State::DETACHED,
          "the observer is still DETACHED (nothing about it changed)");
    check(LANE, stat(state_f.c_str(), &st) != 0,
          "yet the reattach of the last CONTROL session REMOVES the report — a "
          "still-absent observer cannot hold the verdict open");

    // Past max_detach_s the sweep moves the observer to LOST. It stays silent:
    // LOST is the state machine's word about a socket, not the ladder's about an
    // agent.
    const std::vector<std::string> lost =
        g_session_registry.sweep_detached(now_mono_ms, /*max_detach_ms*/60000);
    check_eq_sz(LANE, lost.size(), 1, "the max_detach_s sweep does move it to LOST");
    check(LANE, obs.s->state == Session::State::LOST, "so its state is LOST");
    session_publish_detach_state();
    check(LANE, stat(state_f.c_str(), &st) != 0,
          "and it STILL publishes no verdict — a LOST observer is not a lost agent");

    ::unlink(state_f.c_str());
    session_set_episode_dir_for_test(nullptr);
}

int main(int argc, char** argv) {
    const char* control_name = nullptr;
#ifdef CK3_RESUME_NO_IDENTITY_CHECK
    control_name = "NEGATIVE CONTROL: resume identity check DISABLED";
#endif
#ifdef CK3_DETACH_REPORT_COUNTS_OBSERVERS
    // ck-bw1s: the pre-fix report, which counted every non-attached session.
    control_name = "NEGATIVE CONTROL: the detach report COUNTS OBSERVERS (pre-ck-bw1s)";
#endif
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--expect-fail") == 0 && !control_name)
            control_name = "NEGATIVE CONTROL (asserted on the command line)";

    printf("acceptance_session_resume (ck-6kvq.5 + ck-bw1s): a dropped socket must not "
           "destroy the session or free its claims, and a dropped OBSERVER is not a "
           "session_lost verdict\n");
    if (control_name)
        printf("no CK3, no bridge socket — host only   [%s]\n", control_name);
    else
        printf("no CK3, no bridge socket — host only\n");

    char buf[128];
    snprintf(buf, sizeof(buf), "/tmp/ck3-acceptance-resume-locks-%d.json", (int)getpid());
    g_tmp_lockfile = buf;
    set_personality_locks_path(g_tmp_lockfile.c_str());
    ::unlink(g_tmp_lockfile.c_str());

    lane_detach();
    lane_stale_generation();
    lane_resume();
    lane_same_save();
    lane_different_save();
    lane_unvouched();
    lane_reconcile();
    lane_unavailability();
    lane_max_detach();
    lane_detach_report();
    lane_observer_report();

    ::unlink(g_tmp_lockfile.c_str());
    set_personality_locks_path(nullptr);
    if (!g_tmp_episode_dir.empty()) {
        std::string f = g_tmp_episode_dir + "/state/detached-since";
        ::unlink(f.c_str());
        ::unlink((f + ".tmp").c_str());
        ::rmdir((g_tmp_episode_dir + "/state").c_str());
        ::rmdir(g_tmp_episode_dir.c_str());
    }

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    if (g_fails) {
        printf("acceptance_session_resume: FAIL\n");
        return 1;
    }
    printf("acceptance_session_resume: PASS\n");
    return 0;
}
