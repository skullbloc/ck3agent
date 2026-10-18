// pipeline.h — R1b: the wired conductor (ck-6kvq.9).
//
// Design of record: docs/design/event-pipeline-conductor.md §2.2, §2.3, §2.4,
// §2.8, §2.10, §8; overseer decisions G1, G2, G11 (RULED GO 2026-09-10).
//
// This is the ENGINE-FACING half of the pipeline. The two host-testable halves
// are flush_scheduler.{h,cpp} (cut a plan) and flush_apply.{h,cpp} +
// ticket_table.{h,cpp} (apply a plan, drive tickets). This file owns:
//
//   1. pipeline_mode      — legacy | pipeline | null, DEFAULT legacy (G11).
//   2. classify_polarity  — THE polarity authority (§2.2). The MCP gateway's
//                           table (mcp_server.py:129-178) is demoted to a
//                           timeout hint that must AGREE with this one.
//   3. the conductor      — a BRIDGE-OWNED TIMER THREAD that drives the flush.
//                           Not the game thread: the game thread does not run
//                           while paused, and a flush must be able to END a
//                           pause.
//   4. clock ownership    — G1: pause_game / unpause_game / set_speed from a
//                           session are typed refusals in pipeline mode.
//   5. pipeline telemetry — published on get_tick_info and nowhere else
//                           (§2.10), so the self-heal ladder, overlay poller,
//                           harness and agent all learn it for free.
//
// LEGACY IS THE DEFAULT AND MUST BE INDISTINGUISHABLE. Every entry point below
// is a single relaxed atomic load away from a no-op when the mode is `legacy`.
// That is the acceptance gate for this bead, not an aspiration.

#pragma once

#include <cstdint>
#include <string>

#include "flush_scheduler.h"
#include "pipeline_mode.h"
// ck-m3n5: Session::Mode is a PARAMETER of the conferral function below,
// and a nested enum is not reachable through the forward declaration this
// header used to rely on. session.h includes only world_identity.h and the
// standard library, so there is no cycle.
#include "session.h"

namespace bridge {

class Session;
class ResponseBuilder;

// ============================================================================
// Mode (G11) — the ENUM and the POLICY now live in pipeline_mode.h (ck-6kvq.4).
// ============================================================================
// This header keeps only the process-wide accessor. `PipelineMode`,
// `pipeline_mode_name`, `parse_pipeline_mode` and the four `mode_*` policy
// predicates are pure and host-tested next door; everything below reads the
// one atomic that says which mode this process is in.

PipelineMode pipeline_mode();

// True when the flush loop is driving. `legacy` => false, and every hook in
// this header short-circuits.
bool pipeline_active();

// G1: in pipeline mode the bridge owns the clock, so pause_game / unpause_game
// / set_speed from a session are refused. Unchanged in legacy mode.
bool pipeline_owns_clock();
bool is_clock_verb(const char* action_type);

// Resolve the configured mode once, from CK3_PIPELINE_MODE (legacy|pipeline|
// null). Called from bridge init; safe to call twice.
void pipeline_init_from_env();

// Start / stop the conductor's timer thread. start() is a no-op unless the
// mode is `pipeline`.
void pipeline_start();
void pipeline_stop();

// ============================================================================
// THE POLARITY AUTHORITY (§2.2)
// ============================================================================
// One function, derived from the bridge's OWN classifiers
// (is_deferred_drain_action / is_game_thread_action / action_is_mutation /
// is_bridge_internal_query / is_game_state_query), so there is exactly one
// place where an action's required clock state is decided. The scheduler
// cannot partition a buffer by a property the client owns, and it cannot
// invent a polarity it is handed — which is why this is a prerequisite for the
// flush loop and not a tidy-up.
Polarity classify_polarity(const char* action_type);

// True when this call must be handed to the existing game-thread drain rather
// than served inline. Mirrors the reader thread's existing routing decision
// exactly (ipc_server.cpp's `ipc_safe_deferred` carve-out).
bool action_routes_to_drain(const char* action_type);

// ============================================================================
// IPC entry points
// ============================================================================
// Called by the reader thread for every non-handshake line, BEFORE the legacy
// inline/queue ladder. Returns true when the pipeline consumed the envelope
// (replies already emitted). In legacy mode this returns false for everything
// except the mode-independent `get_action_polarity` introspection verb, so the
// legacy ladder below it is reached unchanged.
bool pipeline_handle_envelope(const std::string& line, Session* session);

// THE FLUSH A HANDLER IS RUNNING INSIDE (ck-6kvq.12, R4 / §3.2).
//
// Thread-local, set by the conductor around each `serve()` and 0 everywhere
// else — on the reader thread in legacy mode, on the game thread, and outside
// any flush. It exists because `board_post` must stamp the SAME flush_id as the
// actions around it (that identity is what makes time-to-muster and reply
// latency measurable against a real clock rather than against wall-clock
// noise), and a handler otherwise has no way to learn which flush is applying
// it. A 0 means "not inside a flush", which is a legitimate answer, not an
// error: legacy and null modes have no flushes at all.
uint64_t pipeline_current_flush_id();

// Called by the game-thread drain (bridge_init.cpp) after it has produced a
// response for a queued action, so a ticket handed to the drain is settled by
// the drain and never silently left RUNNING. No-op in legacy mode.
//
// ck-6kvq.19: `envelope_ticket` is the BRIDGE-MINTED tag the conductor put on
// the drain envelope (DrainRegistry::ticket_from_envelope; 0 when the envelope
// did not come from the conductor). It replaces the client's `request_id` as
// the correlation key, because request_id is OPTIONAL on the wire and not
// required to be unique: keyed by it, every call from a request_id-less client
// executed and then never settled, and two calls sharing one request_id
// collided. `request_id` is still passed for logging and for the negative
// control; it is NEVER used to correlate.
// ck-0lph: the attribution spine for a drain-settled ticket, as a frame
// fragment beginning with ','. False when the ticket did not resolve — and
// then `out` is NOT filled with a blank spine, because a blank one reads as
// attribution that was checked.
bool pipeline_attribution_fragment(uint64_t ticket, std::string* out);

void pipeline_note_drain_result(uint64_t envelope_ticket, const char* request_id,
                                const char* action_type,
                                const std::string& response_json);


// Session lifecycle -> early-flush condition (b) and fairness rotation.
void pipeline_register_session(const char* session_id);

// ── ck-h6nx (D'): declare THE episode's observer ────────────────────────────
// The one session whose submission serves the READ lane early. First
// declaration wins; a second claimant is refused the privilege and logged, so
// an episode has a single answer to "who drove the cadence". Buffered WRITES
// are never served on this trigger — the instrument does not decide when an
// agent's action lands.
// ck-m3n5: THE MODE IS A PARAMETER because this is where the privilege is
// CONFERRED. Before this the mode floor lived only at the ipc_server call site,
// so a caller that logged REFUSED and then declared anyway conferred it on a
// control session — and ck-ce4h's lane, which read the call site's SOURCE TEXT,
// passed 9/9 on exactly that. Asking the floor HERE makes that mutant harmless
// rather than merely detectable: this function is the ONLY caller of
// set_privileged_reader, so a privilege that this refuses cannot be conferred by
// any route.
void pipeline_declare_episode_observer(const char* session_id,
                                       const char* client_name,
                                       Session::Mode minted_mode);



// Who holds it, for the surface that records it ("" when nobody declared).
std::string pipeline_episode_observer();

// ── THE EPISODE HAS OPENED. Release the clock hold. ────────────────────────
// ck-zen6 held the world paused until an episode observer declared, and the arm
// path NEVER DECLARES: `episode_observer: True` is sent in exactly one place in
// the tree, meter_lane.run_live(), the STANDALONE entry point that builds its
// own client. episode_driver embeds ml.MeterLane(self.guard, ...) — the CLASS,
// on the DRIVER's client — and contains zero occurrences of the flag. The node
// pilot therefore ran 17 minutes at 0.0000 game-days/sec: 18 samples, one
// distinct game_date, paused, at the pin. The fix did exactly what it said and
// nothing ever satisfied its release condition.
//
// THE DIAGNOSIS IS THE NAME: ck-zen6 is called "the clock starts when the
// EPISODE starts" and was KEYED on "an episode observer declares". Those are
// different events, and the arm driver does the first and not the second.
//
// WHY NOT MAKE THE DRIVER DECLARE. That is the small diff and it is the wrong
// one: it creates a SECOND DECLARER, and first-wins then makes the read-lane
// privilege a race — the exact hazard ck-zi99's census exists to prevent, and
// one it would NOT catch, because both declarers would be legitimate.
//
// WHY TWO RELEASE SIGNALS CANNOT RACE, which is what makes this safe where a
// second declarer would not be: RELEASING THE CLOCK CONFERS NOTHING AND IS
// IDEMPOTENT. The observer privilege is exclusive and first-wins because two
// privileged readers would be two things driving the cadence; "the episode has
// begun" is a fact, and two sources agreeing on it is not a conflict. So the
// hold releases on EITHER the observer's declaration OR the episode identity,
// and the observer privilege goes back to meaning only what it is for.
void pipeline_note_episode_open(const char* episode_id);
void pipeline_unregister_session(const char* session_id);

// Session lifecycle -> the §2.7 unavailability ledger. Separate from the pair
// above on purpose: those are pipeline-mode scheduler bookkeeping, these are
// MODE-INDEPENDENT accounting, so an episode's covariate does not depend on
// which mode the bridge happened to boot in.
//   open     — a CONTROL session appeared (handshake, or a .5 resume rebinding
//              the same id, which is an ATTACH and never a reset). Observers are
//              not tracked at all.
//   detached — the socket went away. Accrual CONTINUES: under the claim pin a
//              detached session still owns its character, so its silence is
//              still a hole in the record.
//   ended    — the registry record is gone for good (free-play destroy, or a
//              refused resume). The record is KEPT and KEEPS ACCRUING, so a
//              phantom session inflates the covariate toward invalidation
//              instead of disappearing from it.
void pipeline_note_session_open(const Session* session);
void pipeline_note_session_detached(const char* session_id);
void pipeline_note_session_ended(const char* session_id);
// ck-j6lk: for a session that was NEVER A PARTICIPANT (state a). Forgets the
// unavailability record rather than ending it, so the session is ABSENT from
// unavailability_game_days instead of present with an open span. Every other exit
// keeps pipeline_note_session_ended and its documented accrual.
void pipeline_forget_session(const char* session_id);

// ck-y6fp: RE-ARM THE CLOCK HOLD when the session that opened the episode is
// gone for good. `why` is a short literal for the log ("session_ended" |
// "session_lost").
//
// A NO-OP unless `session_id` is the session that declared the episode open, so
// an unrelated session ending never touches the clock. Called from
// pipeline_note_session_ended (destroy) and from the detached sweep (LOST), which
// are the bridge's two ways of losing a session for good — a transient
// reconnect reaches neither, because it resumes the SAME session id first.
void pipeline_note_episode_owner_gone(const char* session_id, const char* why);

// ck-tpvn: THE DESTROY PATH GETS A BOUND, so both ways of losing a session are
// bounded and the re-arm cannot beat a reconnect.
//
// The bridge loses a session two ways and ck-y6fp treated them as one:
//   EXPERIMENT  a drop DETACHES; the session is resumable by token and becomes
//               LOST only past max_detach_s. Already bounded — the sweep IS the
//               bound — so that path still re-arms immediately when it fires.
//   OTHERWISE   the session is DESTROYED at once, and ck-y6fp re-armed with NO
//               bound. But the harness client reconnects in EVERY mode, so the
//               re-arm was right that THAT SESSION is gone and wrong that THE
//               EPISODE is over: it freezes a world whose driver is about to
//               come back under a new id.
//
// So a destroy starts a GRACE WINDOW instead, and this is what closes it. Call
// it from the conductor with a monotonic clock; a declaration arriving inside
// the window cancels it. Returns true if it re-armed.
bool pipeline_tick_owner_grace(uint64_t now_ms);

// ============================================================================
// THE PULSE LEDGER — engine glue (R1c, §2.9)
// ============================================================================
// The ledger itself is pure (pulse_ledger.{h,cpp}). These four functions are the
// only place it meets the game: they resolve `is_mapped` / raw loads and stores,
// and they decide WHEN the "assert empty" is taken.
//
// THE LEDGER IS MODE-INDEPENDENT. The hazard §2.9 describes — a `siglongjmp` out
// of a pulse leaving an engine global clobbered for the rest of the process —
// exists in `legacy` mode today, on the shipping bridge. So the ledger is armed
// in every mode; only the experiment-mode CONTAINMENT below is gated.
//
// COVERAGE. Tier A is every pulsed engine global whose pulse is a plain data
// store, which is what a ledger whose contract is "restore the saved value" can
// actually heal:
//
//   AI_WAR_SUPPRESS          u8   intent_dispatch.cpp, ai_eval_hook.cpp   (§2.9's own example)
//   ALLOW_RANDOM_IN_SCOPE    u64  arm_/restore_fake_random_scope helpers  (~21 call sites)
//   PLAYER_CHAR_ID           u32  intent_dispatch.cpp x3                  (raise_armies/raiders/create_title)
//   DISPATCH_MGR_RECORDER    u64  ai_eval_hook.cpp DIRECT_CMD_V7
//   COMMAND_DISPATCH_GATE    u8   ai_eval_hook.cpp acceptance query
//   DEBUG_YESMEN             u8   ai_eval_hook.cpp execute_direct_dispatch
//   DEBUG_BYPASS             u8   ai_eval_hook.cpp execute_direct_dispatch
//
// Deliberately OUT of Tier A, and why (each is recorded in the bead report):
//   - the score-hook rel32 `.text` patch (ai_eval_hook arm_/disarm_score_hook):
//     healing it needs mprotect, and rewriting `.text` from the conductor thread
//     while the game thread runs it is a worse hazard than the one being fixed.
//   - the CB-selector override (ai_cb_selector_hook.cpp): a bridge-side struct,
//     not an address, so "read it back and restore it" has no meaning.
//   - the one-way writes that are NOT pulses and never had a restore:
//     INTERACTION_DB+0x1000, the war-data front-buffer entry[0], the
//     event_handler pause flag, and the AI subsystem toggle bytes. A ledger
//     cannot restore a value nobody saved; these want a fix, not a ledger.
//
// NOTE ON THE DESIGN DOC: §2.9 names "the DeclareWar subsystem byte" as the
// second pulsed global. It is not one. `AI_TOGGLE_BASE+0x07` has exactly one
// writer (do_toggle_ai_subsystem, action_handlers.cpp), a one-way
// `*flag = enabled ? 1 : 0` with no save and no restore anywhere in the bridge.

// Enter the ledger BEFORE writing the global. `global` and `site` must outlive
// the pulse (string literals). Returns a handle, or 0 if the ledger is full or
// the address is unusable — in which case the caller's own restore is still the
// primary mechanism and nothing changes.
uint64_t pulse_enter_global(const char* global, const char* site, uintptr_t addr,
                            uint8_t width, uint64_t saved);

// Leave the ledger AFTER the restore, on every exit path. Handle 0 is a no-op.
void pulse_exit_global(uint64_t handle);

// Same, for a pulse whose restore function never sees a handle (the
// arm_/restore_fake_random_scope pair): frees the most recent live slot for
// `addr` on this thread.
void pulse_exit_addr(uintptr_t addr);

// THE ASSERTION, at three anchors.
//
//  pulse_sweep_drain — GAME THREAD, immediately after `drain_and_execute_intents`
//    returns (normal path AND the outer sigsetjmp recovery path, which is the
//    exact jump §2.9 is about). Restricted to pulses this thread entered, so
//    the drain pass being over makes any survivor unambiguously leaked.
//
//  pulse_sweep_ipc — READER THREAD, after one envelope's inline dispatch. Same
//    reasoning for the ~21 ALLOW_RANDOM_IN_SCOPE pulses that happen there.
//
//  pulse_sweep_flush — CONDUCTOR THREAD, end of every flush. §2.9's literal
//    assertion, and the only net that is not thread-local. Generation-guarded
//    (see pulse_ledger.h): it convicts only a pulse that has already survived a
//    whole flush boundary, because a false pulse_leak aborts an episode.
size_t pulse_sweep_drain();
size_t pulse_sweep_ipc();
size_t pulse_sweep_flush(uint64_t flush_id);

// EXPERIMENT-MODE CONTAINMENT (§2.9: "in experiment mode abort the episode with
// that typed reason"). The bridge cannot terminate an episode — that is the
// abort ladder's job (ck-6kvq.1, reason `pulse_leak`, exit 13). What the bridge
// does is (1) latch the verdict and publish it as `pipeline.pulse_leak` on
// get_tick_info, and (2) in EXPERIMENT mode refuse every further control-session
// submission with {reason:"episode_aborted"}, so nothing else contaminates a run
// whose validity is already gone while the ladder notices.
bool        pipeline_episode_aborted();
const char* pipeline_abort_reason();

// ============================================================================
// UNAVAILABILITY ACCOUNTING — engine glue (R3 as REVERSED, §2.7)
// ============================================================================
// G3' was reversed by the progenitor on 2026-09-12: in experiment mode THE
// CLOCK RUNS, and an agent's silence is MEASURED rather than waited out. The
// checkpoint clock gate built for the original ruling is withdrawn; what is left
// is a covariate.
//
// The ledger itself is pure (unavailability.{h,cpp}); these are the only places
// it meets the game — they resolve the current GAME DAY and decide what counts
// as activity. `episode_policy.{h,cpp}` owns the mode and the two thresholds.

// Resolve the episode policy (mode, id, X, min span) from the environment and
// configure the ledger with it. Called once from bridge init.
void pipeline_episode_init_from_env();

// Any submission OR read from a CONTROL session resets its silence (§2.7).
// Called from the IPC reader for every envelope, in EVERY pipeline mode, before
// the legacy/pipeline fork — a read served by the synchronous fast path is still
// activity, and an experiment must not be able to record a hole that only
// exists because the agent happened to poll rather than act.
// OBSERVER TRAFFIC NEVER COUNTS: observe sessions are never registered with the
// ledger, and an unknown session id is ignored.
void pipeline_note_session_activity(const Session* session);

// Bead .13's hook: mark a control session's decision budget as exhausted. With
// the gate gone this no longer changes any clock behaviour — it is carried on
// the wire (`pipeline.budget_exhausted`) so .13's governor and the episode
// bundle can tell "silent because spent" from "silent because capped", which
// are different holes in the record.
void pipeline_set_budget_exhausted(const char* session_id, bool exhausted);

// ============================================================================
// Telemetry (§2.10) — published on get_tick_info, Tier-1, always safe.
// ============================================================================
// Emits the `pipeline` object:
//   {mode, flush_id, last_flush_ms, last_flush_age_ms, buffer_depth,
//    gate_state, waiting_on, episode_id, episode_mode, tickets_open,
//    tickets_evicted, pulse_leak, last_activity, unavailability, ...}
//
// `last_activity` is per CONTROL session — the game date (and day number) of its
// most recent submission or read, the raw material of §2.7's covariate.
// `unavailability` carries the computed spans and both totals.
//
// `gate_state` and `waiting_on` SURVIVE THE GATE'S WITHDRAWAL as constants
// ("inert" and []). They are read by ck-6kvq.1's SHIPPED abort ladder, which
// keys its hold timer on "$gate_state|$waiting_on"; dropping the keys would
// change that ladder's progress key on a bridge it was not rebuilt against, for
// no gain. Constant-"inert" is never in the ladder's held set, so the gate can
// never fire — which is exactly the withdrawal. The `gate_held_beyond_max`
// termination reason retires with it (knock-on to ck-6kvq.1, §6.3).
//
// episode_id has no producer yet (bead .2) unless CK3_EPISODE_ID is set; it is
// emitted as "" rather than omitted, so the lanes coding against this shape keep
// a stable key set.
//
// `last_flush_ms` vs `last_flush_age_ms` (ck-6kvq.9 left this open): the name
// says stamp, G9's probe wants age, and ck-6kvq.1's ladder already reads
// `last_flush_ms` AS AN AGE ("no flush for ${pl_flush_ms}ms"). The consumer wins
// — a monotonic stamp is meaningless to anything outside this process, and one
// of the two readings makes the shipped ladder abort on a healthy bridge.
// `last_flush_ms` is therefore the AGE in milliseconds since the last flush;
// `last_flush_age_ms` is its explicit-named twin and carries the same number;
// `last_flush_mono_ms` carries the raw stamp for anyone debugging in-process.
//
// `pulse_leak` (§2.9) is null while clean and an OBJECT once latched. Null
// matters: ck-6kvq.1 treats any non-empty, non-"0", non-"false" value as the
// abort, so an empty object would be a false abort on a healthy run.
void pipeline_emit_state(ResponseBuilder& r);

}  // namespace bridge
