// test_unavailability.cpp — host unit tests for per-session unavailability
// accounting and the episode policy.
//
// ck-6kvq.10 built the accounting (R3 as reversed: the clock runs and a capped
// agent's cost is MEASURED, not prevented). ck-ijfa replaced the GATE, on the
// progenitor's ruling of 2026-09-13:
//
//   ANOMALOUS IS A WALL-CLOCK JUDGEMENT. A gap between a control session's
//   activities counts as unavailability only if it exceeds 3 heartbeat
//   intervals = 360 WALL-SECONDS. `unavailable_game_days` is then the GAME-DAYS
//   those anomalous gaps SPANNED, and more than X of them invalidates.
//
// ck-2ngg then made X ITSELF A RULE: "5% of the episode's DECLARED horizon,
// unsteered". 365 is that rule evaluated at the registered 20-game-year horizon
// (7300 game-days) and is wrong at every other one — on the smoke horizon
// (1568 game-days) the rule says 78 and the constant said 365, 4.7x too
// permissive. Test 15 is that rule; test 16 is the ledger with no X at all.
//
// Design of record: docs/design/event-pipeline-conductor.md §2.7, §2.10, §6.1.
// Pre-registration §7.1 (the covariate) and §7.3 (X = 5% of the declared
// horizon; 365 at the registered 20-game-year one — ck-2ngg).
//
// No CK3 binary, no game, no engine: the ledger takes GAME DAY NUMBERS and
// MONOTONIC WALL MILLISECONDS, both passed in. Two units on purpose — the gate
// is in the AGENT's clock (lateness is a property of the agent) and the
// covariate is in the WORLD's clock (what the absence cost the experiment).
//
// THE NEGATIVE CONTROL (test 9) is the point, and it is the one e0 specified:
// the SUPERSEDED mechanism — every gap of at least 30 game-days counted in
// full — is run against the SAME healthy heartbeat history and must FAIL. Same
// input, same rate, ZERO versus thousands of game-days: that single case
// discriminates the two mechanisms completely.

#include "unavailability.h"
#include "episode_policy.h"
#include "exclusion_rule.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace bridge;

// ---------------------------------------------------------------- harness ---
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

#define CHECK_EQ(a, b, msg)                                                   \
    do {                                                                      \
        ++g_checks;                                                           \
        auto _a = (a); auto _b = (b);                                         \
        if (!(_a == _b)) {                                                    \
            ++g_failures;                                                     \
            printf("  FAIL: %s  (got %lld, want %lld)  (%s:%d)\n", (msg),     \
                   (long long)_a, (long long)_b, __FILE__, __LINE__);         \
        }                                                                     \
    } while (0)

static void section(const char* name) { printf("\n[%s]\n", name); }

// A fresh ledger with the pre-registered numbers: the 360-wall-second anomaly
// gate, and X = 365 game-days — which is NOT a constant but the rule's answer
// at the REGISTERED horizon (1066.09.15 -> 1086.09.15, 7300 game-days x 0.05).
// Every legacy test below models an episode on that horizon, which is why its
// numbers are unchanged; `armed` is stated so no lane can inherit it silently.
static UnavailabilityLedger& fresh(int64_t anomaly_wall_s = 360,
                                   int64_t x = kExclusionDaysAtRegisteredHorizon,
                                   bool armed = true) {
    UnavailabilityLedger& l = unavailability();
    l.reset();
    l.configure(anomaly_wall_s, x, armed);
    return l;
}

static const UnavailSession* find(const std::vector<UnavailSession>& v,
                                  const char* id) {
    for (const UnavailSession& u : v) if (u.session_id == id) return &u;
    return nullptr;
}

// 1066.09.15 as an absolute day number, so the arithmetic below reads like the
// episode it is modelling rather than like day 0.
static const int64_t D0 = 1066 * 365 + 243 + 14;   // year*365 + cum(Sep) + (15-1)

// The episode's wall clock, monotonic ms, starting somewhere non-zero so a
// missing stamp (0 == "not stamped") can never be mistaken for a real one.
static const uint64_t W0 = 1'000'000;

// ── THE TWO NAMED CONDITIONS these tests model ─────────────────────────────
//
// Every wall-clock number below is stated WITH its rate and condition, because
// a threshold quoted without them is the defect this bead was filed for.
//
// FAST: the E0 NULL free-running rate measured on the node and corroborated by
//   Appendix A.19 — ~1.73 game-days per wall-second. One R3 heartbeat interval
//   (120 wall-seconds) spans 208 game-days here.
static const int64_t kFastDaysPerBeat = 208;      // 208 days / 120 s = 1.733 gd/s
//
// SLOW: the ~0.75 game-years/hour WORKED EXAMPLE the retired numbers were
//   derived at — 0.076 game-days per wall-second. Used where a test needs an
//   anomalous gap SHORTER than X, which the fast rate cannot produce (see
//   test 13's finding).
static const uint64_t kBeatMs = 120'000;          // the R3 heartbeat, 120 s

// ===========================================================================
// 1. e0 CONTROL #1 — A HEALTHY HEARTBEAT ACCRUES ZERO
// ===========================================================================
// The discriminating case. 120 wall-seconds is under the 360-second gate, so a
// perfectly available agent accrues NOTHING — at a rate where every one of its
// gaps spans 208 GAME-days, which under the superseded game-day gate was
// counted in full (test 9).
static void test_healthy_heartbeat_accrues_nothing() {
    section("e0 CONTROL #1: a 120s heartbeat at 1.73 game-days/wall-second accrues ZERO");
    UnavailabilityLedger& l = fresh();

    l.note_session("s1", D0, W0);
    int64_t  day  = D0;
    uint64_t wall = W0;
    // 120 beats: deliberately inside kMaxRetainedSpans, so test 9 can re-derive
    // the superseded rule from the ITEMISED record rather than from a total.
    for (int i = 0; i < 120; ++i) {
        day  += kFastDaysPerBeat;
        wall += kBeatMs;
        l.note_day(day, wall);
        l.note_activity("s1", day, wall);
    }

    const UnavailSession* u = find(l.snapshot(), "s1");
    CHECK(u != nullptr, "the control session is tracked");
    CHECK_EQ(u->unavailable_game_days, 0,
             "ZERO: no gap exceeds the 360-wall-second gate, so an agent doing "
             "exactly what R3 prescribes accrues no unavailability");
    CHECK_EQ(u->span_count, 0u, "and no anomalous span is recorded");
    CHECK_EQ(u->unavailable_wall_seconds, 0.0, "in either unit");
    CHECK_EQ(u->activity_count, 120u, "every touch counted");
    CHECK_EQ(u->last_activity_day, day, "the cursor is at the last touch");
    CHECK(!u->over_threshold, "nowhere near X");
    CHECK(!l.episode_invalid(), "the episode is valid");

    // THE RAW TOTALS are still exact, in BOTH units, so an analysis can
    // re-derive a different gate from the record without re-running anything.
    CHECK_EQ(u->gap_count, 120u, "every gap is still RECORDED");
    CHECK_EQ(u->silent_game_days, 120 * kFastDaysPerBeat,
             "and every silent game-day is counted exactly");
    CHECK_EQ((int64_t)(u->silent_wall_seconds + 0.5), 120 * 120,
             "beside the wall-seconds they took — the pairing that was missing");
    CHECK(u->wall_measured, "the record is wall-stamped");
    CHECK_EQ(u->unjudged_gap_count, 0u, "and every gap was judgeable");
}

// ===========================================================================
// 2. e0 CONTROL #2 — A TEN-MINUTE SILENCE IS ONE ANOMALOUS SPAN
// ===========================================================================
static void test_ten_minute_silence_is_one_anomaly() {
    section("e0 CONTROL #2: a 10-wall-minute silence accrues its spanned game-days "
            "as ONE anomalous span");
    UnavailabilityLedger& l = fresh();

    l.note_session("s1", D0, W0);
    l.note_activity("s1", D0, W0);

    // Ten wall-minutes of silence. The clock keeps running — that IS the G3'
    // reversal — so the world moves 600 s x 1.733 = 1040 game-days meanwhile.
    const int64_t  dark_days = 5 * kFastDaysPerBeat;      // 1040
    const uint64_t dark_ms   = 5 * kBeatMs;               // 600 s
    for (int i = 1; i <= 5; ++i)
        l.note_day(D0 + i * kFastDaysPerBeat, W0 + (uint64_t)i * kBeatMs);

    std::vector<UnavailSession> snap = l.snapshot();
    const UnavailSession* u = find(snap, "s1");
    CHECK_EQ(u->span_count, 1u, "ONE anomalous span");
    CHECK_EQ(u->unavailable_game_days, dark_days,
             "measured as the GAME-DAYS it spanned — what the absence cost the "
             "experiment, not how late the agent was");
    CHECK_EQ((int64_t)(u->unavailable_wall_seconds + 0.5), (int64_t)(dark_ms / 1000),
             "and the same gap in wall-seconds, which is what the GATE read");
    CHECK(u->spans.size() == 1 && u->spans[0].open, "it is flagged OPEN");
    CHECK(u->spans[0].anomalous, "and ANOMALOUS");
    CHECK_EQ(u->spans[0].start_day, D0, "starting at the last activity");
    CHECK_EQ(u->spans[0].end_day, D0 + dark_days, "ending at the current day");

    // The agent comes back. The span closes at exactly the same number — an open
    // span is computed, never accumulated, so closing it cannot double-count.
    l.note_activity("s1", D0 + dark_days, W0 + dark_ms);
    u = find((snap = l.snapshot()), "s1");
    CHECK_EQ(u->unavailable_game_days, dark_days, "closing does not double-count");
    CHECK_EQ(u->span_count, 1u, "still one span");
    CHECK(!u->spans[0].open, "now closed");
    CHECK(u->spans[0].anomalous, "and still anomalous");

    // A HEALTHY beat immediately afterwards adds NOTHING, even though it spans
    // another 208 game-days. The gate is doing the work, not the clock.
    l.note_day(D0 + dark_days + kFastDaysPerBeat, W0 + dark_ms + kBeatMs);
    l.note_activity("s1", D0 + dark_days + kFastDaysPerBeat, W0 + dark_ms + kBeatMs);
    u = find((snap = l.snapshot()), "s1");
    CHECK_EQ(u->unavailable_game_days, dark_days,
             "a healthy beat after an anomaly adds nothing");
    CHECK_EQ(u->gap_count, 2u, "though it IS recorded as a gap");
}

// ===========================================================================
// 3. OBSERVER TRAFFIC NEVER COUNTS (§2.7, literal)
// ===========================================================================
static void test_observer_traffic_never_counts() {
    section("observer traffic never counts");
    UnavailabilityLedger& l = fresh();

    // The engine glue never registers an observe session, so the ledger sees an
    // unknown id. It must be ignored — silently, and without creating a record
    // that would then start accruing against the episode.
    l.note_activity("obs-1", D0 + 5, W0 + 3000);
    l.note_day(D0 + 400, W0 + 600'000);
    CHECK_EQ(l.snapshot().size(), 0u, "an unregistered id creates no record");
    CHECK(!l.episode_invalid(), "and cannot invalidate an episode");

    // A control session's own reads DO count — activity is "any submission OR
    // read". The ledger cannot tell them apart, which is the point: the glue
    // calls it before the legacy/pipeline fork, for both.
    l.note_session("s1", D0, W0);
    l.note_activity("s1", D0 + 400, W0 + 600'000);
    l.note_activity("obs-1", D0 + 400, W0 + 600'000);
    CHECK_EQ(l.snapshot().size(), 1u, "still only the control session is tracked");
}

// ===========================================================================
// 4. DETACH / RESUME — spans sum correctly across a dropped socket
// ===========================================================================
static void test_detach_resume_sums() {
    section("spans sum correctly across detach and resume");
    UnavailabilityLedger& l = fresh();

    l.note_session("s1", D0, W0);
    l.note_activity("s1", D0, W0);

    // The socket drops. Under the claim pin the session is NOT destroyed and
    // NOT released — and its silence keeps accruing, because the character is
    // still its character and still running on its last steer.
    l.note_attached("s1", false);
    const int64_t  dark_days = 5 * kFastDaysPerBeat;
    const uint64_t dark_ms   = 5 * kBeatMs;             // 600 s > the 360 s gate
    for (int i = 1; i <= 5; ++i)
        l.note_day(D0 + i * kFastDaysPerBeat, W0 + (uint64_t)i * kBeatMs);

    const UnavailSession* u = find(l.snapshot(), "s1");
    CHECK(!u->attached, "recorded as detached");
    CHECK(!u->ended, "but NOT ended — the claim is pinned");
    CHECK_EQ(u->unavailable_game_days, dark_days, "and accruing through the detachment");

    // bead .5's resume: the SAME session_id is re-registered. That is an ATTACH,
    // never a reset — zeroing the cursor here would erase the very gap the
    // covariate exists to measure.
    l.note_session("s1", D0 + dark_days, W0 + dark_ms);
    u = find(l.snapshot(), "s1");
    CHECK(u->attached, "re-attached");
    CHECK_EQ(u->unavailable_game_days, dark_days,
             "THE RESUME DID NOT ERASE THE GAP (the whole point of the covariate)");
    CHECK_EQ(u->first_day, D0, "and the session's first day is unchanged");

    l.note_activity("s1", D0 + dark_days, W0 + dark_ms);
    u = find(l.snapshot(), "s1");
    CHECK_EQ(u->unavailable_game_days, dark_days, "the resumed span closes at the same value");
    CHECK_EQ(u->span_count, 1u, "one span across the whole detach/resume cycle");
}

// ===========================================================================
// 5. A PHANTOM SESSION inflates the covariate; it does not vanish
// ===========================================================================
static void test_phantom_session_keeps_accruing() {
    section("a session that is gone for good keeps accruing");
    UnavailabilityLedger& l = fresh();

    l.note_session("s1", D0, W0);
    l.note_activity("s1", D0, W0);
    l.note_session_ended("s1");            // refused resume, or free-play destroy

    for (int i = 1; i <= 10; ++i)
        l.note_day(D0 + i * kFastDaysPerBeat, W0 + (uint64_t)i * kBeatMs);

    const UnavailSession* u = find(l.snapshot(), "s1");
    CHECK(u != nullptr, "the record SURVIVES the session");
    CHECK(u->ended, "marked ended");
    CHECK_EQ(u->unavailable_game_days, 10 * kFastDaysPerBeat, "and still accruing");
    CHECK(u->over_threshold, "well past X=365, so this session invalidates");
    CHECK(l.episode_invalid(),
          "a phantom session inflates the covariate to invalidation rather than "
          "quietly disappearing from the record (§2.7 knock-on to ck-6kvq.5)");
}

// ===========================================================================
// 6. X = 365 GAME-DAYS: the bridge FLAGS, it does not exclude
// ===========================================================================
// MODELLED AT THE SLOW CONDITION, and deliberately: at the fast free-running
// rate no anomalous gap can be SMALLER than X (test 13's finding), so a test
// that needs to sit exactly ON X has to name a slower rate. ~0.91 game-days per
// wall-second here — 365 game-days in 400 wall-seconds.
static void test_exclusion_threshold() {
    section("X = 365 cumulative GAME-DAYS spanned by anomalous gaps");
    UnavailabilityLedger& l = fresh();
    CHECK_EQ(l.exclusion_days(), 365,
             "X at the REGISTERED horizon (ck-2ngg: the rule's answer at "
             "1066.09.15 -> 1086.09.15, not a constant)");
    CHECK(l.exclusion_armed(), "and it is ARMED, so a verdict means something");
    CHECK_EQ(l.anomaly_wall_seconds(), 360, "and the RULED gate (2026-09-13)");

    l.note_session("s1", D0, W0);
    l.note_activity("s1", D0, W0);

    // Exactly X is NOT over — "more than 365" (§7.3).
    l.note_day(D0 + 365, W0 + 400'000);
    l.note_activity("s1", D0 + 365, W0 + 400'000);
    const UnavailSession* u = find(l.snapshot(), "s1");
    CHECK(u->spans[0].anomalous, "400 wall-seconds is over the 360-second gate");
    CHECK_EQ(u->unavailable_game_days, 365, "sitting exactly on X");
    CHECK(!u->over_threshold, "exactly X is NOT over X");
    CHECK(!l.episode_invalid(), "so the episode is still valid");

    // A second anomalous gap tips it.
    l.note_day(D0 + 396, W0 + 800'000);
    l.note_activity("s1", D0 + 396, W0 + 800'000);
    u = find(l.snapshot(), "s1");
    CHECK_EQ(u->unavailable_game_days, 396, "cumulative, across two anomalous spans");
    CHECK(u->over_threshold, "now over X");
    CHECK(l.episode_invalid(), "and the episode is flagged INVALID");

    // Flagged, not acted on: nothing here stops, refuses, or deletes anything.
    // The archive-and-report decision is the analysis's (§7.3).
    CHECK_EQ(l.snapshot().size(), 1u, "the record is intact, not dropped");
}

// ===========================================================================
// 7. Time hygiene: monotone days, same-day traffic, and no rewinds
// ===========================================================================
static void test_time_hygiene() {
    section("the current day is monotone and a rewind is counted, not applied");
    UnavailabilityLedger& l = fresh();

    l.note_session("s1", D0, W0);
    l.note_day(D0 + 100, W0 + 400'000);
    CHECK_EQ(l.current_day(), D0 + 100, "advanced");

    l.note_day(D0 + 40, W0 + 401'000);   // a reload, or a racing read
    CHECK_EQ(l.current_day(), D0 + 100, "a lower day is NOT applied");
    CHECK_EQ(l.day_regressions(), 1u, "it is counted, so it is visible");

    // Several actions on one game day (the common case) close nothing.
    l.note_activity("s1", D0 + 100, W0 + 402'000);
    l.note_activity("s1", D0 + 100, W0 + 403'000);
    l.note_activity("s1", D0 + 100, W0 + 404'000);
    const UnavailSession* u = find(l.snapshot(), "s1");
    CHECK_EQ(u->activity_count, 3u, "all three counted as activity");
    CHECK_EQ(u->span_count, 1u, "one anomalous span: the 402 wall-seconds before "
                                "the first of them");
    CHECK_EQ(u->unavailable_game_days, 100, "and it is counted exactly once");

    // Late-dated activity never rewinds the cursor.
    l.note_activity("s1", D0 + 60, W0 + 405'000);
    u = find(l.snapshot(), "s1");
    CHECK_EQ(u->last_activity_day, D0 + 100, "the cursor does not go backwards");
    CHECK_EQ(u->unavailable_game_days, 100, "and no gap is re-opened");
}

// ===========================================================================
// 8. A session that connects and never speaks is unavailable from connect
// ===========================================================================
// PRESERVED THROUGH THE GATE CHANGE, because the semantics carry even though
// the numbers do not: this is ck-zgh4's SILENT arm, and a session that connects
// and says nothing must be anomalous from the moment it connected.
static void test_silent_from_connect() {
    section("a session that handshakes and never speaks");
    UnavailabilityLedger& l = fresh();

    l.note_session("s1", D0, W0);
    for (int i = 1; i <= 8; ++i)          // 8 beats = 960 wall-seconds of silence
        l.note_day(D0 + i * kFastDaysPerBeat, W0 + (uint64_t)i * kBeatMs);

    const UnavailSession* u = find(l.snapshot(), "s1");
    CHECK_EQ(u->activity_count, 0u, "it never spoke");
    CHECK_EQ(u->span_count, 1u, "one anomalous span");
    CHECK_EQ(u->unavailable_game_days, 8 * kFastDaysPerBeat,
             "and it is unavailable from the moment it CONNECTED, not from its "
             "first (never-arriving) action");
    CHECK_EQ((int64_t)(u->unavailable_wall_seconds + 0.5), 960,
             "960 wall-seconds, which is what the gate judged");
    CHECK(l.episode_invalid(), "and it is past X");
}

// ===========================================================================
// 9. THE NEGATIVE CONTROL — the SUPERSEDED mechanism, on the same input
// ===========================================================================
// e0 specified this one, and it is the discriminating case: the pre-ck-ijfa
// rule was "every gap of at least min_span_days (30) GAME-DAYS counts IN FULL".
// Run it over the SAME healthy heartbeat history test 1 accrues ZERO on.
//
// It is computed from the RECORDED gaps rather than by reconfiguring the ledger
// — which is the point twice over: the old rule is gone from the product, and
// the record kept in both units is rich enough to re-derive ANY gate offline,
// which is the property whose absence let a 26x error live in a comment block.
static void test_negative_control_superseded_mechanism() {
    section("NEGATIVE CONTROL: the SUPERSEDED game-day gate invalidates a HEALTHY episode");

    UnavailabilityLedger& l = fresh();
    l.note_session("s1", D0, W0);
    int64_t  day  = D0;
    uint64_t wall = W0;
    for (int i = 0; i < 120; ++i) {
        day  += kFastDaysPerBeat;
        wall += kBeatMs;
        l.note_day(day, wall);
        l.note_activity("s1", day, wall);
    }
    const UnavailSession* u = find(l.snapshot(), "s1");

    // THE SHIPPED RULE.
    CHECK_EQ(u->unavailable_game_days, 0,
             "THE SHIPPED RULE: a healthy 120-second heartbeat accrues ZERO");
    CHECK(!l.episode_invalid(), "and the episode is VALID");

    // THE SUPERSEDED RULE, re-derived from the same recorded gaps.
    const int64_t kRetiredMinSpanDays = 30;
    int64_t superseded = 0;
    size_t  superseded_spans = 0;
    for (const UnavailSpan& sp : u->spans) {
        if (sp.days >= kRetiredMinSpanDays) { superseded += sp.days; ++superseded_spans; }
    }
    CHECK_EQ(superseded_spans, 120u,
             "under the retired game-day gate EVERY one of the 120 healthy beats "
             "is a 'stall', because 208 game-days >= 30");
    CHECK(superseded > 365,
          "and they are counted IN FULL, so the covariate blows through X=365 "
          "long before the horizon — a perfectly available agent invalidating "
          "its own episode");
    CHECK_EQ(superseded, u->silent_game_days,
             "in fact it EQUALS total elapsed game-days: the gaps partition the "
             "timeline, so the retired rule did not correlate with elapsed time, "
             "it measured it");

    // SAME INPUT, SAME RATE, OPPOSITE VERDICT. 0 versus 33280 game-days: the
    // gate is what does the work, and it is demonstrably in the path.
    CHECK(u->unavailable_game_days == 0 && superseded == 120 * kFastDaysPerBeat,
          "ZERO under the shipped gate, 24960 game-days under the retired one, "
          "on the SAME input at the SAME rate — one case discriminates the two "
          "mechanisms completely");
}

// ===========================================================================
// 10. Span retention is bounded; the totals stay exact; anomalies always get in
// ===========================================================================
static void test_span_retention_is_bounded() {
    section("span retention is capped, the totals stay exact, and an ANOMALY always gets in");
    UnavailabilityLedger& l = fresh();
    l.note_session("s1", D0, W0);
    l.note_activity("s1", D0, W0);

    // Anomalous gaps: 400 wall-seconds each (> the 360 s gate), 40 game-days each.
    const int64_t  kGapDays = 40;
    const uint64_t kGapMs   = 400'000;
    const size_t   kStalls  = kMaxRetainedSpans + 17;
    int64_t  day  = D0;
    uint64_t wall = W0;
    for (size_t i = 0; i < kStalls; ++i) {
        day  += kGapDays;
        wall += kGapMs;
        l.note_day(day, wall);
        l.note_activity("s1", day, wall);
    }

    const UnavailSession* u = find(l.snapshot(), "s1");
    CHECK_EQ(u->span_count, kStalls, "the anomalous COUNT is exact");
    CHECK_EQ(u->unavailable_game_days, (int64_t)kStalls * kGapDays, "the TOTAL is exact");
    CHECK_EQ(u->spans.size(), kMaxRetainedSpans, "only the itemisation is capped");
    CHECK(u->spans_truncated, "and the truncation is declared, not hidden");

    // AN ANOMALOUS GAP EVICTS A NON-ANOMALOUS ONE. Ordinary turn-taking must not
    // be able to crowd the covariate's own evidence out of the record.
    UnavailabilityLedger& l2 = fresh();
    l2.note_session("s2", D0, W0);
    l2.note_activity("s2", D0, W0);
    day = D0; wall = W0;
    for (size_t i = 0; i < kMaxRetainedSpans; ++i) {     // healthy beats, all retained
        day += kFastDaysPerBeat; wall += kBeatMs;
        l2.note_day(day, wall); l2.note_activity("s2", day, wall);
    }
    const UnavailSession* a = find(l2.snapshot(), "s2");
    CHECK_EQ(a->spans.size(), kMaxRetainedSpans, "the list is FULL of healthy gaps");
    CHECK_EQ(a->span_count, 0u, "none of which is anomalous");

    day += 40; wall += 400'000;                          // one real anomaly
    l2.note_day(day, wall); l2.note_activity("s2", day, wall);
    a = find(l2.snapshot(), "s2");
    CHECK_EQ(a->span_count, 1u, "the anomaly is counted");
    bool itemised = false;
    for (const UnavailSpan& sp : a->spans) if (sp.anomalous) itemised = true;
    CHECK(itemised,
          "AND ITEMISED: it evicted an ordinary gap rather than being dropped, "
          "because the anomalous gaps ARE the covariate");
}

// ===========================================================================
// 11. Multi-session: N=6, ids stable and ascending, totals independent
// ===========================================================================
static void test_multi_session() {
    section("N=6 sessions, independent totals, stable order");
    UnavailabilityLedger& l = fresh();

    const char* ids[6] = {"s-morcar", "s-edwin", "s-king", "s-gyrth",
                          "s-leofwine", "s-waltheof"};
    for (int i = 0; i < 6; ++i) {
        l.note_session(ids[i], D0, W0);
        l.note_activity(ids[i], D0, W0);
    }

    // Only the king stalls — and only he is silent for longer than the gate.
    const int kBeats = 20;
    for (int b = 1; b <= kBeats; ++b) {
        const int64_t  day  = D0 + (int64_t)b * kFastDaysPerBeat;
        const uint64_t wall = W0 + (uint64_t)b * kBeatMs;
        l.note_day(day, wall);
        for (int i = 0; i < 6; ++i) if (i != 2) l.note_activity(ids[i], day, wall);
    }

    std::vector<UnavailSession> snap = l.snapshot();
    CHECK_EQ(snap.size(), 6u, "all six tracked");

    // std::map ordering => ascending ids, a stable wire order and a stable diff.
    bool ascending = true;
    for (size_t i = 1; i < snap.size(); ++i)
        ascending = ascending && (snap[i - 1].session_id < snap[i].session_id);
    CHECK(ascending, "ids ascending");

    for (int i = 0; i < 6; ++i) {
        const UnavailSession* u = find(snap, ids[i]);
        if (i == 2) {
            CHECK_EQ(u->unavailable_game_days, (int64_t)kBeats * kFastDaysPerBeat,
                     "the stalled king accrues the game-days his absence spanned");
            CHECK_EQ((int64_t)(u->unavailable_wall_seconds + 0.5), kBeats * 120,
                     "and the wall-seconds the gate judged");
            CHECK(u->over_threshold, "and is over X");
        } else {
            CHECK_EQ(u->unavailable_game_days, 0,
                     "the earls, on the heartbeat, accrue nothing");
            CHECK_EQ(u->gap_count, (size_t)kBeats, "though every gap is recorded");
            CHECK(!u->over_threshold, "and they are nowhere near X");
        }
    }
    CHECK(l.episode_invalid(), "ONE session over X invalidates the whole episode");
}

// ===========================================================================
// 12. The episode policy: mode, env, and the claim pin
// ===========================================================================
static void test_episode_policy() {
    section("episode policy: mode, thresholds, and the claim pin");

    unsetenv("CK3_EPISODE_MODE");
    unsetenv("CK3_EPISODE_ID");
    unsetenv("CK3_UNAVAIL_ANOMALY_WALL_S");
    unsetenv("CK3_UNAVAIL_X_DAYS");
    unsetenv("CK3_UNAVAIL_EXCLUSION_FRACTION");
    unsetenv("CK3_EPISODE_HORIZON_START");
    unsetenv("CK3_EPISODE_HORIZON_END");
    episode_policy().init_from_env();

    EpisodePolicy p = episode_policy().get();
    CHECK(p.mode == EpisodeMode::FreePlay, "FREE PLAY IS THE DEFAULT (G11 posture)");
    CHECK(!episode_is_experiment(), "so nothing is an experiment by accident");
    CHECK(!episode_pins_claims(),
          "and the claim pin is OFF — free play keeps multi-agent-control.md Q5 "
          "byte for byte, including orphan-to-native");
    CHECK_EQ(p.unavail_anomaly_wall_s, 360,
             "the RULED anomaly gate: 3 x the 120-second R3 heartbeat");
    // ck-2ngg: WITH NO DECLARED HORIZON THERE IS NO X. Not 365 — the bridge
    // refuses to judge rather than borrowing another horizon's answer.
    CHECK(!p.unavail_exclusion_armed,
          "no declared horizon => the exclusion is UNARMED");
    CHECK(p.unavail_exclusion_unarmed_reason == std::string(kUnarmedNoHorizon),
          "and typed: exclusion_unarmed_no_horizon");
    CHECK_EQ(p.unavail_exclusion_days, 0, "with no X standing behind it");
    CHECK(p.unavail_exclusion_fraction > 0.049 && p.unavail_exclusion_fraction < 0.051,
          "THE pre-registered constant on this axis is the FRACTION, 0.05");
    CHECK(std::string(episode_mode_name(p.mode)) == "free_play", "wire name");

    setenv("CK3_EPISODE_MODE", "experiment", 1);
    setenv("CK3_EPISODE_ID", "ep-20260912T000000Z-a-paper", 1);
    setenv("CK3_UNAVAIL_ANOMALY_WALL_S", "240", 1);
    setenv("CK3_EPISODE_HORIZON_START", kRegisteredHorizonStart, 1);
    setenv("CK3_EPISODE_HORIZON_END",   kRegisteredHorizonEnd,   1);
    episode_policy().init_from_env();
    p = episode_policy().get();
    CHECK(p.mode == EpisodeMode::Experiment, "experiment mode resolves");
    CHECK(episode_pins_claims(),
          "THE CLAIM PIN IS ON: a dropped socket must not release a claim and "
          "ai_ctrl+0x28 must not be re-enabled — the surviving half of R3");
    CHECK(p.episode_id == "ep-20260912T000000Z-a-paper", "episode id carried");
    CHECK(p.unavail_exclusion_armed, "a DECLARED horizon arms the exclusion");
    CHECK_EQ(p.horizon_game_days, 7300, "the registered horizon, in game-days");
    CHECK_EQ(p.unavail_exclusion_days, 365,
             "and X is the RULE's answer for it — 7300 x 0.05 — which is the "
             "number the paper cites, unmoved");
    CHECK(p.horizon_start_game_date == std::string(kRegisteredHorizonStart)
          && p.horizon_end_game_date == std::string(kRegisteredHorizonEnd),
          "the horizon it derived from is carried VERBATIM, so a bundle can be "
          "re-judged offline without re-running anything");
    CHECK(p.unavail_exclusion_unarmed_reason.empty(),
          "an armed policy carries no unarmed reason");
    CHECK_EQ(p.unavail_anomaly_wall_s, 240, "and the gate is overridable for a pilot");

    // THE SMOKE HORIZON, through the env the harness actually sets. This is the
    // configuration the constant got wrong by 4.7x.
    setenv("CK3_EPISODE_HORIZON_END", "1071.01.01", 1);
    episode_policy().init_from_env();
    p = episode_policy().get();
    CHECK_EQ(p.horizon_game_days, 1568, "the smoke horizon, in game-days");
    CHECK_EQ(p.unavail_exclusion_days, 78,
             "X FOLLOWS THE HORIZON: 78, not the 365 the constant applied to "
             "every episode regardless of how long it was");

    // THE FRACTION IS THE ONLY KNOB — and a junk one is REFUSED and PUBLISHED,
    // never silently applied.
    setenv("CK3_UNAVAIL_EXCLUSION_FRACTION", "0.10", 1);
    episode_policy().init_from_env();
    p = episode_policy().get();
    CHECK_EQ(p.unavail_exclusion_days, 157, "0.10 x 1568 = 156.8 -> 157");
    setenv("CK3_UNAVAIL_EXCLUSION_FRACTION", "5", 1);
    episode_policy().init_from_env();
    p = episode_policy().get();
    CHECK_EQ(p.unavail_exclusion_days, 78,
             "a '5' meant as '5%' is REFUSED; the published default stands "
             "rather than re-scaling X by 100x");
    CHECK(p.refused_exclusion_fraction_env == std::string("5"),
          "and the refusal is PUBLISHED, because a knob that is quietly ignored "
          "has told the operator nothing");
    unsetenv("CK3_UNAVAIL_EXCLUSION_FRACTION");

    // A HORIZON THAT IS NOT ONE is typed apart from an absent one.
    setenv("CK3_EPISODE_HORIZON_END", "1071.13.01", 1);
    episode_policy().init_from_env();
    p = episode_policy().get();
    CHECK(!p.unavail_exclusion_armed, "an unparseable horizon does not arm");
    CHECK(p.unavail_exclusion_unarmed_reason == std::string(kUnarmedBadHorizon),
          "typed exclusion_unarmed_bad_horizon, NOT 'no horizon' — somebody "
          "meant to declare one and got it wrong, and that is a different fix");
    CHECK(p.horizon_end_game_date == std::string("1071.13.01"),
          "and what they actually declared is carried verbatim, so the typo is "
          "visible in the bundle");

    // ── THE SECOND RETIRED KNOB (ck-2ngg) ─────────────────────────────────
    // CK3_UNAVAIL_X_DAYS was the door back to the coded constant: set to 365 on
    // a smoke horizon it restores the exact defect, silently and permissively.
    // It must reconfigure NOTHING — and must be PUBLISHED as ignored, because a
    // retired knob that quietly does nothing is a trap for the next operator.
    setenv("CK3_EPISODE_HORIZON_END", "1071.01.01", 1);
    setenv("CK3_UNAVAIL_X_DAYS", "365", 1);
    episode_policy().init_from_env();
    p = episode_policy().get();
    CHECK_EQ(p.unavail_exclusion_days, 78,
             "CK3_UNAVAIL_X_DAYS is RETIRED and cannot put 365 back");
    CHECK(p.ignored_x_days_env == std::string("365"),
          "and the attempt is published as IGNORED, verbatim");
    unsetenv("CK3_UNAVAIL_X_DAYS");
    setenv("CK3_EPISODE_HORIZON_END", kRegisteredHorizonEnd, 1);
    episode_policy().init_from_env();
    p = episode_policy().get();
    CHECK(std::string(episode_mode_name(p.mode)) == "experiment", "wire name");

    // A junk value degrades to the documented default, never to 0 — a 0-second
    // gate would make every gap anomalous, which is the failure mode the gate
    // exists to end (test 9).
    setenv("CK3_UNAVAIL_ANOMALY_WALL_S", "not-a-number", 1);
    episode_policy().init_from_env();
    CHECK_EQ(episode_policy().get().unavail_anomaly_wall_s, 360,
             "a junk gate falls back to the default, NOT to 0");

    // THE RETIRED KNOB IS DEAD. Setting it must change nothing at all — a
    // retired env var that still works is a mechanism that was not retired.
    setenv("CK3_UNAVAIL_MIN_SPAN_DAYS", "7", 1);
    setenv("CK3_UNAVAIL_ANOMALY_WALL_S", "360", 1);
    episode_policy().init_from_env();
    CHECK_EQ(episode_policy().get().unavail_anomaly_wall_s, 360,
             "CK3_UNAVAIL_MIN_SPAN_DAYS is RETIRED and reconfigures nothing");
    unsetenv("CK3_UNAVAIL_MIN_SPAN_DAYS");

    unsetenv("CK3_EPISODE_HORIZON_START");
    unsetenv("CK3_EPISODE_HORIZON_END");
    setenv("CK3_EPISODE_MODE", "free_play", 1);
    episode_policy().init_from_env();
    CHECK(!episode_pins_claims(), "anything but \"experiment\" is free play");

    unsetenv("CK3_EPISODE_MODE");
    unsetenv("CK3_EPISODE_ID");
    unsetenv("CK3_UNAVAIL_ANOMALY_WALL_S");
    unsetenv("CK3_UNAVAIL_X_DAYS");
    episode_policy().init_from_env();
}

// ===========================================================================
// 13. A whole synthetic episode — the shape the seal writes (§6.1)
// ===========================================================================
static void test_synthetic_episode_seal() {
    section("a synthetic 20-game-year episode seals with summable totals");
    UnavailabilityLedger& l = fresh();

    const char* ids[3] = {"s-king", "s-mercia", "s-northumbria"};
    for (int i = 0; i < 3; ++i) {
        l.note_session(ids[i], D0, W0);
        l.note_activity(ids[i], D0, W0);
    }

    // 20 game-years at 208 game-days per 120-second beat is ~35 beats.
    const int kBeats = 20 * 365 / (int)kFastDaysPerBeat;   // 35
    for (int b = 1; b <= kBeats; ++b) {
        const int64_t  day  = D0 + (int64_t)b * kFastDaysPerBeat;
        const uint64_t wall = W0 + (uint64_t)b * kBeatMs;
        l.note_day(day, wall);
        // A dark window [a, b) leaves a gap from beat a-1 to beat b: six beats
        // for Mercia (720 wall-seconds) and five for Northumbria (600).
        const bool mercia_dark = (b >= 10 && b < 15);
        const bool north_dark  = (b >= 20 && b < 24);
        l.note_activity(ids[0], day, wall);
        if (!mercia_dark) l.note_activity(ids[1], day, wall);
        if (!north_dark)  l.note_activity(ids[2], day, wall);
        else              l.note_attached(ids[2], false);
        if (b == 24) l.note_session(ids[2], day, wall);          // .5 resume
    }

    std::vector<UnavailSession> snap = l.snapshot();
    const UnavailSession* king   = find(snap, "s-king");
    const UnavailSession* mercia = find(snap, "s-mercia");
    const UnavailSession* north  = find(snap, "s-northumbria");

    CHECK_EQ(king->unavailable_game_days, 0, "the king was never dark");
    CHECK_EQ(king->gap_count, (size_t)kBeats, "but every one of his gaps is recorded");

    CHECK_EQ(mercia->span_count, 1u, "Mercia: one anomalous stall");
    CHECK_EQ(mercia->unavailable_game_days, 6 * kFastDaysPerBeat,
             "spanning the game-days its 720 wall-seconds of silence covered");
    const UnavailSpan* mspan = nullptr;
    for (const UnavailSpan& sp : mercia->spans) if (sp.anomalous) mspan = &sp;
    CHECK(mspan != nullptr, "and the anomalous span is itemised among the healthy ones");
    if (mspan) {
        CHECK_EQ(mspan->start_day, D0 + 9 * kFastDaysPerBeat, "anchored on its last input");
        CHECK_EQ(mspan->end_day, D0 + 15 * kFastDaysPerBeat, "and on the next one");
        CHECK_EQ((int64_t)(mspan->wall_seconds + 0.5), 720, "720 wall-seconds of it");
    }
    CHECK_EQ(north->span_count, 1u, "Northumbria: one anomalous stall");
    CHECK_EQ(north->unavailable_game_days, 5 * kFastDaysPerBeat, "of five beats");
    CHECK(north->attached, "and it is attached again at the horizon");

    // The ANOMALOUS spans a bundle writes must sum to the total it writes, or
    // run_summary.json's `unavailability_game_days` is not auditable (§6.1).
    for (const UnavailSession& u : snap) {
        int64_t sum = 0;
        for (const UnavailSpan& sp : u.spans) if (sp.anomalous) sum += sp.days;
        CHECK_EQ(sum, u.unavailable_game_days,
                 "the ANOMALOUS spans sum to the session total, so the bundle is "
                 "auditable");
    }

    // ── A FINDING, ASSERTED SO IT CANNOT BE FORGOTTEN ──────────────────────
    // At the E0 free-running rate the two ruled constants are in tension: the
    // 360-second gate spans ~623 game-days there, which is ALREADY past X=365.
    // So at that rate the FIRST anomalous gap invalidates the episode, and X
    // cannot discriminate a brief lapse from a dead agent. It is not a defect in
    // this code — both numbers behave exactly as ruled — and the numbers are the
    // pre-registration's to change after ck-zgh4 measures the ARM rate. It is
    // asserted here so the tension is visible in a test run rather than only in
    // a comment.
    CHECK(mercia->unavailable_game_days > 365,
          "FINDING: at 1.73 game-days/wall-second the smallest possible anomaly "
          "(just over 360 s) already spans ~623 game-days, so the FIRST anomaly "
          "crosses X=365 — the gate is coarser than X at this rate. Reportable "
          "to the pre-registration, not fixable here");
    CHECK(l.episode_invalid(),
          "which is why this synthetic episode reads INVALID on two brief lapses");

    // The same two lapses at the SLOW condition (~0.076 game-days/wall-second,
    // the rate the retired numbers were derived at) are a VALID episode: 600
    // wall-seconds spans ~46 game-days there. Same history, same gate, different
    // rate, opposite verdict — which is exactly why a threshold must never be
    // quoted without its rate and condition.
    UnavailabilityLedger& slow = fresh();
    slow.note_session("s-mercia", D0, W0);
    slow.note_activity("s-mercia", D0, W0);
    slow.note_day(D0 + 46, W0 + 600'000);
    slow.note_activity("s-mercia", D0 + 46, W0 + 600'000);
    const UnavailSession* sm = find(slow.snapshot(), "s-mercia");
    CHECK(sm->spans[0].anomalous, "600 wall-seconds is anomalous at ANY rate");
    CHECK_EQ(sm->unavailable_game_days, 46, "but it spans only 46 game-days here");
    CHECK(!slow.episode_invalid(), "so the episode is VALID at the slow condition");
}

// ===========================================================================
// 14. BOTH UNITS — every gap carries game-days AND wall-seconds, and an
//     UNSTAMPED gap is UNJUDGED rather than assumed
// ===========================================================================
static void test_both_units() {
    section("every gap is recorded in BOTH units; an unstamped gap is UNJUDGED");
    UnavailabilityLedger& l = fresh();

    l.note_session("english-1", D0, W0);
    l.note_activity("english-1", D0 + kFastDaysPerBeat, W0 + kBeatMs);          // healthy
    l.note_activity("english-1", D0 + 5 * kFastDaysPerBeat, W0 + 5 * kBeatMs);  // 480 s: anomalous

    const UnavailSession* u = find(l.snapshot(), "english-1");
    CHECK(u != nullptr, "the session is tracked");
    if (!u) return;
    CHECK(u->wall_measured, "the record is WALL-STAMPED");
    CHECK_EQ(u->gap_count, 2u, "two gaps, both recorded");
    CHECK_EQ(u->span_count, 1u, "one of them anomalous");
    CHECK_EQ(u->silent_game_days, 5 * kFastDaysPerBeat, "raw game-days: both gaps");
    CHECK_EQ((int64_t)(u->silent_wall_seconds + 0.5), 600, "raw wall-seconds: both gaps");
    CHECK_EQ(u->unavailable_game_days, 4 * kFastDaysPerBeat,
             "the covariate: the anomalous gap's game-days only");
    CHECK_EQ((int64_t)(u->unavailable_wall_seconds + 0.5), 480,
             "and the same gap's wall-seconds");
    CHECK_EQ((int64_t)u->spans.size(), 2, "both gaps are ITEMISED");
    for (const UnavailSpan& sp : u->spans) {
        CHECK(sp.wall_measured, "each gap says its wall duration is KNOWN");
        CHECK(sp.days > 0 && sp.wall_seconds > 0.0,
              "and carries BOTH units side by side — the pairing ck-ijfa found "
              "nothing could check");
    }
    CHECK(!u->spans[0].anomalous, "the 120-second gap is not anomalous");
    CHECK(u->spans[1].anomalous, "the 480-second one is");

    // NOT STAMPED IS NOT ZERO AND NOT ANOMALOUS. A caller that passes no wall
    // clock gets wall_measured=false, an UNJUDGED gap, and a visible count —
    // never a duration invented from the game-days at an assumed rate, which is
    // the exact move that produced the 26x error.
    UnavailabilityLedger& l2 = fresh();
    l2.note_session("unstamped", D0);
    l2.note_activity("unstamped", D0 + 5000);
    const UnavailSession* u2 = find(l2.snapshot(), "unstamped");
    CHECK(u2 != nullptr, "the unstamped session is tracked");
    if (!u2) return;
    CHECK_EQ(u2->silent_game_days, 5000, "its raw game-days are exact as always");
    CHECK(!u2->wall_measured, "but it is NOT wall-measured");
    CHECK_EQ(u2->unavailable_game_days, 0,
             "so the gap is NOT anomalous — unknown is not 'long', and the bridge "
             "never converts game-days at a borrowed rate to find out");
    CHECK_EQ(u2->unjudged_gap_count, 1u,
             "and the unjudged gap is COUNTED, so its silence is visible rather "
             "than an invisible zero");
    CHECK(!u2->spans.empty() && !u2->spans[0].wall_measured,
          "the span says the same");
    CHECK(!l2.episode_invalid(), "an unjudgeable record cannot invalidate an episode");
}

// ===========================================================================
// 15. X IS A RULE, NOT A CONSTANT (ck-2ngg)
// ===========================================================================
// THE DEFECT: `exclusion_days_ = 365` stored the rule's answer at ONE horizon
// and applied it to all of them. The two are indistinguishable at the
// registered horizon and nowhere else — ck-uq5i's shape exactly, and permissive
// everywhere it is wrong, which is why no episode ever showed a symptom.
static void test_exclusion_is_a_rule() {
    section("X = round(0.05 x declared horizon), in GAME-DAYS");

    // ── THE REGRESSION: the number the paper cites does not move ───────────
    const ExclusionRule reg = resolve_exclusion_rule(kRegisteredHorizonStart,
                                                     kRegisteredHorizonEnd);
    CHECK(reg.armed, "the registered horizon arms the exclusion");
    CHECK_EQ(reg.horizon_game_days, 7300,
             "1066.09.15 -> 1086.09.15 is TWENTY GAME-YEARS, 7300 game-days");
    CHECK_EQ(reg.exclusion_days, 365,
             "7300 x 0.05 = 365.0 EXACTLY — the rule reproduces the pre-"
             "registered number at the horizon it was derived at");
    CHECK_EQ(reg.exclusion_days, kExclusionDaysAtRegisteredHorizon,
             "and the UNITS-table anchor in episode_policy.h IS that answer, "
             "not a second copy of it that could drift");
    CHECK(reg.unarmed_reason.empty(), "an armed rule carries no unarmed reason");
    CHECK(reg.fraction > 0.049 && reg.fraction < 0.051,
          "the fraction it used is published beside the value");

    // ── THE SMOKE HORIZON: where the constant was 4.7x too permissive ──────
    const ExclusionRule smoke = resolve_exclusion_rule("1066.09.15", "1071.01.01");
    CHECK(smoke.armed, "the smoke horizon arms too");
    CHECK_EQ(smoke.horizon_game_days, 1568,
             "1066.09.15 -> 1071.01.01 is 1568 game-days on the ENGINE's "
             "calendar");
    CHECK_EQ(smoke.exclusion_days, 78, "1568 x 0.05 = 78.4, rounds to 78");
    CHECK(smoke.exclusion_days * 4 < kExclusionDaysAtRegisteredHorizon,
          "THE DEFECT, measured: the constant 365 is more than 4x the rule's "
          "answer here — and it is 23% of the whole horizon, so a smoke episode "
          "judged at 365 could not be excluded at all");

    // ── THE CALENDAR IS THE ENGINE'S, AND IT MATTERS BY EXACTLY 3 DAYS ─────
    // ck-2ngg's own spec quoted 1571 game-days and X = 79 for this horizon.
    // That is the UNIFORM 30-DAY-MONTH arithmetic of scripts/meter_lane.py's
    // `_ordinal()`, which that module documents as CADENCE ARITHMETIC ONLY and
    // never writes to a row. The ledger's day numbers come from the engine's
    // REAL month lengths (ck-05mg), so the span is 1568 and X is 78. Asserted
    // side by side so the discrepancy is a fact on the record rather than an
    // argument, and so re-introducing the uniform calendar fails here.
    const int64_t uniform_span = 1571;          // what 12x30-day months give
    const int64_t uniform_x    = 79;            // round(1571 x 0.05) = 78.55
    CHECK_EQ(uniform_span - smoke.horizon_game_days, 3,
             "the uniform-30-day cadence helper and the engine's calendar "
             "differ by 3 days on a 15-September start");
    CHECK_EQ(uniform_x - smoke.exclusion_days, 1,
             "which is one whole game-day of X — the engine's calendar is the "
             "one the ledger counts in, so 78 is the rule's answer");

    // ── THE OTHER 'REGISTERED HORIZON' IN THE REPO ────────────────────────
    // docs/e0-episode-runbook.md and docs/multi-instance.md offer
    // `--horizon 1086.1.1` as "the full paper horizon". It is 7043 game-days —
    // 257 short of twenty game-years — and the rule's answer there is 352, not
    // 365. THE RULE DID NOT CREATE THIS DISCREPANCY; IT CREATED THE ABILITY TO
    // SEE IT. Under the constant both spellings got 365, so no test, no reader
    // and no artefact could tell them apart — the ambiguity was invisible, not
    // absent. Which one the pre-registration means is its call, and the harness
    // declares it per episode either way.
    const ExclusionRule runbook = resolve_exclusion_rule("1066.09.15", "1086.01.01");
    CHECK_EQ(runbook.horizon_game_days, 7043, "1066.09.15 -> 1086.01.01");
    CHECK_EQ(runbook.exclusion_days, 352, "7043 x 0.05 = 352.15, rounds to 352");
    CHECK(runbook.exclusion_days != reg.exclusion_days,
          "the repo's two spellings of 'the registered horizon' give DIFFERENT "
          "X — a difference the constant hid completely");

    // ── ROUNDING, STATED ──────────────────────────────────────────────────
    // Round half away from zero on the product. The span is non-negative, so
    // that is round-half-up. 78.4 -> 78 and 78.5 -> 79 are one rule apart and
    // the difference is a threshold, so it is pinned rather than described.
    CHECK_EQ(resolve_exclusion_rule("1000.01.01", "1000.01.11").exclusion_days, 1,
             "10 days x 0.05 = 0.5 rounds AWAY FROM ZERO to 1");
    CHECK_EQ(resolve_exclusion_rule("1000.01.01", "1000.01.10").exclusion_days, 0,
             "9 days x 0.05 = 0.45 rounds to 0 — an armed X of 0 is a real "
             "threshold (any anomaly invalidates), not an unarmed episode");
    CHECK(resolve_exclusion_rule("1000.01.01", "1000.01.10").armed,
          "and it is ARMED, which is why `armed` is a separate question from "
          "`exclusion_days != 0`");

    // ── THE FRACTION IS THE ONLY KNOB, AND IT IS GUARDED ──────────────────
    CHECK_EQ(resolve_exclusion_rule(kRegisteredHorizonStart, kRegisteredHorizonEnd,
                                    0.10).exclusion_days, 730,
             "a pilot fraction of 0.10 doubles X at the same horizon");
    CHECK_EQ(resolve_exclusion_rule(kRegisteredHorizonStart, kRegisteredHorizonEnd,
                                    5.0).exclusion_days, 365,
             "a '5' meant as '5%' is REFUSED and the published default stands, "
             "rather than silently re-scaling X by 100x");
    CHECK(!exclusion_fraction_is_usable(0.0) && !exclusion_fraction_is_usable(-0.05)
          && !exclusion_fraction_is_usable(1.5),
          "the usable range is (0, 1]");

    // ── NO HORIZON => UNARMED, TYPED, NEVER 365 ───────────────────────────
    const ExclusionRule none = resolve_exclusion_rule("", "");
    CHECK(!none.armed, "no declared horizon means NO X");
    CHECK(none.unarmed_reason == std::string(kUnarmedNoHorizon),
          "and it says so in the closed vocabulary: exclusion_unarmed_no_horizon");
    CHECK_EQ(none.exclusion_days, 0,
             "it does NOT fall back to 365 — falling back to another horizon's "
             "answer is the defect itself");
    CHECK(!resolve_exclusion_rule("1066.09.15", "").armed,
          "half a horizon is not a horizon");

    // A DECLARED horizon that is not one is a DIFFERENT repair from an absent
    // one, so it gets its own name rather than being folded into 'no horizon'.
    const ExclusionRule bad = resolve_exclusion_rule("1066.09.15", "not-a-date");
    CHECK(!bad.armed, "an unparseable endpoint does not arm");
    CHECK(bad.unarmed_reason == std::string(kUnarmedBadHorizon),
          "typed exclusion_unarmed_bad_horizon — somebody MEANT to declare one");
    CHECK(!resolve_exclusion_rule("1086.09.15", "1066.09.15").armed,
          "a horizon that runs backwards is refused, not negated");
    CHECK(!resolve_exclusion_rule("1066.09.15", "1066.09.15").armed,
          "and a zero-length one is not a horizon either");
    CHECK(!is_game_date("1066.02.30"),
          "1066.02.30 is a date CK3 cannot produce, and is refused rather than "
          "silently becoming 2 March");
    CHECK(!is_game_date("1066.13.01"),
          "month 13 — the ck-05mg rendering bug's own artefact — is refused");
    CHECK(is_game_date("1066.9.15") && is_game_date("1066.09.15"),
          "both the padded and unpadded spellings the harness may emit parse");
}

// ===========================================================================
// 16. AN UNARMED LEDGER JUDGES NOTHING — and says so
// ===========================================================================
// THE NEGATIVE-CONTROL SHAPE ck-2ngg ASKED FOR, run inline against the SAME
// session history: the pre-fix constant passes an episode the rule excludes.
static void test_unarmed_ledger_and_the_prefix_control() {
    section("no declared horizon => UNARMED: nothing is judged, and that is "
            "visible");

    UnavailabilityLedger& l = fresh(360, 0, /*armed*/false);
    CHECK(!l.exclusion_armed(), "the ledger knows it has no X");
    CHECK_EQ(l.exclusion_days(), 0, "and reports 0, which is NOT a threshold");

    l.note_session("s1", D0, W0);
    l.note_activity("s1", D0, W0);
    l.note_day(D0 + 5000, W0 + 3'000'000);            // 50 wall-minutes silent
    l.note_activity("s1", D0 + 5000, W0 + 3'000'000);
    const UnavailSession* u = find(l.snapshot(), "s1");
    CHECK(u != nullptr, "the session is tracked exactly as before");
    if (!u) return;
    CHECK_EQ(u->unavailable_game_days, 5000,
             "THE COVARIATE IS STILL MEASURED — arming decides judgement, never "
             "measurement, so an episode with no declared horizon still carries "
             "everything an analysis needs to judge it offline");
    CHECK(u->spans[0].anomalous, "and the gap is still typed anomalous");
    CHECK(!u->over_threshold,
          "but NOTHING is over threshold, because there is no threshold");
    CHECK(!l.episode_invalid(),
          "so the episode is not flagged — and a consumer that reads only this "
          "false would call an UNJUDGED episode a clean one, which is why "
          "exclusion_armed() is a required companion question");

    // ── DECLARED IS NOT OBSERVED, AND THE BUNDLE SAYS BOTH ────────────────
    // Raised by ck-7pw9/ck-zgh4 while this bead was in flight. X is 5% of the
    // DECLARED horizon, as ruled — so an episode that declares twenty
    // game-years and stops after 5000 days has a tolerance proportional to an
    // observation it did not make. The ruling stands; what must not stand is
    // that being invisible. The ledger reports what it actually SAW, beside the
    // declared horizon, and never feeds it back into the threshold.
    CHECK_EQ(l.observed_game_days(), 5000,
             "the ledger reports the span it ACTUALLY observed");
    CHECK_EQ(l.first_day(), D0, "from the first game day that reached it");
    CHECK(l.observed_game_days() !=
              resolve_exclusion_rule(kRegisteredHorizonStart,
                                     kRegisteredHorizonEnd).horizon_game_days,
          "which is a DIFFERENT number from the declared horizon — and with "
          "both in the bundle, 'declared 20 years, ran 1' is legible as a "
          "purchase instead of indistinguishable from an honest long run");

    // ── THE PRE-FIX CONTROL, on one input ─────────────────────────────────
    // A smoke episode (horizon 1568 game-days) with one session unsteered for
    // 120 game-days. The RULE's X there is 78: excluded. The pre-fix CONSTANT
    // was 365: passed. Same history, same gate, opposite verdicts — that single
    // case discriminates the two mechanisms completely, which is what test 9
    // did for ck-ijfa's gate change.
    const int64_t rule_x     = resolve_exclusion_rule("1066.09.15",
                                                      "1071.01.01").exclusion_days;
    const int64_t prefix_x   = kExclusionDaysAtRegisteredHorizon;   // the old 365
    const int64_t unsteered  = 120;

    UnavailabilityLedger& lr = fresh(360, rule_x, /*armed*/true);
    lr.note_session("earl", D0, W0);
    lr.note_activity("earl", D0, W0);
    lr.note_day(D0 + unsteered, W0 + 400'000);        // 400 s > the 360 s gate
    lr.note_activity("earl", D0 + unsteered, W0 + 400'000);
    CHECK_EQ(rule_x, 78, "the rule's X on the smoke horizon");
    CHECK(unsteered > rule_x, "120 unsteered game-days is past it");
    CHECK(lr.episode_invalid(),
          "so under the RULE this episode is EXCLUDED");

    UnavailabilityLedger& lc = fresh(360, prefix_x, /*armed*/true);
    lc.note_session("earl", D0, W0);
    lc.note_activity("earl", D0, W0);
    lc.note_day(D0 + unsteered, W0 + 400'000);
    lc.note_activity("earl", D0 + unsteered, W0 + 400'000);
    CHECK(unsteered < prefix_x, "and short of the pre-fix constant 365");
    CHECK(!lc.episode_invalid(),
          "PRE-FIX CONTROL: the constant PASSES the same episode the rule "
          "excludes — one history, two verdicts, and the difference is entirely "
          "whether X was computed for this episode's own horizon");
}

// ===========================================================================
// ck-48lf — THE EXACT EXTREMA, AND THE OPEN GAP IS ONE OF THEM
// ===========================================================================
// `spans` is capped at kMaxRetainedSpans, so a reader cannot take a maximum
// from it. The scalars exist for that, and the case this row exists for is the
// one a source-text assertion cannot reach: a session that WENT QUIET AND NEVER
// CAME BACK has its longest gap only in the OPEN gap, which is computed at
// snapshot time and never enters `spans` accounting. Removing the fold in
// render_locked leaves every closed-gap figure correct and loses exactly this.
static void test_exact_extrema_include_the_open_gap() {
    section("ck-48lf: the extrema are over EVERY gap, including the open one");
    UnavailabilityLedger& l = fresh();

    l.note_session("s-open", D0, W0);
    int64_t  day  = D0;
    uint64_t wall = W0;
    // Three short, closed gaps first, so there IS a closed maximum to beat.
    for (int i = 0; i < 3; ++i) {
        day  += 2;
        wall += 4000;                       // 4 s apiece
        l.note_day(day, wall);
        l.note_activity("s-open", day, wall);
    }
    {
        const UnavailSession* u = find(l.snapshot(), "s-open");
        CHECK(u != nullptr, "the session is tracked");
        CHECK_EQ((int64_t)(u->max_span_wall_seconds + 0.5), 4,
                 "the closed maximum is the longest CLOSED gap, exactly");
        CHECK_EQ(u->max_span_game_days, 2,
                 "and in game-days too");
    }

    // Now it goes quiet and never returns: advance the world only.
    day  += 500;
    wall += 900000;                         // 900 s, far past the 360 s gate
    l.note_day(day, wall);

    const UnavailSession* u = find(l.snapshot(), "s-open");
    CHECK(u != nullptr, "still tracked after going silent");
    CHECK_EQ((int64_t)(u->max_span_wall_seconds + 0.5), 900,
             "THE OPEN GAP IS THE MAXIMUM: a session that never came back has "
             "its longest gap nowhere else, and this is the figure a reader "
             "must not have to reconstruct from a capped list");
    CHECK_EQ(u->max_span_game_days, 500,
             "in game-days as well");

    // AND IT IS A MAXIMUM, NOT A SUM — the failure mode next door.
    CHECK(u->max_span_wall_seconds < u->silent_wall_seconds,
          "a maximum is strictly less than the total of several gaps, so a "
          "`+=` where a max belongs cannot pass this row");
    CHECK_EQ(u->max_span_game_days < u->silent_game_days, true,
             "same in game-days");

    // The retained list still holds the short ones, which is the whole point:
    // the scalar and the itemisation disagree BY DESIGN and the scalar wins.
    double from_list = 0.0;
    for (const UnavailSpan& s : u->spans)
        if (s.wall_measured && s.wall_seconds > from_list) from_list = s.wall_seconds;
    CHECK(u->max_span_wall_seconds >= from_list,
          "the scalar is never below what the retained list can show");
}

int main() {
    printf("=== unavailability accounting: the WALL-CLOCK anomaly gate (ck-ijfa) "
           "over §2.7's GAME-DAY covariate (ck-6kvq.10) ===\n");
    test_healthy_heartbeat_accrues_nothing();
    test_ten_minute_silence_is_one_anomaly();
    test_observer_traffic_never_counts();
    test_detach_resume_sums();
    test_phantom_session_keeps_accruing();
    test_exclusion_threshold();
    test_time_hygiene();
    test_silent_from_connect();
    test_negative_control_superseded_mechanism();
    test_span_retention_is_bounded();
    test_multi_session();
    test_episode_policy();
    test_synthetic_episode_seal();
    test_both_units();
    test_exclusion_is_a_rule();
    test_unarmed_ledger_and_the_prefix_control();
    test_exact_extrema_include_the_open_gap();

    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) printf("PASS\n");
    else                 printf("FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
