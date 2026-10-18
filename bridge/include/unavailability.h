// unavailability.h — R3 as REVERSED: the clock runs; unavailability is measured
// (ck-6kvq.10).
//
// Design of record: docs/design/event-pipeline-conductor.md §2.7 (rewritten
// 2026-09-12), §2.10, §6.1. Pre-registration §7.1/§7.3.
//
// WHAT THIS REPLACES. R3 was originally "a session-cap stall means the window
// HOLDS and the world waits". That was built as a checkpoint clock gate
// (decision G3', overseer GO 2026-09-10). On 2026-09-12 the progenitor REVERSED
// G3': in experiment mode THE CLOCK RUNS, and what a capped agent costs the
// experiment is MEASURED instead of prevented:
//
//   * per control session, per episode, record the game-date spans in which the
//     session posted no action and no read — its UNAVAILABILITY, in game-days;
//   * the claim PERSISTS through the gap (that part of R3 survives — the arm
//     never changes mid-episode; see episode_policy.h's claim pin);
//   * an episode in which any English session exceeds X cumulative game-days
//     is INVALID — archived and reported, never silently dropped.
//
// THE ANOMALY GATE (PROGENITOR RULING, 2026-09-13, via bt-bbd — ck-ijfa).
// A gap between a control session's activities counts as UNAVAILABILITY only if
// it exceeds THREE HEARTBEAT INTERVALS = 360 WALL-SECONDS, measured from
// wall-clock stamps on the activity records. Under three beats is a slow turn;
// over three beats is an absence. `unavailable_game_days` is then the game-days
// SPANNED BY ANOMALOUS GAPS ONLY, and more than X of them invalidates the
// episode.
//
// X IS A RULE, NOT A CONSTANT (ck-2ngg). "5% of the horizon unsteered" is the
// threshold; 365 is that rule EVALUATED AT THE REGISTERED 20-game-year horizon
// (7300 game-days x 0.05 = 365.0) and is wrong at every other one. This ledger
// used to store `exclusion_days_ = 365` outright, so a smoke episode whose
// whole horizon is 1568 game-days was judged against a number 4.7x larger than
// its own rule — permissive, therefore symptomless. X is now COMPUTED per
// episode from the harness's DECLARED horizon and handed to configure() along
// with whether it is ARMED at all:
//
//   ARMED     judge normally: more than exclusion_days() invalidates.
//   UNARMED   no horizon was declared, so there is NO X. Nothing is flagged
//             over threshold, episode_invalid() is false, and
//             exclusion_armed() is false so the seal can mark
//             `unavailability_over_x` NOT EVALUABLE. A consumer that tests
//             only `episode_invalid()` would read an unjudged episode as a
//             passed one, which is why the armed bit is a separate, REQUIRED
//             question rather than a zero in the threshold.
//
// See exclusion_rule.h for the rule and episode_policy.h for its constants.
//
// WHAT THIS SUPERSEDES, and why, because the failure is worth keeping.
// ck-6kvq.10 gated in GAME-DAYS: a gap of at least `min_span_days` (30) counted
// IN FULL. That number was derived under the GATED design — 30 game-days was one
// agent turn because the world WAITED for the agent — and it crossed the
// same-day G3' reversal with its justification intact. Once the clock
// free-runs, the game-days a gap spans are a function of GAME SPEED and have
// nothing to do with whether the agent was late. ck-ijfa's arithmetic:
//
//   at the node's free-running ~1.73 game-days/wall-second, one HEALTHY
//   2-minute heartbeat interval spans ~204 game-days. 204 >= 30, so under the
//   old rule EVERY inter-turn gap was "unavailability" and was counted whole —
//   and because the gaps partition the timeline, `unavailable_game_days` did
//   not merely correlate with elapsed time, it EQUALLED it. X=365 then meant
//   "exclude every episode after one game-year", 5% into a 20-year run, on an
//   agent doing exactly what R3 prescribes. The heartbeat that exists to
//   PREVENT recorded unavailability was what accumulated it.
//
// So `min_span_days` is RETIRED. It is not re-derived, because no game-day
// number can express "the agent was late" while the clock rate is free.
//
// THE GATE IS IN WALL-CLOCK; THE COVARIATE IS IN GAME-DAYS. That split is the
// point and it is deliberate:
//   * WALL-CLOCK decides whether a gap is anomalous, because lateness is a
//     property of the AGENT and the agent runs on a wall clock;
//   * GAME-DAYS measure what the anomaly COST the experiment, because what a
//     missing agent costs is world-time its character went unsteered.
// Anything that mixes the two — a game-day threshold on an agent's punctuality,
// a wall-clock threshold on experimental cost — is the class of error this bead
// was filed for, and it was invisible because only one unit was ever recorded.
//
// EVERY GAP IS RECORDED IN BOTH UNITS, always: game-days AND wall-seconds, per
// gap, plus the RAW totals (`silent_game_days`, `silent_wall_seconds` — every
// forward gap, anomalous or not). An analysis that wants a different gate can
// re-derive one from the recorded gaps without re-running an episode. That is
// the property whose absence let a 26x error live in a comment block.
//
// AN UNSTAMPED GAP IS NOT JUDGED. `wall_measured == false` means no wall stamp
// reached the ledger for that gap's endpoints, so its duration is UNKNOWN: it is
// NOT anomalous (the bridge never invents a duration by converting game-days at
// some assumed rate — that move is the original defect), it still contributes to
// the raw silent totals, and it is COUNTED in `unjudged_gap_count` so its
// silence is visible rather than silent.
//
// TIME IS A GAME DAY NUMBER, never a formatted date. The bridge's own
// `(raw_date - DATE_EPOCH)/24` is monotone and exact; "YYYY.MM.DD" is a display
// string (and was wrong until ck-05mg). Wall stamps are MONOTONIC milliseconds
// from the same clock the conductor already reads.
//
// PURE. No engine dependencies; host-tested with no CK3 binary
// (bridge/test/test_unavailability.cpp).

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace bridge {

// One recorded gap. `end_day - start_day == days` always; an OPEN span is the
// one still accruing at read time and is closed at the ledger's current day.
//
// THE CONVENTION: a span is the ELAPSED game-days BETWEEN consecutive inputs —
// last input on day 399, next on day 580 is a span of 181, not the 180 days on
// which the session happened to say nothing. That is the number of game-days the
// agent was out of the loop, which is what §2.7's covariate is denominated in,
// and it is the only reading under which an open span and the closed span it
// becomes are the same number. Asserted in test_unavailability.cpp so it cannot
// silently flip and shift every episode's covariate by one per span.
struct UnavailSpan {
    int64_t start_day = 0;   // the day of the last activity before the silence
    int64_t end_day   = 0;   // the day activity resumed, or the current day
    int64_t days      = 0;
    bool    open      = false;

    // THE SAME GAP IN WALL-CLOCK (ck-ijfa). `wall_measured == false` means the
    // endpoints carried no stamp, so the duration is UNKNOWN and the gap is
    // UNJUDGED — never a game-day figure converted at an assumed rate.
    bool     wall_measured = false;
    uint64_t start_wall_ms = 0;
    uint64_t end_wall_ms   = 0;
    double   wall_seconds  = 0.0;

    // THE VERDICT: wall_seconds > anomaly_wall_seconds. Only an anomalous gap
    // contributes to `unavailable_game_days`, `span_count` and X.
    bool     anomalous     = false;
};

// One control session's record. Kept after the session ends: a session that is
// gone keeps accruing, because a phantom agent is exactly what the covariate is
// supposed to make visible (§2.7 knock-on to ck-6kvq.5 — a refused resume must
// inflate the covariate, not quietly vanish).
struct UnavailSession {
    std::string session_id;
    bool     attached  = true;   // socket currently connected
    bool     ended     = false;  // registry record gone for good
    int64_t  first_day = 0;      // day the session was first seen
    int64_t  last_activity_day = 0;
    uint64_t activity_count    = 0;

    // THE RAW TOTALS — every forward gap, anomalous or not, in both units.
    // Kept so an analysis can re-derive a different gate without re-running.
    int64_t  silent_game_days    = 0;
    double   silent_wall_seconds = 0.0;
    size_t   gap_count           = 0;   // every gap, exact

    // THE COVARIATE — ANOMALOUS GAPS ONLY (gaps whose WALL duration exceeds
    // anomaly_wall_seconds). `unavailable_game_days` is the GAME-DAYS those
    // gaps spanned, and it is what X is tested against — X being the RULE's
    // answer for THIS episode's declared horizon (ck-2ngg), not a constant.
    int64_t  unavailable_game_days    = 0;
    double   unavailable_wall_seconds = 0.0;
    size_t   span_count               = 0;   // anomalous gaps, exact

    // A gap whose wall duration is unknown cannot be judged and is NOT counted
    // as unavailability. Counted here so that is visible in the bundle rather
    // than being an invisible zero.
    size_t   unjudged_gap_count = 0;

    std::vector<UnavailSpan> spans;      // the gaps, newest last, CAPPED
    bool     spans_truncated       = false;

    // ck-48lf: THE EXTREMA, OVER EVERY GAP, because `spans` is capped and a
    // reader cannot get a maximum out of a truncated list. Measured on d50:
    // `spans_truncated` was true on all six seats with 128 retained of 418-853
    // gaps, and the figure that travelled -- "max gap <=66 s on all six" --
    // was the maximum over the RETAINED spans. The same episode's longest gap
    // between agent tool calls was 110-120 s. A scalar cannot be truncated, so
    // nothing has to be reconstructed from a list that announces itself
    // incomplete.
    //
    // Maintained on the same closed-gap path as the exact totals beside them,
    // and the OPEN gap is folded in at snapshot time exactly as the totals are,
    // so it can neither be missed nor double-counted.
    double   max_span_wall_seconds = 0.0;   // every gap, exact; 0 if none measured
    int64_t  max_span_game_days    = 0;     // every gap, exact
    // > X: this session invalidates. ALWAYS FALSE WHEN THE EXCLUSION IS
    // UNARMED — there is no X to be over — so read it beside the ledger's
    // exclusion_armed(), never alone.
    bool     over_threshold        = false;

    bool     wall_measured         = false;  // any stamp ever reached this record
    uint64_t first_wall_ms         = 0;
    uint64_t last_activity_wall_ms = 0;
};

// At most this many spans are RETAINED per session. The totals and the counts
// stay exact when it bites; only the itemisation is lost.
//
// AN ANOMALOUS GAP ALWAYS GETS IN (ck-ijfa). When the list is full, an anomalous
// gap EVICTS the oldest non-anomalous one, because the anomalous gaps ARE the
// covariate and a chatty session must not be able to crowd its own evidence out
// of the record with ordinary turn-taking.
constexpr size_t kMaxRetainedSpans = 128;

// THE PRE-REGISTERED ANOMALY GATE, in wall-seconds: three intervals of the R3
// 120-second heartbeat. Overridable for a pilot via CK3_UNAVAIL_ANOMALY_WALL_S
// and PUBLISHED in telemetry, so an episode's bundle always says which number
// it was measured under.
constexpr int64_t kDefaultAnomalyWallSeconds = 360;

class UnavailabilityLedger {
public:
    // Thresholds (from EpisodePolicy; see episode_policy.h for the reasoning).
    // `anomaly_wall_s` is the WALL-CLOCK gate; `exclusion_days` is X, in
    // GAME-DAYS. Two units on purpose — see the header note above.
    //
    // `exclusion_armed` HAS NO DEFAULT, deliberately (ck-2ngg): every call site
    // must state whether this episode has an X at all. A defaulted `true` is
    // how the constant got in, and a defaulted `false` would silently disarm a
    // real episode.
    void    configure(int64_t anomaly_wall_s, int64_t exclusion_days,
                      bool exclusion_armed);
    int64_t anomaly_wall_seconds() const;
    int64_t exclusion_days() const;

    // Is there an X for this episode? FALSE means no horizon was declared, so
    // the criterion was NOT EVALUATED — which is not the same as passing it.
    bool    exclusion_armed() const;

    // A CONTROL session appeared (handshake, or a resume that rebinds an id).
    // `day` seeds its clock: a session that connects and never speaks is
    // unavailable from the moment it connected, which is the honest reading.
    // Calling this again for a known id is an ATTACH, not a reset — .5's resume
    // must not erase the gap it just finished accruing.
    // `wall_ms` is a MONOTONIC wall stamp (ck-ijfa); 0 means "not stamped".
    void note_session(const std::string& session_id, int64_t day,
                      uint64_t wall_ms = 0);

    // Socket came or went. Detaching does NOT stop accrual and does NOT end the
    // record: under the claim pin a detached session still owns its character.
    void note_attached(const std::string& session_id, bool attached);

    // The session is gone for good (free-play destroy, or a refused resume).
    // The record is KEPT and KEEPS ACCRUING — see UnavailSession.
    void note_session_ended(const std::string& session_id);

    // ck-j6lk: FORGET the record entirely — a SEPARATE VERB, not a flag on the one
    // above, and the distinction is the whole bead.
    //
    // note_session_ended keeps the record and keeps accruing, which is correct and
    // documented: a seat that is gone for good was still a seat, and its silence is
    // the covariate. But shakedown 3 on deploy-32 showed the bridge destroying both
    // driver helpers — its log reads "UNDECLARED CLAIMLESS DETACH — destroying, not
    // pinning" twice — while unavailability_game_days STILL reported both, each
    // `ended: true`, each with one open span of 364 days and `over_threshold: true`.
    // So unavailability_over_x fired again. The registry forgot them; the ledger did
    // not, because ending a record is not the same as never having had one.
    //
    // A non-participant must be ABSENT from the report, not present and ended: it was
    // never a seat, so it has no silence to account for. Called ONLY on the state-a
    // branch (see episode_participant.h). Every other path keeps note_session_ended
    // and the contract above is unchanged, which is why this is a new call rather
    // than a parameter — a parameter would have put the two meanings on one line and
    // the next reader would have had to know which callers passed what.
    void forget_session(const std::string& session_id);

    // ANY submission OR read from a control session (§2.7). Observer traffic
    // never reaches here: unknown ids are ignored, and only control sessions are
    // ever registered.
    void note_activity(const std::string& session_id, int64_t day,
                       uint64_t wall_ms = 0);

    // The world's clock moved. Monotone by construction — a lower day (a reload)
    // is ignored and counted, never applied, because letting the current day go
    // backwards would silently shrink every open span.
    void note_day(int64_t day, uint64_t wall_ms = 0);

    int64_t  current_day() const;
    uint64_t day_regressions() const;
    bool     has_day() const;

    // ── DECLARED vs OBSERVED (ck-2ngg, raised by ck-7pw9/ck-zgh4) ──────────
    // X is 5% of the DECLARED horizon, as ruled. That makes the tolerance a
    // property of the episode's CONFIGURATION, and an episode that declares
    // twenty game-years and stops after one has bought a budget proportional to
    // an observation it never made. The ruling is the ruling — but the bundle
    // must make the purchase LEGIBLE rather than leave "declared 20, ran 20"
    // indistinguishable from "declared 20, ran 1".
    //
    // So the ledger also reports what it ACTUALLY SAW: the first game day that
    // reached it, and the span to the current one. Published beside the
    // declared horizon and the computed X, never instead of either, and never
    // fed back into the threshold — the three numbers together are what turns
    // "the tolerance was 5%" into an auditable claim instead of a trusted one.
    int64_t  first_day() const;
    int64_t  observed_game_days() const;

    // Every tracked session, ids ascending (stable output for a wire contract
    // and for a diff), with the open span closed at the current day.
    std::vector<UnavailSession> snapshot() const;

    // Is any session over X? The bridge FLAGS; the analysis excludes.
    //
    // FALSE WHEN UNARMED, and a consumer must read `exclusion_armed()` beside
    // it: an unjudged episode and a judged-and-clean one give the same answer
    // here, and only the armed bit tells them apart.
    bool episode_invalid() const;

    // Test/reset seam.
    void reset();

private:
    struct Rec {
        bool     attached  = true;
        bool     ended     = false;
        int64_t  first_day = 0;
        int64_t  last_activity_day = 0;
        uint64_t activity_count    = 0;

        // CLOSED gaps only; the OPEN one is computed at snapshot time so it can
        // never be double-counted.
        int64_t  silent_closed       = 0;
        double   silent_wall_closed  = 0.0;
        size_t   gap_count_closed    = 0;
        int64_t  unavail_closed      = 0;
        double   unavail_wall_closed = 0.0;
        size_t   span_count_closed   = 0;
        size_t   unjudged_closed     = 0;
        // ck-48lf: extrema over CLOSED gaps; the open one joins at render.
        double   max_gap_wall_closed = 0.0;
        int64_t  max_gap_days_closed = 0;

        std::vector<UnavailSpan> spans;
        bool     spans_truncated   = false;

        bool     wall_stamped          = false;
        uint64_t first_wall_ms         = 0;
        uint64_t last_activity_wall_ms = 0;
    };

    // ck-ijfa: advance the wall cursor. Called with mu_ HELD.
    void           note_wall_locked(uint64_t wall_ms);
    UnavailSession render_locked(const std::string& id, const Rec& r) const;
    void           push_span_locked(Rec& r, const UnavailSpan& s) const;
    // wall_seconds > anomaly_wall_seconds_. An UNMEASURED gap is never
    // anomalous: unknown is not "long".
    bool           is_anomalous_locked(bool wall_measured, double wall_seconds) const;

    mutable std::mutex          mu_;
    std::map<std::string, Rec>  recs_;
    int64_t                     current_day_     = 0;
    int64_t                     first_day_       = 0;   // ck-2ngg: what was OBSERVED
    bool                        have_day_        = false;
    uint64_t                    current_wall_ms_ = 0;   // ck-ijfa
    bool                        have_wall_       = false;
    uint64_t                    day_regressions_ = 0;
    int64_t                     anomaly_wall_s_  = kDefaultAnomalyWallSeconds;
    // ck-2ngg: NO DEFAULT THRESHOLD. An un-configured ledger has no X — it is
    // UNARMED — because the only honest default for "5% of a horizon nobody
    // declared" is "I cannot say". The 365 that used to sit here was the rule's
    // answer at one horizon, applied to all of them.
    int64_t                     exclusion_days_  = 0;
    bool                        exclusion_armed_ = false;
};

UnavailabilityLedger& unavailability();

}  // namespace bridge
