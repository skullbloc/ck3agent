// pipeline_mode.h — R8: what `null` MEANS (ck-6kvq.4).
//
// Design of record: docs/design/event-pipeline-conductor.md §6.3, with §2.1
// (the flush loop), §2.7 (the checkpoint gate) and §8 (migration, G11).
//
// WHY THIS FILE EXISTS
// ====================
// `PipelineMode` had three values and TWO predicates, both written as
// comparisons at their use sites:
//
//     pipeline_active()     := mode != Legacy      // so NULL was "active"
//     pipeline_owns_clock() := mode == Pipeline    // so NULL was not
//
// Every other behaviour — does this mode buffer calls, run the conductor,
// admit a control session, arm the checkpoint gate — was then decided by
// whichever of those two predicates happened to be in scope. That is how
// `null` ended up meaning "pipeline with clock ownership switched off", which
// is not what §6.3 describes and is not an E0 arm:
//
//   * it BUFFERED every PAUSED / TICK call and ran the conductor, so the flush
//     plan's Pause / Unpause / SetSpeed steps drove the clock — in the one mode
//     whose definition is "the clock free-runs";
//   * it admitted CONTROL sessions, so "no control sessions" was a convention
//     nothing enforced;
//   * the checkpoint gate armed, and with no control session present the gate's
//     own `no_control_sessions` branch held the world at checkpoint 1 FOREVER.
//     An E0 episode run with `CK3_EPISODE_MODE=experiment` — which is exactly
//     how R11 wants it run — would have hung on its first checkpoint.
//
// So the policy is lifted out of the comparisons and named, one function per
// behaviour. A mode is now a ROW of answers, and adding a fourth mode means
// answering every column rather than silently inheriting `!= Legacy`.
//
// PURITY. No engine, no globals, no <atomic>, no getenv. Every function here is
// a total function of its argument, which is what makes the policy host-
// testable with no CK3 binary: bridge/test/test_pipeline_mode.cpp.

#pragma once

namespace bridge {

// ============================================================================
// The three modes (G11)
// ============================================================================
enum class PipelineMode {
    Legacy,    // byte-for-byte pre-pipeline behaviour. THE DEFAULT.
    Pipeline,  // the buffered, polarity-driven flush loop; bridge owns the clock.
    Null,      // E0: no control sessions, no flush loop, the clock free-runs.
};

// "legacy" | "pipeline" | "null" — these strings ARE the wire contract
// (run_summary.json `pipeline_mode`, get_tick_info `pipeline.mode`). Note that
// the last one is the four-character STRING "null", never a JSON null (§6.1).
const char* pipeline_mode_name(PipelineMode m);

// Parse `CK3_PIPELINE_MODE`. An absent, empty or UNRECOGNISED value resolves to
// `Legacy` — G11's safe default — and sets `*recognised` to false so the caller
// can say so out loud instead of starting a silently different episode.
PipelineMode parse_pipeline_mode(const char* s, bool* recognised);

// ============================================================================
// THE POLICY TABLE
// ============================================================================
// One column per behaviour. The whole table, so it can be read at a glance:
//
//   behaviour                        legacy   pipeline   null
//   ------------------------------   ------   --------   ----
//   buffers_calls                    no       YES        no
//   runs_flush_loop                  no       YES        no
//   owns_clock                       no       YES        no
//   admits_control_sessions          YES      YES        no
//   admits_observer_clock_verbs      no       no         YES
//
// ck-1ek9 REMOVED A SIXTH COLUMN, `may_arm_checkpoint_gate`. The original
// table (425c20b) carried it, and it was the column the WIP author called the
// one that "would have bitten hardest". It is gone because THE SUBSYSTEM IT
// GOVERNS IS GONE: the checkpoint clock gate was withdrawn from the experiment
// path on 2026-09-12 when the progenitor reversed G3' (episode_policy.h:14-18).
// A policy predicate with no consumer is exactly the disease this bead exists
// to cure — a rule that is stated, compiles, reads as enforcement, and is
// enforced on nothing. If the gate ever returns, the column returns with it.
//
// (`pipeline{...}` telemetry is emitted in EVERY mode, unconditionally, at
// query_handlers.cpp:7330 — a client must be able to learn which mode it is
// talking to without a second call — so it is not a policy column.)
//
// ⚠ TWO OF THESE COLUMNS HAVE NO CALLER YET (ck-1ek9). `mode_buffers_calls`
// and `mode_runs_flush_loop` answer correctly and NOTHING ASKS THEM:
// pipeline.cpp still computes `pipeline_active()` as `mode != Legacy` and
// `pipeline_start()` still starts the conductor for `Null`. ck-1ek9 wired the
// control-session column and the clock carve-out — the P0, the one that made
// two guards unfireable — and stopped there, because changing how `null`
// SERVES CALLS changes the runtime characteristics of an E0 set that is
// mid-flight. Follow-up: ck-1ek9.1. Said here as well as in the runbook,
// because this file is where someone will come to find out what `null` means,
// and a table that overstates itself is what this bead was about.
//
// `null` is NOT "pipeline minus clock ownership". It is closer to `legacy`
// minus control sessions: calls are served by the legacy inline ladder exactly
// as they are today, so an observer's reads answer immediately and the ops
// probes (self-heal, overlay poller) see the world they have always seen.

// Calls are accepted, buffered and answered with a ticket (§2.4). False here
// means the legacy inline/queue ladder serves the envelope unchanged.
//
// NULL = false, and that is load-bearing twice over. The conductor is what
// drains the buffer, so buffering without a conductor is an unbounded client
// hang; and the drain is what emits the plan's clock steps, so not buffering is
// what makes "the clock free-runs" true rather than aspirational.
bool mode_buffers_calls(PipelineMode m);

// The bridge-owned conductor timer thread runs (§2.3).
bool mode_runs_flush_loop(PipelineMode m);

// G1: `pause_game` / `unpause_game` / `set_speed` from a session are refused
// `clock_owned_by_pipeline`. In `null` the clock is nobody's but the engine's:
// the harness sets a speed once at episode start and the world free-runs.
bool mode_owns_clock(PipelineMode m);

// A handshake may mint a CONTROL session. FALSE in `null`, which is the
// enforcement behind E0's "no control sessions, no steering, zero tokens".
//
// A refused control request is DOWNGRADED to observe, never rejected. Rejecting
// would break every read-only tool that still handshakes as v1 (and is
// force-upgraded to CONTROL at ipc_server.cpp:413-419) — the self-heal probe,
// the overlay poller, the harness — and a dark ops ladder during an unattended
// episode is a worse failure than a downgraded session. Downgrading instead
// routes the refusal through the mode enforcement that ALREADY exists and is
// already comprehensive (dispatch_router.cpp:1764 / session.cpp:216 / :909 all
// refuse a mutation with `reason:"observe_mode"`), rather than adding a second,
// parallel gate that could disagree with the first.
bool mode_admits_control_sessions(PipelineMode m);

// The typed reason recorded when a control handshake is downgraded. A constant
// so the log line, the handshake ack and the test all quote the same token.
// THE ONE THING A NULL-MODE OBSERVER MAY STILL DO: set the clock (ck-1ek9).
//
// `admits_control_sessions` is false for null, and `set_speed` is a MUTATION
// (session.cpp `action_is_readonly` does not list it), so the downgrade alone
// makes the episode's own declared game speed unsettable BY ANYONE — and
// `pin_game_speed` refuses the episode when it cannot pin. Landing the
// downgrade without this column refuses every E0 episode at open.
//
// THIS IS NOT A HOLE IN THE BAN, AND THAT DISTINCTION IS THE WHOLE DESIGN.
// The alternative considered was to carve an exemption into the handshake so a
// self-described "clock setter" still gets CONTROL. That was rejected: the
// exemption would key on something the CLIENT declares, so the ban would be
// opt-out-able by any client that names itself correctly — which is the same
// class of defect as the one this bead repairs (a requirement checked, never
// enforced). Here NOTHING gets a control session in null mode, ever, and the
// permission granted instead is for ONE VERB FAMILY that steers nothing.
//
// Precedent, in the same gate: `declare_save_identity` is already exempt from
// the mutation check as harness provenance (dispatch_router.cpp), because
// "gating it on control would mean the identity could only ever be vouched for
// by a session that is itself part of the experimental condition". The game
// speed is the same kind of fact: a declared CONSTANT OF the episode, not a
// decision INSIDE it. §4.1's outcomes cannot move because the world was
// simulated at speed 2 rather than 3; they can move because somebody steered.
//
// FALSE for legacy and pipeline, so this cannot leak into an arm: legacy HAS
// control sessions and the harness must use one, and pipeline OWNS the clock
// (`mode_owns_clock`) and refuses the verb from any session at all.
bool mode_admits_observer_clock_verbs(PipelineMode m);

const char* pipeline_mode_control_downgrade_reason();

}  // namespace bridge
