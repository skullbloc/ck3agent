// test/acceptance_save_identity.cpp — ck-9fc3 ACCEPTANCE.
//
// THE CLAIM UNDER TEST, in one sentence: after a save is loaded into a live CK3
// process, agent personality steering is restored ONLY when the loaded world is
// PROVABLY the same save, and otherwise stays quarantined — and the decision and
// its evidence are published so a harness can record the resume-to-steer gap.
//
// WHY IT MATTERS. g_personality_locks is keyed by CHARACTER ID; character ids
// are reused across saves; apply_personality_locks() pokes the int8 axes into
// character structs three times per on_tick. ck-hxh7 stopped the write by
// emptying the live set on every detected reload and quarantining the entries,
// which is correct and incomplete: a legitimate same-save resume (R7) then
// loses its steering for the rest of the episode. Restoring on the wrong world
// is worse than not restoring at all, so the rule has to be a PROOF of sameness
// and not a heuristic.
//
// NO GAME. This is a host binary. It links the REAL world_identity.cpp, the REAL
// personality_world_reset.cpp, the REAL personality_persist.cpp and the REAL
// world_epoch.cpp, and drives reloads through the documented
// world_epoch_observe() seam. The decision under test is the shipped one.
//
// THE NEGATIVE CONTROL IS THE POINT. A guard nobody has watched fail is a guard
// nobody has tested. Lane 3 loads a DIFFERENT save and demands that nothing is
// restored; lane 7 then calls the UNGUARDED primitive on the identical fixture
// and demands that the contamination DOES appear — so the pass in lane 3 is the
// guard's doing and not an accident of the setup.

#include "bridge.h"
#include "personality_locks.h"
#include "world_epoch.h"
#include "world_identity.h"
#include "game_date.h"

#include <cstdio>
#include <cstring>
#include <shared_mutex>
#include <string>
#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <vector>

// Stub globals referenced by bridge.h and the engine-free TUs when compiled
// standalone (the acceptance_world_reset / test_session_registry pattern).
namespace bridge {
FILE*                  g_logfile = nullptr;
uintptr_t              g_base_addr = 0;
std::vector<MemRange>  g_valid_ranges;
std::shared_mutex      g_ranges_mutex;
}

using namespace bridge;

// ---------------------------------------------------------------------------
// Reporting: report every check, never abort on the first.
// ---------------------------------------------------------------------------
static int g_checks = 0;
static int g_fails  = 0;

static void check(const char* lane, bool ok, const char* what, const std::string& detail = "") {
    g_checks++;
    if (!ok) g_fails++;
    printf("  [%-9s] %-4s %s%s%s\n", lane, ok ? "ok" : "FAIL", what,
           detail.empty() ? "" : " — ", detail.c_str());
}

static void check_eq_sz(const char* lane, size_t actual, size_t expected, const char* what) {
    char d[128];
    snprintf(d, sizeof(d), "expected %zu, got %zu", expected, actual);
    check(lane, actual == expected, what, actual == expected ? "" : d);
}

static void check_eq_str(const char* lane, const std::string& actual,
                         const char* expected, const char* what) {
    bool ok = (actual == expected);
    check(lane, ok, what, ok ? "" : ("expected \"" + std::string(expected) + "\", got \"" + actual + "\""));
}

// ---------------------------------------------------------------------------
// Fixtures. Two DIFFERENT saves of the SAME 1066 bookmark — the hard case: a
// start-year check alone cannot tell them apart, which is exactly why the
// identity has to come from the save's bytes.
// ---------------------------------------------------------------------------
// The real sha256 of ~/ck3_dumps/william_1066_baseline.save, as verified by
// scripts/save_verify.py and recorded in ck-6kvq.2's run_summary.json.
static const char* kSaveA = "0c94b59880e0549a5a0c8fbe4dc675a3de171f28d79f7628ad765116de782d5d";
static const char* kSaveB = "7f21aa0c1d3e4b5968708192a3b4c5d6e7f8091a2b3c4d5e6f708192a3b4c5d6";
static const char* kEpisodeA = "ep-20260912T033051Z-e0-paper-9eae1e0e";
static const char* kEpisodeB = "ep-20260912T104500Z-a-pilot-11112222";

static WorldIdentity ident(const char* episode, const char* sha, uint32_t year = 1066) {
    WorldIdentity id;
    id.episode_id = episode ? episode : "";
    id.save_sha256 = sha ? sha : "";
    id.start_date_year = year;
    return id;
}

// Dates are built through the ONE renderer's own inverse (ck-05mg), so the
// fixture cannot drift from what the bridge will print.
static int32_t raw_for(int y, int m, int d) { return ck3::game_date::ymd_to_raw(y, m, d); }

// One world: 1066 bookmark, some heap address, some date.
static WorldFingerprint world_fp(uintptr_t gs, int32_t raw) {
    WorldFingerprint f;
    f.loaded = true;
    f.game_state_ptr = gs;
    f.start_date_year = 1066;
    f.game_date = raw;
    return f;
}

static PersonalityLock lock_with(int8_t v) {
    PersonalityLock pl{};
    for (int i = 0; i < 10; i++) { pl.axes[i] = v; pl.locked[i] = true; }
    return pl;
}

static void set_live(const std::vector<std::pair<uint32_t, int8_t>>& entries) {
    std::unique_lock<std::shared_mutex> lk(g_personality_lock_mutex);
    g_personality_locks.clear();
    g_quarantined_personality_locks.clear();
    for (const auto& [cid, v] : entries) g_personality_locks[cid] = lock_with(v);
}

static bool live_has(uint32_t cid, int8_t v) {
    std::shared_lock<std::shared_mutex> lk(g_personality_lock_mutex);
    auto it = g_personality_locks.find(cid);
    return it != g_personality_locks.end() && it->second.axes[0] == v && it->second.locked[0];
}

static void clean_slate() {
    world_epoch_reset_for_test();
    world_identity_reset_for_test();
    set_live({});
}

// A private lock file per run, so the acceptance never touches the real
// /tmp/ck3-personality-locks.json a live campaign may be using.
static std::string g_tmp_lockfile;
static void use_temp_lockfile() {
    char buf[128];
    snprintf(buf, sizeof(buf), "/tmp/ck3-acceptance-locks-%d.json", (int)getpid());
    g_tmp_lockfile = buf;
    set_personality_locks_path(g_tmp_lockfile.c_str());
    ::unlink(g_tmp_lockfile.c_str());
}

static std::string read_lockfile() {
    FILE* f = fopen(g_tmp_lockfile.c_str(), "r");
    if (!f) return "";
    std::string s;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

static void write_lockfile(const std::string& body) {
    FILE* f = fopen(g_tmp_lockfile.c_str(), "w");
    if (!f) return;
    fwrite(body.data(), 1, body.size(), f);
    fclose(f);
}

static bool json_has(const std::string& j, const char* needle) {
    return j.find(needle) != std::string::npos;
}

// ===========================================================================
// 1. THE RULE — judge_same_save(), pure, no state anywhere.
// ===========================================================================
static void lane_rule() {
    printf("\n1. THE RULE — judge_same_save() is pure and total\n");
    const char* LANE = "rule";

    std::string why;
    const WorldIdentity A  = ident(kEpisodeA, kSaveA);
    const WorldIdentity A2 = ident(kEpisodeB, kSaveA);   // same save, other run
    const WorldIdentity B  = ident(kEpisodeA, kSaveB);
    const WorldIdentity none;

    check(LANE, judge_same_save(A, A, 1066, false, &why) == SameSave::YES,
          "the same save reloaded is the same save");
    check(LANE, judge_same_save(A, B, 1066, false, &why) == SameSave::DIFFERENT_SAVE,
          "a different save hash is DIFFERENT_SAVE — the ck-9fc3 case");
    check(LANE, judge_same_save(A, none, 1066, false, &why) == SameSave::NO_DECLARED_IDENTITY,
          "nobody vouching for the loaded world is NOT a match");
    check(LANE, judge_same_save(none, A, 1066, false, &why) == SameSave::NO_RECORDED_IDENTITY,
          "steering that names no save cannot be proven to belong here");

    // episode_id is excluded from same-save on purpose (two episodes may replay
    // one save) but the disk carrier is stricter, because it is shared by path.
    check(LANE, judge_same_save(A, A2, 1066, false, &why) == SameSave::YES,
          "in-process: a reminted episode id does not break a same-save resume");
    check(LANE, judge_same_save(A, A2, 1066, true, &why) == SameSave::DIFFERENT_EPISODE,
          "on disk: another episode may not inherit this episode's steering");

    // The world may only ever REFUTE.
    check(LANE, judge_same_save(A, A, 867, false, &why) == SameSave::WORLD_CONTRADICTS,
          "an 867 world contradicts a declaration that says 1066");
    check(LANE, judge_same_save(A, A, 0, false, &why) == SameSave::YES,
          "an unreadable world abstains rather than guessing");

    // A start-year disagreement under a matching hash means somebody is lying.
    WorldIdentity liar = ident(kEpisodeA, kSaveA, 867);
    check(LANE, judge_same_save(A, liar, 0, false, &why) == SameSave::DIFFERENT_SAVE,
          "a matching hash with a disagreeing start year is refused, not trusted");

    check(LANE, !why.empty(), "every verdict carries a reason string");

    // The decision strings are a CLOSED vocabulary a harness switches on, and
    // they are formed as "refused_" + same_save_name(v). Pin the whole table so
    // a rename here cannot silently diverge from the E0 runbook's §2.8 list.
    const char* expect[] = {"yes", "no_declared_identity", "no_recorded_identity",
                            "different_save", "different_episode",
                            "world_contradicts_declaration"};
    const SameSave all[] = {SameSave::YES, SameSave::NO_DECLARED_IDENTITY,
                            SameSave::NO_RECORDED_IDENTITY, SameSave::DIFFERENT_SAVE,
                            SameSave::DIFFERENT_EPISODE, SameSave::WORLD_CONTRADICTS};
    bool names_ok = true;
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (std::string(same_save_name(all[i])) != expect[i]) names_ok = false;
    }
    check(LANE, names_ok,
          "the verdict vocabulary is exactly the six documented names (E0 runbook §2.8)");
}

// ===========================================================================
// 2. THE RESUME PATH — the same save reloaded restores the steering.
// ===========================================================================
static void lane_same_save_restores() {
    printf("\n2. SAME SAVE RELOADED — steering is restored (R7 resume)\n");
    const char* LANE = "in-proc";

    clean_slate();
    world_epoch_observe(world_fp(0x7f0000001000, raw_for(1066, 9, 15)));   // world comes up
    world_identity_declare(ident(kEpisodeA, kSaveA));
    set_live({{31808, 90}, {30853, 20}});
    check_eq_sz(LANE, personality_lock_count(), 2, "two characters are steered before the reload");

    // The reload: same save, new heap address, clock back at the bookmark.
    bool detected = world_epoch_observe(world_fp(0x7f0000009000, raw_for(1066, 9, 15)));
    check(LANE, detected, "the reload is detected (ck-hxh7)");
    check_eq_sz(LANE, personality_lock_count(), 0,
                "the live write set is EMPTY across the reload — apply_personality_locks() "
                "writes nothing into the new world until somebody vouches for it");
    check_eq_sz(LANE, personality_quarantine_count(), 2, "the entries are held in quarantine");
    check(LANE, world_identity_declared().empty(),
          "the declared identity is cleared by the reload — nobody has vouched for the new world");
    check(LANE, world_identity_quarantined().save_sha256 == kSaveA,
          "the quarantine remembers WHICH save the steering belongs to");

    // The harness declares the same save. THE restore.
    RestoreRecord rec = world_identity_declare(ident(kEpisodeA, kSaveA));
    check_eq_str(LANE, rec.decision, "restored_from_quarantine", "the decision is a restore");
    check_eq_sz(LANE, rec.restored_count, 2, "both entries came back");
    check_eq_sz(LANE, personality_lock_count(), 2, "the live write set is steering again");
    check(LANE, live_has(31808, 90) && live_has(30853, 20),
          "the restored axes are the ones the agent set, byte for byte");
    check_eq_sz(LANE, personality_quarantine_count(), 0, "the quarantine is emptied by the restore");

    // The documented procedure declares at episode start AND on resume, so a
    // repeat declaration is expected and must not read as a refusal.
    RestoreRecord again = world_identity_declare(ident(kEpisodeA, kSaveA));
    check_eq_str(LANE, again.decision, "nothing_to_restore",
                 "re-declaring the same identity for an already-steered world is idempotent");
    check_eq_sz(LANE, personality_lock_count(), 2, "and leaves the steering exactly as it was");
}

// ===========================================================================
// 3. THE NEGATIVE CONTROL — a DIFFERENT save must not restore.
// ===========================================================================
static void lane_different_save_refused() {
    printf("\n3. NEGATIVE CONTROL — a DIFFERENT save must NOT restore\n");
    const char* LANE = "negative";

    clean_slate();
    world_epoch_observe(world_fp(0x7f0000001000, raw_for(1068, 6, 23)));
    world_identity_declare(ident(kEpisodeA, kSaveA));
    set_live({{31808, 90}, {30853, 20}});

    world_epoch_observe(world_fp(0x7f0000009000, raw_for(1066, 9, 15)));
    check_eq_sz(LANE, personality_quarantine_count(), 2, "the reload quarantined both entries");

    // A DIFFERENT save is loaded into the same live process. 31808 and 30853
    // exist in it too — they are simply different people.
    RestoreRecord rec = world_identity_declare(ident(kEpisodeA, kSaveB));
    check_eq_str(LANE, rec.decision, "refused_different_save", "the restore is REFUSED, typed");
    check_eq_sz(LANE, rec.restored_count, 0, "nothing was restored");
    check_eq_sz(LANE, personality_lock_count(), 0,
                "THE P1: the live write set stays EMPTY, so not one personality write lands on a "
                "character carrying a reused id from the previous world");
    check_eq_sz(LANE, personality_quarantine_count(), 2,
                "the steering is not destroyed either — it is still held, and still named");
    check(LANE, rec.why.find(kSaveB) != std::string::npos &&
                rec.why.find(kSaveA) != std::string::npos,
          "the refusal names both saves, so an operator can see which is which");

    // And the same-save declaration still works afterwards: the refusal did not
    // poison the quarantine.
    RestoreRecord ok = world_identity_declare(ident(kEpisodeA, kSaveA));
    check_eq_str(LANE, ok.decision, "restored_from_quarantine",
                 "a correct declaration after a refused one still restores");
    check_eq_sz(LANE, personality_lock_count(), 2, "the steering survived the refusal intact");
}

// ===========================================================================
// 4. THE DEFAULT — no declaration means it stays quarantined, and the gap is
//    published and GROWS.
// ===========================================================================
static void lane_default_and_gap() {
    printf("\n4. THE DEFAULT AND THE RESUME-TO-STEER GAP\n");
    const char* LANE = "gap";

    clean_slate();
    world_epoch_observe(world_fp(0x7f0000001000, raw_for(1070, 4, 11)));
    world_identity_declare(ident(kEpisodeA, kSaveA));
    set_live({{31808, 90}});

    WorldFingerprint f = world_fp(0x7f0000009000, raw_for(1066, 9, 15));
    world_epoch_observe(f);

    std::string j = world_identity_emit_json();
    check(LANE, json_has(j, "\"decision\":\"pending_no_declaration\""),
          "with steering quarantined and nobody vouching, the published decision is pending");
    check(LANE, json_has(j, "\"open\":true"), "the resume gap is published as OPEN");
    check(LANE, json_has(j, "\"game_days\":0"), "the gap starts at zero game-days");

    // 30 game-days pass with the episode running unsteered.
    for (int d = 0; d < 30; d++) {
        f.game_date += WORLD_RAW_UNITS_PER_DAY;
        world_epoch_observe(f);
    }
    check_eq_sz(LANE, personality_lock_count(), 0,
                "30 game-days later still nothing is applied — silence is the safe default");
    j = world_identity_emit_json();
    check(LANE, json_has(j, "\"game_days\":30"),
          "the OPEN gap grows with the clock, so a harness can record it by polling");

    RestoreRecord rec = world_identity_declare(ident(kEpisodeA, kSaveA));
    check_eq_str(LANE, rec.decision, "restored_from_quarantine", "the declaration closes the gap");
    check(LANE, rec.gap_game_days == 30,
          "the decision carries the gap it closed: 30 game-days unsteered",
          "got " + std::to_string(rec.gap_game_days));

    j = world_identity_emit_json();
    check(LANE, json_has(j, "\"open\":false"), "the gap is published closed once steering resumes");
    check(LANE, json_has(j, "\"restored_from_quarantine\""),
          "the closed decision stays on the wire for the harness to read");
}

// ===========================================================================
// 5. THE CROSS-PROCESS CARRIER — /tmp/ck3-personality-locks.json.
// ===========================================================================
static void lane_disk_carrier() {
    printf("\n5. THE DISK CARRIER — the way this hazard could outlive the process\n");
    const char* LANE = "disk";

    use_temp_lockfile();

    // --- the writer stamps the identity -----------------------------------
    clean_slate();
    world_epoch_observe(world_fp(0x7f0000001000, raw_for(1066, 9, 15)));
    world_identity_declare(ident(kEpisodeA, kSaveA));
    set_live({{31808, 90}, {30853, 20}});
    {
        std::unique_lock<std::shared_mutex> lk(g_personality_lock_mutex);
        save_personality_locks_locked();
    }
    std::string body = read_lockfile();
    check(LANE, json_has(body, kSaveA), "the file records WHICH save the steering belongs to");
    check(LANE, json_has(body, kEpisodeA), "and which episode recorded it");
    check(LANE, json_has(body, "\"locks\""),
          "the payload keeps the old \"locks\" shape, so an old reader is not confused");

    WorldIdentity peeked;
    check(LANE, peek_personality_locks_identity(&peeked) && peeked.save_sha256 == kSaveA,
          "the header reads back");

    // --- a NEW process, same save, same episode: restore ------------------
    clean_slate();
    world_epoch_observe(world_fp(0x7f00000a1000, raw_for(1066, 9, 15)));
    RestoreRecord rec = world_identity_declare(ident(kEpisodeA, kSaveA));
    check_eq_str(LANE, rec.decision, "restored_from_disk",
                 "a fresh process resuming the same save of the same episode gets its steering back");
    check_eq_sz(LANE, personality_lock_count(), 2, "both entries loaded");
    check(LANE, live_has(31808, 90), "the axes survived the process boundary");

    // --- a NEW process, DIFFERENT save: refused ---------------------------
    clean_slate();
    world_epoch_observe(world_fp(0x7f00000b1000, raw_for(1066, 9, 15)));
    rec = world_identity_declare(ident(kEpisodeA, kSaveB));
    check_eq_str(LANE, rec.decision, "refused_different_save",
                 "a different save does not inherit the file — the hazard cannot escape the world");
    check_eq_sz(LANE, personality_lock_count(), 0, "nothing landed");

    // --- a DIFFERENT episode replaying the SAME save: refused -------------
    clean_slate();
    world_epoch_observe(world_fp(0x7f00000c1000, raw_for(1066, 9, 15)));
    rec = world_identity_declare(ident(kEpisodeB, kSaveA));
    check_eq_str(LANE, rec.decision, "refused_different_episode",
                 "two episodes sharing one host and one /tmp path do not share steering");
    check_eq_sz(LANE, personality_lock_count(), 0, "nothing landed");

    // --- a LEGACY, headerless file: never loadable ------------------------
    write_lockfile("{\"locks\":{\"31808\":{\"axes\":[90,90,90,90,90,90,90,90,90,90],"
                   "\"locked\":[true,true,true,true,true,true,true,true,true,true]}}}");
    clean_slate();
    world_epoch_observe(world_fp(0x7f00000d1000, raw_for(1066, 9, 15)));
    rec = world_identity_declare(ident(kEpisodeA, kSaveA));
    check_eq_str(LANE, rec.decision, "nothing_to_restore",
                 "a file written before ck-9fc3 names no save and can never be restored");
    check_eq_sz(LANE, personality_lock_count(), 0, "the pre-ck-9fc3 payload does not land");

    // --- a live world that is ALREADY steered is never stomped ------------
    // A perfectly matching file (same save, same episode) still loses to
    // steering the agent has already set against the world now loaded.
    clean_slate();
    world_epoch_observe(world_fp(0x7f00000e1000, raw_for(1066, 9, 15)));
    world_identity_declare(ident(kEpisodeA, kSaveA));
    set_live({{31808, 90}});
    {
        std::unique_lock<std::shared_mutex> lk(g_personality_lock_mutex);
        save_personality_locks_locked();
    }
    clean_slate();
    world_epoch_observe(world_fp(0x7f00000f1000, raw_for(1066, 9, 15)));
    set_live({{40001, 55}});
    rec = world_identity_declare(ident(kEpisodeA, kSaveA));
    check_eq_str(LANE, rec.decision, "refused_live_set_not_empty",
                 "a world that is already being steered is not overwritten from disk");
    check(LANE, live_has(40001, 55) && personality_lock_count() == 1,
          "the live steering is exactly what the agent set for THIS world");

    ::unlink(g_tmp_lockfile.c_str());
    set_personality_locks_path(nullptr);
}

// ===========================================================================
// 6. THE WIRE — the evidence a harness actually reads.
// ===========================================================================
static void lane_wire() {
    printf("\n6. THE WIRE — get_tick_info's \"save_identity\" fragment\n");
    const char* LANE = "wire";

    clean_slate();
    world_epoch_observe(world_fp(0x7f0000001000, raw_for(1066, 9, 15)));
    world_identity_declare(ident(kEpisodeA, kSaveA));
    set_live({{31808, 90}});
    world_epoch_observe(world_fp(0x7f0000009000, raw_for(1066, 9, 15)));
    world_identity_declare(ident(kEpisodeA, kSaveB));   // refused

    std::string j = world_identity_emit_json();

    // Balanced braces and a single top-level object: it is spliced verbatim
    // into get_tick_info via ResponseBuilder::value_raw, which does not validate.
    int depth = 0, minimum = 1;
    bool in_str = false, esc = false;
    for (char c : j) {
        if (esc) { esc = false; continue; }
        if (in_str) { if (c == '\\') esc = true; else if (c == '"') in_str = false; continue; }
        if (c == '"') in_str = true;
        else if (c == '{') depth++;
        else if (c == '}') { depth--; if (depth < minimum) minimum = depth; }
    }
    check(LANE, depth == 0 && minimum == 0 && !in_str && j.front() == '{' && j.back() == '}',
          "the fragment is one balanced JSON object");

    check(LANE, json_has(j, "\"decision\":\"refused_different_save\""),
          "the decision is on the wire");
    check(LANE, json_has(j, "\"recorded\":"), "with the identity the steering belongs to");
    check(LANE, json_has(j, "\"presented\":"), "and the identity that was declared");
    check(LANE, json_has(j, "\"resume_gap\":"), "and the resume-to-steer gap");
    check(LANE, json_has(j, "\"decisions\":["), "and a bounded ledger of prior decisions");
    check(LANE, json_has(j, "\"reload_game_date\":\"1066.09.15\""),
          "dates render through ck3::game_date — the real 365-day calendar (ck-05mg)");

    // The ledger is bounded: a flapping harness cannot grow get_tick_info.
    for (int i = 0; i < 40; i++) world_identity_declare(ident(kEpisodeA, kSaveB));
    j = world_identity_emit_json();
    size_t n = 0;
    for (size_t at = j.find("\"epoch\":"); at != std::string::npos; at = j.find("\"epoch\":", at + 1)) n++;
    check(LANE, n <= 12, "the published ledger stays bounded under 40 declarations",
          "epoch keys: " + std::to_string(n));
}

// ===========================================================================
// 7. CAN THIS TEST STILL SEE THE BUG? — the control on the control.
// ===========================================================================
static void lane_the_bug_is_reachable() {
    printf("\n7. THE CONTROL ON THE CONTROL — the fixture can still produce the bug\n");
    const char* LANE = "control";

    // Identical setup to lane 3, right up to the decision — but calling the
    // UNGUARDED primitive instead of going through the identity gate. If this
    // did NOT contaminate, lane 3's pass would prove nothing about the guard.
    clean_slate();
    world_epoch_observe(world_fp(0x7f0000001000, raw_for(1068, 6, 23)));
    world_identity_declare(ident(kEpisodeA, kSaveA));
    set_live({{31808, 90}, {30853, 20}});
    world_epoch_observe(world_fp(0x7f0000009000, raw_for(1066, 9, 15)));

    size_t moved = restore_quarantined_personality_locks();   // no identity asked for
    check_eq_sz(LANE, moved, 2, "the unguarded primitive DOES restore");
    check(LANE, live_has(31808, 90) && live_has(30853, 20),
          "and the previous world's axes ARE now sitting in the live write set, aimed at "
          "whoever holds those ids in the world now loaded — the ck-9fc3 symptom, on demand");
    check(LANE, personality_lock_count() == 2,
          "so lane 3's empty write set is the identity gate's doing, not the fixture's");

    // And the gate is the only production caller of that primitive — asserted
    // structurally by scripts/acceptance_save_identity.py, which greps the
    // shipped sources rather than trusting this comment.
}

// ===========================================================================
// 8. LOCK ORDER — the two paths run in opposite directions, concurrently.
// ===========================================================================
// world_identity_declare() takes the identity module's mutex and then
// g_personality_lock_mutex (to restore). save_personality_locks_locked() runs
// with g_personality_lock_mutex ALREADY HELD and needs the declared identity.
// Those are opposite orders, and the first draft of this bead had the writer
// reach back for world_identity_declared() — a textbook cycle that no
// single-threaded test can see. The identity is now PUSHED into a leaf-locked
// copy instead. This lane drives both directions at once and fails LOUDLY on a
// hang rather than sitting in CI forever.
static void lane_lock_order() {
    printf("\n8. LOCK ORDER — both directions, concurrently\n");
    const char* LANE = "locks";

    use_temp_lockfile();
    clean_slate();
    world_epoch_observe(world_fp(0x7f0000021000, raw_for(1066, 9, 15)));
    world_identity_declare(ident(kEpisodeA, kSaveA));
    set_live({{31808, 90}});

    std::atomic<bool> done{false};
    std::atomic<int>  writer_rounds{0};
    std::atomic<int>  declarer_rounds{0};

    std::thread writer([&] {
        for (int i = 0; i < 2000 && !done.load(); i++) {
            std::unique_lock<std::shared_mutex> lk(g_personality_lock_mutex);
            save_personality_locks_locked();     // personality -> identity copy
            writer_rounds++;
        }
    });
    std::thread declarer([&] {
        for (int i = 0; i < 2000 && !done.load(); i++) {
            world_identity_declare(ident(kEpisodeA, kSaveA));   // identity -> personality
            declarer_rounds++;
        }
    });

    // Watchdog: a cycle here is a HANG, and a hang is not a test result.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        if (writer_rounds.load() >= 2000 && declarer_rounds.load() >= 2000) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    bool finished = (writer_rounds.load() >= 2000 && declarer_rounds.load() >= 2000);
    if (!finished) {
        printf("  [%-9s] FAIL 2000 rounds of each direction complete without deadlock — "
               "STUCK at writer=%d declarer=%d\n", LANE, writer_rounds.load(), declarer_rounds.load());
        fflush(stdout);
        ::_exit(1);   // threads are wedged; a clean return would hang on join()
    }
    done = true;
    writer.join();
    declarer.join();
    check(LANE, true, "2000 rounds of each direction complete without deadlock");
    check(LANE, personality_lock_count() >= 1,
          "and the table is intact afterwards (no lost or duplicated entries)");

    ::unlink(g_tmp_lockfile.c_str());
    set_personality_locks_path(nullptr);
}

int main() {
    printf("acceptance_save_identity (ck-9fc3): quarantined steering is restored ONLY on the same save\n");
    printf("no CK3, no bridge socket — host only\n");

    lane_rule();
    lane_same_save_restores();
    lane_different_save_refused();
    lane_default_and_gap();
    lane_disk_carrier();
    lane_wire();
    lane_the_bug_is_reachable();
    lane_lock_order();

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    if (g_fails) {
        printf("acceptance_save_identity: FAIL\n");
        return 1;
    }
    printf("acceptance_save_identity: PASS\n");
    return 0;
}
