// test_war_history.cpp — host unit tests for the ended-war ledger and, above
// all, for the war-IDENTITY rule (bead ck-sd4d).
//
// THE DEFECT. An ended war was invisible to every client, so the paper's
// secondary outcome — the Norman Conquest's engine-recorded outcome and date —
// could not be reported at all. The meter lane had to leave both columns empty
// and bracket the end between two 90-day samples.
//
// THE SECOND DEFECT, and the one this file is mostly about. When the Conquest
// left the active set, a target-title fallback silently began metering the
// NORWEGIAN INVASION, because the scripted 1066 start ships two wars that both
// target k_england:
//     Norwegian Invasion   29767 -> 30853   (invasion_cb)
//     Norman Conquest      31808 -> 30853   (norman_conquest_cb)
// Identity must key on war_id, or on cb_type plus belligerents. Never on the
// targeted title.
//
// WHY A HOST TEST REACHES THESE. The ledger, the filter and the identity rule
// are pure data structures over integers and strings — war_history.cpp has no
// engine reads, no globals and no offsets header. The functions exercised here
// are exactly the ones do_query_war_history calls; nothing is re-implemented.
//
// WHAT A HOST TEST CANNOT REACH, stated plainly:
//   * that funcs::WAR_END_CORE is still the single termination path, that its
//     3rd argument is still the outcome, and that the CWar is still wiped
//     inside it. Those are claims about the binary, established by decompile
//     in re/findings/re_war_end_outcome.md and checked against a live game by
//     scripts/test_ended_wars_acceptance.py.
//   * that outcome 0 really is an attacker victory in a running campaign. Only
//     the live acceptance run shows that (William takes k_england).
//
// NOT assert(): NDEBUG erases it under the CI build type and the suite goes
// vacuously green (ck-6kvq.24). CHECK counts failures; main() returns them.

#include "war_history.h"

#include <cstdio>
#include <string>

using bridge::WarEndRecord;
using bridge::WarHistory;
using bridge::WarHistoryFilter;
using bridge::WarOutcome;
using bridge::filter_is_identifying;
using bridge::filter_is_title_only;
using bridge::war_matches;
using bridge::war_outcome_from_name;
using bridge::war_outcome_name;

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) {                                                        \
            ++g_failures;                                                     \
            printf("  FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__);       \
        }                                                                     \
    } while (0)

#define CHECK_STR(a, b, msg)                                                  \
    do {                                                                      \
        ++g_checks;                                                           \
        std::string _a = (a), _b = (b);                                       \
        if (_a != _b) {                                                       \
            ++g_failures;                                                     \
            printf("  FAIL: %s  (got \"%s\", want \"%s\")  (%s:%d)\n", (msg), \
                   _a.c_str(), _b.c_str(), __FILE__, __LINE__);               \
        }                                                                     \
    } while (0)

// ── the two scripted 1066 wars, as the engine ships them ────────────────────
static constexpr uint32_t K_ENGLAND      = 108;
static constexpr uint32_t HAROLD         = 30853;
static constexpr uint32_t WILLIAM        = 31808;
static constexpr uint32_t HARALD_HARDRADA = 29767;

static WarEndRecord norman_conquest() {
    WarEndRecord r;
    r.war_id = 1;
    r.outcome = static_cast<int>(WarOutcome::AttackerVictory);
    r.cb_type = "norman_conquest_cb";
    r.attacker_id = WILLIAM;
    r.defender_id = HAROLD;
    r.target_titles = {K_ENGLAND};
    r.end_total_days = 389971;   // 1068.07.xx-ish; exact value irrelevant here
    r.end_date_known = true;
    r.tick = 700;
    return r;
}

static WarEndRecord norwegian_invasion() {
    WarEndRecord r;
    r.war_id = 2;
    r.outcome = static_cast<int>(WarOutcome::DefenderVictory);
    r.cb_type = "invasion_cb";
    r.attacker_id = HARALD_HARDRADA;
    r.defender_id = HAROLD;
    r.target_titles = {K_ENGLAND};
    r.end_total_days = 389380;
    r.end_date_known = true;
    r.tick = 20;
    return r;
}

// ---------------------------------------------------------------------------
static void test_outcome_names() {
    printf("outcome enum <-> wire names\n");
    CHECK_STR(war_outcome_name(0), "attacker_victory", "0 is an attacker victory");
    CHECK_STR(war_outcome_name(1), "defender_victory", "1 is a defender victory");
    CHECK_STR(war_outcome_name(2), "white_peace", "2 is a white peace");
    CHECK_STR(war_outcome_name(3), "invalidated", "3 is invalidated");
    CHECK_STR(war_outcome_name(-1), "unknown", "an unset outcome reads unknown");
    CHECK_STR(war_outcome_name(99), "unknown",
              "an outcome the engine grew later must not be mislabelled");

    CHECK(war_outcome_from_name("attacker_victory") == WarOutcome::AttackerVictory,
          "round trip: attacker_victory");
    CHECK(war_outcome_from_name("white_peace") == WarOutcome::WhitePeace,
          "round trip: white_peace");
    // The engine's own trigger vocabulary is victory/defeat/white_peace/invalidated.
    CHECK(war_outcome_from_name("victory") == WarOutcome::AttackerVictory,
          "the engine's bare 'victory' is the attacker's");
    CHECK(war_outcome_from_name("defeat") == WarOutcome::DefenderVictory,
          "the engine's bare 'defeat' is the defender's victory");
    CHECK(war_outcome_from_name("nonsense") == WarOutcome::Unknown,
          "an unknown name does not silently become a real outcome");
    CHECK(war_outcome_from_name(nullptr) == WarOutcome::Unknown,
          "a null name does not crash or become a real outcome");
}

// ---------------------------------------------------------------------------
// THE REGRESSION. Both 1066 wars in one ledger; the target title must never
// be what tells them apart.
static void test_the_two_1066_wars_are_never_confused() {
    printf("the two 1066 k_england wars are never confused\n");
    WarHistory h;
    h.record(norwegian_invasion());
    h.record(norman_conquest());
    CHECK(h.size() == 2, "both wars are in the ledger");

    // 1. The honest query: CB type plus both belligerents.
    WarHistoryFilter f;
    f.has_cb_type = true;   f.cb_type = "norman_conquest_cb";
    f.has_attacker = true;  f.attacker_id = WILLIAM;
    f.has_defender = true;  f.defender_id = HAROLD;
    CHECK(filter_is_identifying(f), "cb_type + belligerents identifies a war");
    auto rows = h.query(f);
    CHECK(rows.size() == 1, "exactly one war matches the Conquest's identity");
    if (rows.size() == 1) {
        CHECK(rows[0].war_id == 1, "and it is the Conquest, not the Invasion");
        CHECK_STR(war_outcome_name(rows[0].outcome), "attacker_victory",
                  "the Conquest's outcome comes back with it");
    }

    // 2. Belligerents alone also identify: both wars share the DEFENDER, so a
    //    defender-only filter must NOT be treated as identifying.
    WarHistoryFilter def_only;
    def_only.has_defender = true; def_only.defender_id = HAROLD;
    CHECK(!filter_is_identifying(def_only),
          "a shared defender alone identifies nothing - Harold is defending both");
    CHECK(h.query(def_only).size() == 2,
          "and it honestly returns both, rather than picking one");

    // 3. THE TRAP. A title-only query is refused, not answered.
    WarHistoryFilter title_only;
    title_only.has_target_title = true; title_only.target_title = K_ENGLAND;
    CHECK(filter_is_title_only(title_only),
          "a bare k_england query is the target-title trap");
    CHECK(!filter_is_identifying(title_only),
          "and a target title never identifies a war");
    // Were it ever answered, it would match BOTH - which is exactly how the
    // meter lane started metering the wrong war.
    CHECK(h.query(title_only).size() == 2,
          "the title genuinely matches both wars, so answering it would be a coin flip");

    // 4. A title used to NARROW an already-identified query is fine.
    WarHistoryFilter narrowed = f;
    narrowed.has_target_title = true; narrowed.target_title = K_ENGLAND;
    CHECK(!filter_is_title_only(narrowed),
          "a title alongside cb_type + belligerents is a narrowing hint, not an identity");
    CHECK(narrowed.has_target_title && h.query(narrowed).size() == 1,
          "and it still selects exactly the Conquest");

    // 5. war_id alone identifies.
    WarHistoryFilter by_id;
    by_id.has_war_id = true; by_id.war_id = 2;
    CHECK(filter_is_identifying(by_id), "war_id identifies a war");
    auto only = h.query(by_id);
    CHECK(only.size() == 1 && only[0].cb_type == "invasion_cb",
          "war_id 2 is the Norwegian Invasion");
}

// ---------------------------------------------------------------------------
static void test_ledger_mechanics() {
    printf("ledger mechanics\n");
    WarHistory h;
    CHECK(h.size() == 0, "a fresh ledger is empty");
    CHECK(h.total_recorded() == 0, "and has observed nothing");

    WarEndRecord a = norman_conquest();
    h.record(a);
    h.record(a);   // same war, same tick
    CHECK(h.size() == 1, "a war that ends once is recorded once");
    CHECK(h.total_recorded() == 1, "and counted once");

    // Newest first.
    WarEndRecord b = norwegian_invasion();
    b.tick = 9000;
    h.record(b);
    auto all = h.query(WarHistoryFilter{});
    CHECK(all.size() == 2, "an empty filter browses the whole ledger");
    CHECK(all.size() == 2 && all[0].war_id == b.war_id,
          "newest first - the most recent termination leads");

    // since_tick.
    WarHistoryFilter recent;
    recent.since_tick = 1000;
    CHECK(h.query(recent).size() == 1, "since_tick drops older terminations");

    // limit.
    WarHistoryFilter one;
    one.limit = 1;
    CHECK(h.query(one).size() == 1, "limit caps the result");

    // The bound drops the OLDEST, and total_recorded keeps counting past it.
    WarHistory ring;
    for (size_t i = 0; i < WarHistory::kMaxRecords + 10; ++i) {
        WarEndRecord r = norman_conquest();
        r.war_id = static_cast<uint32_t>(i + 1);
        r.tick = i + 1;
        ring.record(r);
    }
    CHECK(ring.size() == WarHistory::kMaxRecords, "the ring is bounded");
    CHECK(ring.total_recorded() == WarHistory::kMaxRecords + 10,
          "but the observed-termination count is not truncated by the bound");
    WarHistoryFilter oldest;
    oldest.has_war_id = true; oldest.war_id = 1;
    CHECK(ring.query(oldest).empty(), "the oldest record was the one dropped");

    // clear() is the world-reset path (ck-hxh7): records go, the observation
    // counter stays, because it is what tells an operator the hook is alive.
    ring.clear();
    CHECK(ring.size() == 0, "clear empties the ledger");
    CHECK(ring.total_recorded() == WarHistory::kMaxRecords + 10,
          "clear does not erase the evidence that the hook fired");
}

// ---------------------------------------------------------------------------
static void test_matching_is_conjunctive() {
    printf("filters are conjunctive\n");
    WarEndRecord r = norman_conquest();
    WarHistoryFilter f;
    f.has_cb_type = true;  f.cb_type = "norman_conquest_cb";
    f.has_attacker = true; f.attacker_id = WILLIAM;
    CHECK(war_matches(r, f), "the Conquest matches its own cb + attacker");
    f.has_defender = true; f.defender_id = 12345;
    CHECK(!war_matches(r, f), "a wrong defender rejects the match outright");

    WarHistoryFilter t;
    t.has_target_title = true; t.target_title = 999;
    CHECK(!war_matches(r, t), "a title the war never targeted does not match");
}

// ---------------------------------------------------------------------------
// ck-yifp. THE ENGINE MOVES A BELLIGERENT, so a filter that pins the pair the
// war was DECLARED with can miss the row the engine actually wrote.
//
// Not hypothetical. Smoke episode 1's own ledger, through this very hook:
//     war 0 ended defender_victory at 1070.05.04 (cb=norwegian_invasion_cb 29767->31808)
// declared 29767 -> 30853. William took k_england in 1068 and the war's primary
// defender followed the contested title to him. The meter lane asked this
// ledger with defender_id pinned at 30853 and would have come back empty on a
// war that IS in it — an absence reading as "no end".
static void test_a_moved_defender_defeats_a_frozen_pair_filter() {
    printf("a reassigned defender defeats a declared-pair filter (ck-yifp)\n");

    // The row as the engine wrote it: declared against HAROLD, terminated
    // against the character who by then held the title.
    WarEndRecord moved;
    moved.war_id = 0;
    moved.cb_type = "norwegian_invasion_cb";
    moved.attacker_id = HARALD_HARDRADA;
    moved.defender_id = WILLIAM;      // NOT the declared defender
    moved.outcome = static_cast<int>(WarOutcome::DefenderVictory);

    WarHistory ledger;
    ledger.record(moved);

    // The pre-ck-yifp filter shape: cb_type + the DECLARED pair.
    WarHistoryFilter frozen;
    frozen.has_cb_type = true;  frozen.cb_type = "norwegian_invasion_cb";
    frozen.has_attacker = true; frozen.attacker_id = HARALD_HARDRADA;
    frozen.has_defender = true; frozen.defender_id = HAROLD;
    CHECK(filter_is_identifying(frozen), "the frozen-pair filter IS identifying");
    CHECK(ledger.query(frozen).empty(),
          "a declared-pair filter MISSES the row the engine wrote - this is the "
          "defect, asserted so it cannot come back silently");

    // The handle that survives it. war_id alone is identifying by itself.
    WarHistoryFilter by_id;
    by_id.has_war_id = true; by_id.war_id = 0;
    CHECK(filter_is_identifying(by_id), "a war_id alone identifies a war");
    CHECK(ledger.query(by_id).size() == 1,
          "the war_id finds the row whatever happened to the belligerents");

    // And cb_type + the ATTACKER, which is the fallback for an observer that
    // never held the id. The attacker is the half that did not move.
    WarHistoryFilter by_atk;
    by_atk.has_cb_type = true;  by_atk.cb_type = "norwegian_invasion_cb";
    by_atk.has_attacker = true; by_atk.attacker_id = HARALD_HARDRADA;
    CHECK(filter_is_identifying(by_atk), "cb_type + attacker identifies a war");
    CHECK(ledger.query(by_atk).size() == 1,
          "cb_type + attacker finds it without pinning the moved half");
}

// ---------------------------------------------------------------------------
// ck-yifp. A termination the hook SAW and could not copy out must not be
// silently identical to a war that never ended.
static void test_uncaptured_terminations_are_counted_separately() {
    printf("an uncapturable termination is counted, not swallowed (ck-yifp)\n");
    WarHistory ledger;
    CHECK(ledger.total_uncaptured() == 0, "a fresh ledger has dropped nothing");

    ledger.record(norman_conquest());
    ledger.note_uncaptured();
    ledger.note_uncaptured();

    CHECK(ledger.total_recorded() == 1, "one row was recorded");
    CHECK(ledger.total_uncaptured() == 2, "two terminations were dropped");
    CHECK(ledger.size() == 1, "the dropped ones are NOT in the ledger");

    // clear() is the world reset. It empties the world's rows and leaves both
    // process-health counters alone, for the same reason: they say whether the
    // hook is working, not what is in the loaded world.
    ledger.clear();
    CHECK(ledger.size() == 0, "clear empties the ledger");
    CHECK(ledger.total_uncaptured() == 2,
          "clear does not erase the evidence that the hook dropped something");
}

int main() {
    printf("== test_war_history (ck-sd4d, ck-yifp) ==\n");
    test_outcome_names();
    test_the_two_1066_wars_are_never_confused();
    test_ledger_mechanics();
    test_matching_is_conjunctive();
    test_a_moved_defender_defeats_a_frozen_pair_filter();
    test_uncaptured_terminations_are_counted_separately();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_checks == 0) {
        printf("NO CHECKS RAN - the suite is vacuous\n");
        return 1;
    }
    return g_failures;
}
