// episode_policy.h — the episode's MODE, and nothing else (ck-6kvq.10).
//
// Design of record: docs/design/event-pipeline-conductor.md §2.7, §6.1.
//
// WHY THIS FILE EXISTS AT ALL. Two unrelated behaviours need to know "is this a
// pre-registered experiment episode, or free play?":
//
//   * §2.9's pulse-leak CONTAINMENT (abort the episode, refuse further
//     control-session work) — experiment mode only;
//   * §2.7's CLAIM PIN (a dropped socket must not release a claim and must not
//     re-enable `ai_ctrl+0x28`) — experiment mode only, free-play keeps
//     docs/design/multi-agent-control.md Q5 byte for byte.
//
// Until 2026-09-12 both read the answer off `CheckpointGate::policy().mode`,
// because the checkpoint clock gate happened to own the env parsing. The
// progenitor REVERSED G3' that day and the gate was withdrawn from the
// experiment path (§2.7); the mode outlived it, so it lives here now, in a
// module small enough that nothing has to be withdrawn from it next time.
//
// PURE. No engine dependencies, host-tested with no CK3 binary.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "registered_speed.h"   // ck-on4u: THE declaration of the pinned speed

namespace bridge {

// §2.7 "experiment mode only". Free-play is the default, and a free-play bridge
// must behave exactly as the shipping one does.
enum class EpisodeMode { FreePlay, Experiment };

// "free_play" | "experiment" — these strings are published on get_tick_info.
const char* episode_mode_name(EpisodeMode m);

// ═══════════════════════════════════════════════════════════════════════════
// THE EXCLUSION RULE'S CONSTANTS (ck-2ngg). ONE home each — exclusion_rule.h
// consumes them, and the acceptance reads them out of this file.
// ═══════════════════════════════════════════════════════════════════════════

// THE pre-registered constant on the unavailability axis, and now the only one:
// X is this FRACTION of the episode's declared horizon. Published beside every
// value it produced; overridable for a pilot via CK3_UNAVAIL_EXCLUSION_FRACTION.
constexpr double kDefaultExclusionFraction = 0.05;

// THE REGISTERED HORIZON, spelled once: the paper's twenty game-years.
// 1066.09.15 -> 1086.09.15 is 7300 game-days EXACTLY on the engine's calendar.
//
// NOT 1086.01.01, which docs/e0-episode-runbook.md and docs/multi-instance.md
// both offer as "the full paper horizon": that is 7043 game-days, 19.3
// game-years, and the rule's answer there is 352, not 365. Both spellings are
// in live use — 1086.09.15 in scripts/test_episode_abort.sh and
// scripts/acceptance_speed_validity.py, 1086.1.1 in the two docs.
//
// READ THE 352 CORRECTLY, because the obvious reading is backwards: THE RULE
// DID NOT CREATE THIS DISCREPANCY, IT CREATED THE ABILITY TO SEE ONE THAT WAS
// ALWAYS THERE. Under the old constant both spellings produced 365, so no test,
// no reader and no artefact could tell them apart — the ambiguity was perfectly
// invisible rather than absent. Which one the pre-registration means is the
// pre-registration's call, and the harness DECLARES it per episode either way.
constexpr const char* kRegisteredHorizonStart = "1066.09.15";
constexpr const char* kRegisteredHorizonEnd   = "1086.09.15";

// X at that horizon: 7300 x 0.05 = 365.0 exactly.
//
// NOT a policy knob and never read at runtime. It is a named ANCHOR so that the
// UNITS table below has a C++ symbol to be audited against
// (scripts/acceptance_threshold_units.py), and so that a change to the rule
// which MOVES the number the paper cites fails a test instead of passing one.
// bridge/test/test_unavailability.cpp asserts it equals what
// resolve_exclusion_rule() actually computes for that horizon, so it cannot
// drift into a lie the way the runtime constant it replaces did.
constexpr int64_t kExclusionDaysAtRegisteredHorizon = 365;

// THE CLOSED VOCABULARY of unarmed reasons. Two members, not one: a typo and an
// absence are repaired by different people. Neither ever means "X is 365".
constexpr const char* kUnarmedNoHorizon  = "exclusion_unarmed_no_horizon";
constexpr const char* kUnarmedBadHorizon = "exclusion_unarmed_bad_horizon";

struct EpisodePolicy {
    EpisodeMode mode = EpisodeMode::FreePlay;

    // Minted by the harness (bead .2), handed to the bridge at episode start and
    // stamped onto every record (§6.1). "" until .2 lands; never omitted from
    // the wire, so the lanes coding against the shape keep a stable key set.
    std::string episode_id;

    // THE EXPERIMENTAL ARM (ck-6kvq.12 / R4, decision G5). Lower-cased, one of
    // "e0" | "a" | "b" | "c", or "" when the episode does not declare one (free
    // play, and every pre-R4 harness). It is carried HERE, in the episode's own
    // config, rather than in the board, because the arm is a property of the
    // episode that the board merely reads: the arm decides the board, never the
    // other way round.
    //
    // The board exists in arm "b" and in no other arm (G5 option A2: ABSENT,
    // not present-and-refused). An unrecognised value is kept verbatim and is
    // not "b", so a typo produces an arm-A-shaped bridge — the conservative
    // direction, since a missing channel is a visible null result while a
    // channel that should not be there silently contaminates the manipulation.
    std::string arm;

    // ═══════════════════════════════════════════════════════════════════════
    // THE DIMENSIONAL TABLE — every threshold, in BOTH units, at a NAMED RATE
    // in a NAMED CONDITION. scripts/acceptance_threshold_units.py READS THESE
    // ROWS and fails the build of this contract if a threshold appears without
    // a rate and a condition, if a row's arithmetic does not close, or if a row
    // disagrees with the C++ default below it. Edit a number HERE and in the
    // constant, and nowhere else.
    //
    //   columns: name | value | unit | condition | rate_gd_per_wall_s |
    //            wall_seconds | game_days | heartbeats_120s
    //
    // UNITS: anomaly_gate | 360 | wall_seconds | e0_null_free_running | 1.73 | 360 | 622.8 | 3
    // UNITS: anomaly_gate | 360 | wall_seconds | speed3_metered_measured | 1.584 | 360 | 570.2 | 3
    // UNITS: anomaly_gate | 360 | wall_seconds | slow_worked_example | 0.076 | 360 | 27.36 | 3
    // UNITS: exclusion_x | 365 | game_days | e0_null_free_running | 1.73 | 211.0 | 365 | 1.758
    // UNITS: exclusion_x | 365 | game_days | speed3_metered_measured | 1.584 | 230.4 | 365 | 1.920
    // UNITS: exclusion_x | 365 | game_days | slow_worked_example | 0.076 | 4802.6 | 365 | 40.02
    // UNITS-END
    //
    // THE exclusion_x ROWS ARE THE REGISTERED HORIZON'S (ck-2ngg). X is not a
    // constant — it is 5% of THIS episode's DECLARED horizon, and 365 is that
    // rule's answer at 1066.09.15 -> 1086.09.15 (7300 game-days) and nowhere
    // else. The rows are anchored to `kExclusionDaysAtRegisteredHorizon`, which
    // a test pins to what the rule actually computes. For an episode on any
    // other horizon, restate ITS OWN X from ITS OWN bundle with
    // scripts/threshold_units.py — never by reading a row here.
    //
    // READ THE CONDITIONS AS WHAT THEY ARE, AND NOTE WHAT IS MISSING.
    //   e0_null_free_running      MEASURED. E0 null mode, no control sessions,
    //                             clock free-running (Appendix A.19; corroborated
    //                             live by ck-zgh4). GAME SPEED UNRECORDED — which
    //                             is itself part of the defect.
    //   speed3_metered_measured   MEASURED by ck-zgh4 on the node during a live
    //                             metered episode at CK3 SPEED 3: 1.584
    //                             game-days/wall-second, 15.6 game-years/wall-hour,
    //                             and days-per-tick = 1.000 EXACTLY — confirming
    //                             that what speed changes is TICKS PER WALL-SECOND,
    //                             not game-days per tick.
    //   slow_worked_example       NOT a measurement. The ~0.75 game-years/hour
    //                             the RETIRED game-day numbers were worked at,
    //                             under the WITHDRAWN checkpoint gate.
    //
    // NONE OF THEM IS THE ARM CONDITION. The progenitor has ruled CK3 SPEED 2
    // for every arm AND for E0; no rate has been measured at speed 2, so the
    // row that will one day govern these thresholds — `arm_speed2_measured` —
    // is deliberately ABSENT rather than filled in from the speed-3 figure or
    // from a free-running one. Add it when ck-zgh4 measures the pair, and cite
    // THAT row in the gate's derivation.
    //
    // AND READ THE FIRST TWO ROWS AGAINST THE LAST TWO. At the E0 null rate the
    // 360-second gate spans ~623 game-days, which is already past X = 365: the
    // gate is COARSER than X there, so the FIRST anomalous gap would invalidate
    // an episode and X could not tell a brief lapse from a dead agent. At the
    // slow condition the gate spans ~27 game-days and X is ~13 anomalies away.
    // Same two numbers, opposite regimes, decided entirely by a rate nobody has
    // measured in an arm. That tension is REAL, it is asserted in
    // bridge/test/test_unavailability.cpp so a test run shows it, and resolving
    // it is the pre-registration's call after ck-zgh4 measures the arm rate —
    // not an edit anyone should make here.
    //
    // ═══════════════════════════════════════════════════════════════════════
    // §2.7's UNAVAILABILITY COVARIATE — the pre-registration numbers, carried
    // here so one `init_from_env` owns every episode-shaped knob.
    // ═══════════════════════════════════════════════════════════════════════
    //
    // EVERY THRESHOLD BELOW IS STATED IN BOTH UNITS, at a NAMED RATE in a NAMED
    // CONDITION, and scripts/acceptance_threshold_units.py FAILS this file if
    // one is not. That rule exists because of what it caught (ck-ijfa):
    //
    //   this block used to derive its numbers at ~0.75 game-years/hour and
    //   justify them by citing "the measured ~19 game-years/hour" — which is
    //   real, but it is APPENDIX A.19's E0 NULL-MODE figure (63 wall-minutes
    //   for 20 game-years, no control sessions, clock free-running), not a
    //   measurement of the condition the thresholds govern. The two rates are
    //   26x apart. Both halves were sound; only their PAIRING was wrong, and
    //   nothing in either half announced it, because a threshold with no
    //   dimensional check is a number that can be off by 26x while every test
    //   stays green.
    //
    // THE MEASURED RATES ON RECORD, each with the condition it was measured in.
    // Neither is an arm rate. Do not quote one without its condition:
    //
    //   E0 NULL, free-running (Appendix A.19, corroborated by ck-zgh4's live E0
    //   set 1066.9.15 -> 1071.1.1): ~19 game-years/wall-hour ~= 1.73 game-days
    //   per wall-second. NO control sessions. GAME SPEED UNRECORDED.
    //
    //   SPEED 3, metered, with a control session attached (ck-zgh4, node,
    //   2026-09-12): 1.584 game-days/wall-second = 15.6 game-years/wall-hour,
    //   days-per-tick 1.000 exactly.
    //
    //   ARM AT THE RULED SPEED 2 (A/B/C, six control sessions on the R3
    //   heartbeat, pipeline mode): NOT MEASURED. The bridge now measures it —
    //   get_tick_info
    //   `pipeline.unavailability.clock_rate`, carried into run_summary.json as
    //   `arm_rate_game_days_per_wall_second` (ck-6kvq.14). `measured:false`
    //   means NOT MEASURED and must be reported as such, never filled in from
    //   the E0 figure.
    //
    // AND THE ROOT NOBODY HAD PINNED: the episode's GAME SPEED was an
    // UNSPECIFIED PRE-REGISTRATION PARAMETER, and every wall-clock figure here
    // is a function of it. That is how two honest rates could disagree by 26x —
    // they are two speeds. The progenitor has since RULED SPEED 2 for every arm
    // and for E0, which closes the parameter but does not supply the number: no
    // rate has been measured at speed 2 yet. The speed is published beside every
    // measured rate (`clock_rate.condition.speed_min/speed_max`) for exactly
    // this reason, and a rate measured across a speed CHANGE reports a range so
    // it cannot be quoted as one speed's.
    //
    // ── THE ANOMALY GATE (PROGENITOR RULING, 2026-09-13, via bt-bbd) ────────
    //
    // A gap between a control session's activities is UNAVAILABILITY only if it
    // exceeds THREE HEARTBEAT INTERVALS, measured in WALL-CLOCK from stamps on
    // the activity records. Derivation, and it needs no rate because it is
    // already in the agent's own unit: the R3 heartbeat is 120 wall-seconds;
    // under three beats is a slow turn, over three beats is an absence.
    //
    //   THRESHOLD             360 wall-seconds = 6 wall-minutes = 3 heartbeats
    //   CONDITION             any — this threshold is denominated in the AGENT's
    //                         clock, so it is invariant to game speed and to the
    //                         clock rate. That invariance is the reason the gate
    //                         moved into wall-clock.
    //   IN GAME-DAYS          NOT FIXED, and deliberately so. At the E0 null
    //                         rate (1.73 gd/s) 360 s spans ~623 game-days; at
    //                         the ~0.75 game-years/hour worked example (0.076
    //                         gd/s) it spans ~27. The span is RECORDED per gap
    //                         at the run's own measured rate, never assumed.
    //
    // WHAT THIS SUPERSEDED, kept because the failure is the lesson.
    // ck-6kvq.10 gated in GAME-DAYS — `min_span_days = 30`, a gap of at least
    // 30 game-days counted IN FULL — and derived that 30 as "one agent turn at
    // the cadence §2.7 itself used (~6.5 wall-minutes)". That was TRUE UNDER THE
    // GATED DESIGN, where the world WAITED for each agent, so 30 game-days
    // really was one turn. The progenitor reversed G3' the same day: the clock
    // free-runs. The number crossed the reversal with its justification intact,
    // and after the reversal the game-days a gap spans are a function of GAME
    // SPEED and say nothing about whether the agent was late:
    //
    //   at 1.73 gd/s one HEALTHY 120-second heartbeat interval spans ~208
    //   game-days. 208 >= 30, so EVERY inter-turn gap was "unavailability" and
    //   was counted whole — and since the gaps partition the timeline,
    //   `unavailable_game_days` did not correlate with elapsed time, it EQUALLED
    //   it. X=365 then meant "exclude every episode after one game-year", 5%
    //   into a 20-year run, on an agent doing exactly what R3 prescribes. The
    //   heartbeat that exists to PREVENT recorded unavailability was what
    //   accumulated it.
    //
    // So `min_span_days` is RETIRED, not re-derived: no game-day number can
    // express "the agent was late" while the clock rate is free.
    //
    // Overridable for a pilot via CK3_UNAVAIL_ANOMALY_WALL_S and PUBLISHED in
    // telemetry, so an episode's bundle always says which number it ran under.
    int64_t unavail_anomaly_wall_s = 360;

    // ── X, THE EXCLUSION THRESHOLD — A RULE, NOT A CONSTANT (ck-2ngg) ───────
    //
    // RULED 2026-09-12, RESTATED 2026-09-13 (the progenitor, via bt-bbd): more
    // than X cumulative GAME-DAYS spanned by ANOMALOUS gaps on any English
    // session invalidates the episode, and
    //
    //     X = 5% OF THE EPISODE'S OWN DECLARED HORIZON, UNSTEERED.
    //
    // That sentence is the threshold. 365 is not: it is that sentence EVALUATED
    // AT THE REGISTERED HORIZON — 1066.09.15 -> 1086.09.15, twenty game-years,
    // 7300 game-days, x 0.05 = 365.0 exactly — and it is wrong at every other
    // horizon. The smoke horizon (1066.09.15 -> 1071.01.01) is 1568 game-days,
    // where the rule says 78 and the constant said 365: 4.7x too PERMISSIVE,
    // which is why nothing ever showed a symptom. A smoke episode judged at 365
    // cannot be excluded at all — its whole horizon is 1568 days, so a session
    // would have to sit unsteered for 23% of the run before the constant
    // noticed. Same shape as ck-uq5i's degeneracy ceiling: a ruled MEANING and
    // a coded CONSTANT that agree in exactly one configuration.
    //
    // THE ARITHMETIC AND ITS CALENDAR. The span is computed through
    // ck3::game_date (ck-05mg's REAL month lengths), because that is the
    // calendar the ledger's day numbers come from. It is NOT the uniform
    // 30-day-month `_ordinal()` in scripts/meter_lane.py, which that module
    // documents as CADENCE ARITHMETIC ONLY and never writes to a row. On a
    // horizon starting 15 September the two disagree by 3 game-days (smoke:
    // 1568 vs 1571; X: 78 vs 79), and picking the wrong one is the same class
    // of units error ck-ijfa was filed for. Rounding is round-half-away-from-
    // zero on the product; the span is non-negative, so that is round-half-up.
    //
    //   THE CONSTANT          the FRACTION, 0.05 — the one pre-registered
    //                         number on this axis. Overridable for a pilot via
    //                         CK3_UNAVAIL_EXCLUSION_FRACTION and PUBLISHED
    //                         beside every value it produced.
    //   THE INPUT             the DECLARED horizon, from the harness at episode
    //                         start (CK3_EPISODE_HORIZON_START / _END). The
    //                         bridge never guesses one and never substitutes a
    //                         default.
    //   NO HORIZON            UNARMED, typed `exclusion_unarmed_no_horizon`.
    //                         The bridge refuses to judge rather than falling
    //                         back to 365 — falling back IS the defect — and
    //                         the seal marks `unavailability_over_x` NOT
    //                         EVALUABLE. An unevaluated criterion must never
    //                         read as a passed one.
    //   IN WALL-CLOCK         RATE-DEPENDENT, and a FLOOR rather than an
    //                         identity, because only gaps over 360 s count. The
    //                         UNITS rows above restate the REGISTERED-horizon
    //                         value in each condition on record; for any other
    //                         horizon, restate the episode's OWN X from its
    //                         bundle with scripts/threshold_units.py rather
    //                         than retyping a row.
    //
    // STILL OPEN, and it must not be presented as settled: no rate has been
    // measured at the RULED CK3 speed 2, so every wall-clock reading of X is a
    // worked example in a condition that is NOT the experiment's. ck-zgh4's
    // speed table is the measurement. Until it lands, quote X in GAME-DAYS.
    //
    // CK3_UNAVAIL_X_DAYS IS RETIRED (ck-2ngg). A direct override of X is a door
    // back to the coded constant this bead closed: set to 365 on a smoke
    // horizon it restores the exact defect, silently and permissively. The
    // fraction is the knob. Setting the retired variable changes nothing and is
    // PUBLISHED as ignored, because a retired knob that silently does nothing
    // is a trap for the next operator.

    // THE FRACTION. See exclusion_rule.h for the rule that consumes it.
    double  unavail_exclusion_fraction = kDefaultExclusionFraction;

    // THE DECLARED HORIZON, verbatim as the harness declared it ("" = not
    // declared). Published so a bundle says which horizon its X came from.
    std::string horizon_start_game_date;
    std::string horizon_end_game_date;
    bool        horizon_declared  = false;
    int64_t     horizon_game_days = 0;

    // THE RULE'S ANSWER for THIS episode. `unavail_exclusion_armed` is what a
    // consumer must test — never `unavail_exclusion_days != 0`, which reads an
    // unarmed episode as one with a threshold of zero.
    bool        unavail_exclusion_armed = false;
    std::string unavail_exclusion_unarmed_reason = kUnarmedNoHorizon;
    int64_t     unavail_exclusion_days = 0;

    // The retired CK3_UNAVAIL_X_DAYS, verbatim, when it was set and IGNORED.
    // "" otherwise. Published, never applied.
    std::string ignored_x_days_env;

    // A CK3_UNAVAIL_EXCLUSION_FRACTION that was set and REFUSED (not a finite
    // number in (0,1] — a "5" meant as "5%" would re-scale X by 100x). "" when
    // none was set or it was accepted. Published for the same reason: a knob
    // that is quietly ignored has told the operator nothing.
    std::string refused_exclusion_fraction_env;

    // ═══════════════════════════════════════════════════════════════════════
    // §9.4's REGISTERED GAME SPEED (ck-on4u). Declared by the harness through
    // CK3_EPISODE_GAME_SPEED; see registered_speed.h for why it is declared at
    // all rather than hard-coded on each side of the language boundary.
    //
    // THE BRIDGE'S ONLY CONSUMER IS THE FLUSH PLANNER'S RESTING STATE. The
    // plan's restore step re-asserts this speed after any flush that touched
    // the clock, which is what makes an episode free-run at a FIXED speed; with
    // the declaration in hand it re-asserts THE PIN instead of a hard-coded 3.
    //
    // `game_speed_declared` is PUBLISHED beside the number and is not
    // decoration: "declared 2" and "defaulted to 2" are the same value and a
    // different fact, and the second one is an episode whose launch path forgot
    // to establish the condition — which is exactly the failure a bundle must
    // be able to show after the fact.
    int         game_speed          = kRegisteredGameSpeed;
    bool        game_speed_declared = false;

    // The raw text of a CK3_EPISODE_GAME_SPEED that was set and is NOT a speed
    // in 1..5. "" when unset or accepted. Published, never applied — same rule
    // as the refused fraction above.
    //
    // ck-aqio: READ IT BESIDE `game_speed_present`, NEVER ON ITS OWN. Under the
    // byte rule the EMPTY STRING is a refusal too — set to nothing is a step
    // that ran and computed nothing — so `!refused_game_speed_env.empty()` is no
    // longer "was it refused?": it silently skips the one refusal whose raw text
    // is empty, which is the case a reader most needs told about.
    std::string refused_game_speed_env;

    // Was the variable SET AT ALL? false = the establishing step never ran or
    // never reached this process; true with an empty refusal = it was set to
    // nothing. Two operator actions that sealed one record before ck-aqio.
    bool        game_speed_present  = false;
};

// Process-wide policy. Written once at bridge init, read from three threads.
class EpisodePolicyHolder {
public:
    void          set(const EpisodePolicy& p);
    EpisodePolicy get() const;

    // THE EPISODE ID, LEARNED AT RUN TIME (ck-kc32). CK3_EPISODE_ID is a
    // LAUNCH-TIME variable and the driver mints its id at BUNDLE OPEN, after the
    // game is already running — so the env var structurally cannot carry it, and
    // a gate comparing `pipeline.episode_id` to the driver's id could never pass.
    // declare_save_identity is where the bridge first learns the real id; this is
    // where it keeps it.
    //
    // LAST DECLARATION WINS, deliberately. The field answers "which episode does
    // the bridge believe it is in", so a second episode declaring itself must
    // replace the first — a stale id that outlived its episode would let the gate
    // pass for the WRONG run, which is worse than never passing.
    void set_episode_id(const std::string& id);

    // Resolve from the environment: CK3_EPISODE_MODE ("experiment" | anything
    // else), CK3_EPISODE_ID, CK3_EPISODE_ARM, CK3_UNAVAIL_ANOMALY_WALL_S,
    // CK3_UNAVAIL_EXCLUSION_FRACTION, and the DECLARED horizon
    // CK3_EPISODE_HORIZON_START / CK3_EPISODE_HORIZON_END, from which X is
    // COMPUTED (ck-2ngg). No horizon => the exclusion is UNARMED and typed,
    // never 365.
    //
    // ck-on4u: and CK3_EPISODE_GAME_SPEED, §9.4's registered game speed, which
    // the flush planner rests at. Unset => the pre-registered default, with
    // `game_speed_declared` false so the bundle can tell the two apart.
    //
    // TWO RETIRED VARIABLES, both published as ignored rather than silently
    // dropped: CK3_UNAVAIL_MIN_SPAN_DAYS (the game-day gate — ck-ijfa) and
    // CK3_UNAVAIL_X_DAYS (a direct override of X — ck-2ngg; it is the door back
    // to the coded constant, so the fraction is the knob instead).
    void init_from_env();

    // LOCK-FREE on purpose. This is read on the IPC reader thread for EVERY
    // envelope (it is what keeps the §2.7 accounting off free play's hot path
    // entirely), and ck-6kvq.9's acceptance is that legacy mode stays
    // indistinguishable from the pre-change bridge in COST as well as in
    // behaviour. A mutex per envelope is not that.
    bool is_experiment() const;

private:
    mutable std::mutex  mu_;
    EpisodePolicy       p_;
    std::atomic<bool>   experiment_{false};   // mirror of p_.mode, lock-free
};

EpisodePolicyHolder& episode_policy();

// Convenience readers — the two questions the rest of the bridge actually asks.
bool episode_is_experiment();

// THE CLAIM PIN (§2.7). True in experiment mode. While it is true:
//   - a dropped socket must NOT destroy its session or release its claims
//     (ipc_server.cpp release_slot);
//   - `ai_ctrl+0x28` must NOT be re-enabled for a claimed character
//     (dispatch_router.cpp do_release_character).
// A claimed character whose session is unavailable keeps its last steer and is
// never handed back to native AI — the surviving half of R3, "the arm never
// changes mid-episode". Free-play is not an episode, so Q5 is untouched.
bool episode_pins_claims();

}  // namespace bridge
