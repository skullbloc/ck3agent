// clock_release_probe.cpp — DOES THE RELEASE ACTUALLY RELEASE? (ck-1var)
//
// ck-t5bc found two surviving mutants in acceptance_clock_start, both at sites I
// did not pick, and both with ONE cause: THE LANE NEVER LINKED pipeline.cpp.
//
//   M1  delete `set_hold_clock_until_observer(false)` from INSIDE
//       pipeline_note_episode_open, keeping the call and the log line.
//       Lane UNCHANGED. The RELEASE — the headline claim of ck-8o5j — had no
//       executed witness anywhere in the tree. The old probe drove the HOLD and
//       GREPPED the arming, and my answer to "is the call there" was a
//       structural row at the CALL SITE, which M1 leaves intact while gutting
//       the callee.
//
//   M3  emit `clock_hold_armed` as a literal false. Lane UNCHANGED, and
//       ck-9rto's cross-check UNCHANGED — a SHIPPING CONDITION that a constant
//       satisfies is a rubber stamp, and `false` reads as "released", so every
//       launch passes. That is the gate_state trap I caught in THEIR draft,
//       shipped in MY field a bead later.
//
// AND THE REASON THE LANE DID NOT LINK IT WAS AN ASSUMPTION, NOT A FACT. A
// comment SAID it "cannot be linked without the game" — in bridge/CMakeLists.txt
// and in pipeline_frames.h, both corrected by ck-1var once this ran. It links
// here with ZERO undefined symbols against the ordinary host source set plus the
// engine stubs below. I inherited that sentence and never tested it.
//
// So this drives the REAL pipeline_note_episode_open and reads the REAL
// pipeline_emit_state, and asserts THE FIELD FLIPS — armed before the signal,
// released after. Not that the key is present: a grep is a claim about spelling.

#include "pipeline.h"
#include "action_dispatch_internal.h"
#include "bridge.h"
#include "personality_locks.h"
#include "session.h"
#include "world_identity.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <shared_mutex>
#include <string>
#include <vector>
#include <csetjmp>

// The engine boundary, and nothing else. Every other symbol comes from the real
// host TUs in the link line.
namespace bridge {
FILE* g_logfile = nullptr;
uintptr_t             g_base_addr = 0;
std::vector<MemRange> g_valid_ranges;
std::shared_mutex     g_ranges_mutex;
std::mutex g_controlled_mutex;
std::vector<uint32_t> g_controlled_chars;
std::atomic<uint32_t> g_controlled_primary{0};
std::atomic<uint32_t> g_controlled_heir{0};
std::atomic<bool> g_agent_mode{false};
std::atomic<uint64_t> g_tick_id{0};
bool is_paused() { return false; }
int  game_get_speed() { return 3; }
void game_pause() {}
void game_unpause() {}
void game_set_speed(int) {}
bool do_pause_game(const char*) { return true; }
bool do_unpause_game(const char*) { return true; }
bool do_set_speed(const char*) { return true; }
bool dispatch_action_with_response(const char*, const char*, std::string*) { return true; }
bool enqueue_action_envelope(const std::string&) { return true; }
SessionState get_session_state() { return SessionState::ACTIVE; }
std::string enumerate_decisions() { return "[]"; }
std::string enumerate_interactions() { return "[]"; }
thread_local bool g_in_protected_region = false;
thread_local sigjmp_buf g_recovery_point;
// The action CLASSIFIERS. Stubbed rather than linked (dispatch_router.cpp would
// drag in 29 more engine symbols) because the claim under test — does the hold
// flip — does not depend on how any action classifies. What it depends on is
// pipeline_note_episode_open and pipeline_emit_state, and both are REAL here.
bool is_registered_action(const char*) { return false; }
bool is_deferred_drain_action(const char*) { return false; }
bool is_bridge_management_verb(const char*) { return false; }
size_t load_personality_locks_from_disk() { return 0; }
size_t personality_lock_count() { return 0; }
const char* personality_locks_path() { return ""; }
size_t restore_quarantined_personality_locks() { return 0; }
size_t personality_quarantine_count() { return 0; }
uint64_t personality_quarantine_epoch() { return 0; }
void set_personality_locks_identity(const WorldIdentity&) {}
bool send_observation(const char*, size_t) { return true; }
bool peek_personality_locks_identity(WorldIdentity*) { return false; }
std::string build_envelope_clock_fragment() { return "\"tick\":0"; }
}  // namespace bridge

// gate_state IS NOT THE HOLD STATE, and this reads it off the same real emit
// rather than grepping for the literal. ck-9rto mutated the structural version
// with a BLOCK COMMENT and an UNCOMPILED FILE and it passed both times: a grep
// asks whether the text exists somewhere, not whether the bridge emits. A value
// read back after driving the real transition cannot be faked by a comment, by
// a file CMake never compiles, or by a constant — "<absent>" is not "inert".
static std::string string_field(const char* key) {
    bridge::ResponseBuilder r;
    bridge::pipeline_emit_state(r);
    const std::string s = r.take();
    const std::string k = std::string("\"") + key + "\":\"";
    const size_t i = s.find(k);
    if (i == std::string::npos) return "<absent>";
    const size_t b = i + k.size();
    const size_t e = s.find('"', b);
    return e == std::string::npos ? "<unterminated>" : s.substr(b, e - b);
}

static bool armed_in_state() {
    bridge::ResponseBuilder r;
    bridge::pipeline_emit_state(r);
    const std::string s = r.take();
    const size_t i = s.find("\"clock_hold_armed\":");
    if (i == std::string::npos) return false;          // ABSENT is not RELEASED
    return s.compare(i + 19, 4, "true") == 0;
}

int main() {
    // PIPELINE MODE, so the gate arms from clock ownership rather than a flag.
    setenv("CK3_PIPELINE_MODE", "pipeline", 1);
    setenv("CK3_EPISODE_ID", "ep-probe", 1);
    // BOTH inits, in the order bridge_init uses them: the MODE is read by
    // pipeline_init_from_env (it is what pipeline_owns_clock answers from), and
    // the episode policy — which arms the clock gate FROM that ownership — by
    // pipeline_episode_init_from_env. Calling only the second armed nothing and
    // the probe reported zeros, which is the probe being right about a setup I
    // had got wrong rather than a finding about the subject.
    bridge::pipeline_init_from_env();
    bridge::pipeline_episode_init_from_env();

    const bool before = armed_in_state();
    const std::string gate_before = string_field("gate_state");
    bridge::pipeline_note_episode_open("ep-probe");
    const bool after = armed_in_state();
    const std::string gate_after = string_field("gate_state");

    // A SECOND OPEN MUST BE A NO-OP, not a re-arm.
    bridge::pipeline_note_episode_open("ep-probe");
    const bool after_twice = armed_in_state();

    // AN EMPTY EPISODE ID IS NOT AN EPISODE and must not release anything.
    bridge::pipeline_episode_init_from_env();      // re-arm from the same mode
    const bool rearmed = armed_in_state();
    bridge::pipeline_note_episode_open("");
    const bool after_empty = armed_in_state();

    std::printf("case\tvalue\n");
    std::printf("armed_before_open\t%d\n", before ? 1 : 0);
    std::printf("armed_after_open\t%d\n", after ? 1 : 0);
    std::printf("armed_after_second_open\t%d\n", after_twice ? 1 : 0);
    std::printf("rearmed_by_init\t%d\n", rearmed ? 1 : 0);
    std::printf("armed_after_empty_id\t%d\n", after_empty ? 1 : 0);
    // SAME STRING BOTH SIDES is the claim: if anyone ever repurposes gate_state
    // to carry the hold, it differs across the flip BY CONSTRUCTION and this
    // reds — which is the drift the retired grep existed to catch.
    std::printf("gate_state_before\t%s\n", gate_before.c_str());
    std::printf("gate_state_after\t%s\n", gate_after.c_str());

    // ── ck-kc32: THE PUBLISHED EPISODE ID ────────────────────────────────────
    // WITH CK3_EPISODE_ID UNSET, which is the real arm shape: the driver mints
    // its id at BUNDLE OPEN, long after launch, so the env var cannot carry it.
    // The bridge must learn it from the declaration and PUBLISH it, or
    // ck-9rto's episode-ack gate compares the driver's id to "" forever.
    unsetenv("CK3_EPISODE_ID");
    bridge::pipeline_init_from_env();
    bridge::pipeline_episode_init_from_env();
    const std::string id_before = string_field("episode_id");
    bridge::pipeline_note_episode_open("ep-minted-at-bundle-open");
    const std::string id_after = string_field("episode_id");

    // AND AGAIN WITH THE HOLD ALREADY DOWN. This is the discriminating case: the
    // store sits BEFORE the hold guard, so a second declaration still publishes.
    // Had it sat after, this would keep the first id — and so would every legacy
    // and free-play launch, where the hold is never armed at all.
    bridge::pipeline_note_episode_open("ep-second-declaration");
    const std::string id_second = string_field("episode_id");

    std::printf("episode_id_before_open\t%s\n",
                id_before.empty() ? "<empty>" : id_before.c_str());
    std::printf("episode_id_after_open\t%s\n", id_after.c_str());
    std::printf("episode_id_hold_already_down\t%s\n", id_second.c_str());

    // ── ck-y6fp: THE RELEASE'S INVERSE, AND WHOSE DEPARTURE FIRES IT ─────────
    // The rule is NARROW ON PURPOSE: only the session that OPENED the episode
    // re-arms the hold by leaving. A broader rule ("no session attached") would
    // be reached by more paths, and episode end is the worst place to be wrong.
    // So the discriminating case is an UNRELATED session going away and NOTHING
    // HAPPENING — without it, a rule that re-armed on any session's departure
    // would pass every positive test here.
    bridge::pipeline_episode_init_from_env();          // re-arm from the mode
    bridge::Session owner("sess-A", "driver", bridge::Session::Mode::CONTROL, 0);
    bridge::ClientContext ctx;
    ctx.session = &owner;
    {
        bridge::ClientContextGuard g(&ctx);
        bridge::pipeline_note_episode_open("ep-owner-test");
    }
    const bool armed_after_owner_open = armed_in_state();
    // ck-tpvn: `session_lost` is the path whose bound has ALREADY been served —
    // the detached sweep waited out max_detach_s before reporting it — so it
    // still re-arms at once. The DESTROY path is bounded instead, below.
    bridge::pipeline_note_episode_owner_gone("sess-B", "session_lost");
    const bool armed_after_stranger_left = armed_in_state();
    bridge::pipeline_note_episode_owner_gone("sess-A", "session_lost");
    const bool armed_after_owner_left = armed_in_state();

    std::printf("armed_after_owner_open\t%d\n", armed_after_owner_open ? 1 : 0);
    std::printf("armed_after_STRANGER_left\t%d\n", armed_after_stranger_left ? 1 : 0);
    std::printf("armed_after_OWNER_left\t%d\n", armed_after_owner_left ? 1 : 0);

    // ── ck-tpvn: THE DESTROY PATH IS BOUNDED, AND A RECONNECT BEATS IT ──────
    // ck-y6fp re-armed the moment a DESTROYED owner was reported. The harness
    // client reconnects in every mode, so that was right about the SESSION and
    // wrong about the EPISODE: it froze a world whose driver was coming back.
    // Now a destroy starts a grace window. THE MIDDLE CASE IS THE ONE THAT
    // MATTERS — a declaration arriving inside the window cancels it, which is
    // the reconnect, and is the case a bound alone would still get wrong.
    {
        bridge::pipeline_episode_init_from_env();       // re-arm from the mode
        bridge::Session own2("sess-C", "driver", bridge::Session::Mode::CONTROL, 0);
        bridge::ClientContext c2; c2.session = &own2;
        {
            bridge::ClientContextGuard g(&c2);
            bridge::pipeline_note_episode_open("ep-grace");
        }
        // DESTROYED, not lost: no bound has been served yet.
        bridge::pipeline_note_episode_owner_gone("sess-C", "session_ended");
        const bool armed_at_destroy = armed_in_state();
        const bool armed_inside = bridge::pipeline_tick_owner_grace(30000)
                                      ? true : armed_in_state();
        // THE RECONNECT: a new session declares the same episode inside the
        // window. mono_ms() supplies the real "since", so the tick below uses a
        // now_ms far past any plausible grace.
        bridge::Session own3("sess-D", "driver", bridge::Session::Mode::CONTROL, 0);
        bridge::ClientContext c3; c3.session = &own3;
        {
            bridge::ClientContextGuard g(&c3);
            bridge::pipeline_note_episode_open("ep-grace");
        }
        const bool rearmed_after_reconnect =
            bridge::pipeline_tick_owner_grace(UINT64_C(1) << 40);
        std::printf("armed_at_destroy\t%d\n", armed_at_destroy ? 1 : 0);
        std::printf("armed_inside_grace\t%d\n", armed_inside ? 1 : 0);
        std::printf("grace_fired_after_reconnect\t%d\n",
                    rearmed_after_reconnect ? 1 : 0);

        // AND WITH NOBODY COMING BACK, the grace must actually close.
        bridge::pipeline_episode_init_from_env();
        bridge::Session own4("sess-E", "driver", bridge::Session::Mode::CONTROL, 0);
        bridge::ClientContext c4; c4.session = &own4;
        {
            bridge::ClientContextGuard g(&c4);
            bridge::pipeline_note_episode_open("ep-grace-2");
        }
        bridge::pipeline_note_episode_owner_gone("sess-E", "session_ended");
        bridge::pipeline_tick_owner_grace(UINT64_C(1) << 40);
        std::printf("armed_after_grace_expired\t%d\n", armed_in_state() ? 1 : 0);
    }

    // ── ck-tpvn / ck-9rto's QUESTION: WHAT IF THE DECLARATION IS REFUSED? ────
    // The bridge is NEVER TOLD the decision — do_declare_save_identity computes
    // rec.decision, writes it into the REPLY, and calls
    // pipeline_note_episode_open(id.episode_id) unconditionally. So the cancel
    // cannot be keyed on acceptance without new plumbing, and the question
    // becomes whether it NEEDS to be.
    //
    // WHAT THE GRACE ACTUALLY ASKS IS "IS THERE AN OWNER?", not "was the
    // declaration accepted". A declaration — refused or not — comes from a live
    // session that has just named an episode, so it ESTABLISHES an owner. The
    // invariant that matters is that the world cannot run unattended for longer
    // than one grace, and that holds only if the new owner's departure opens a
    // NEW grace. That is what this case drives, and it is the answer to
    // "a refused declaration cancels the grace and the hold never re-arms".
    {
        bridge::pipeline_episode_init_from_env();
        bridge::Session o1("sess-F", "driver", bridge::Session::Mode::CONTROL, 0);
        bridge::ClientContext k1; k1.session = &o1;
        { bridge::ClientContextGuard g(&k1);
          bridge::pipeline_note_episode_open("ep-first"); }
        bridge::pipeline_note_episode_owner_gone("sess-F", "session_ended");
        // A DIFFERENT session declares a DIFFERENT episode — the shape a
        // `refused_different_episode` declaration takes on the wire.
        bridge::Session o2("sess-G", "other", bridge::Session::Mode::CONTROL, 0);
        bridge::ClientContext k2; k2.session = &o2;
        { bridge::ClientContextGuard g(&k2);
          bridge::pipeline_note_episode_open("ep-second"); }
        const bool cancelled = !bridge::pipeline_tick_owner_grace(UINT64_C(1) << 40);
        // AND THE NEW OWNER'S DEPARTURE MUST OPEN A NEW GRACE. If it does not,
        // the cancel really is an escape hatch.
        bridge::pipeline_note_episode_owner_gone("sess-G", "session_ended");
        const bool regraced = bridge::pipeline_tick_owner_grace(UINT64_C(2) << 40);
        // ── ck-43u3: THE TRIGGER IS ON THE WIRE ─────────────────────────────────
    // Published so a reader can SEE which trigger served a flush instead of
    // inferring it from a latency. Read off the real emit; the counts start at
    // zero here because this probe runs no conductor, and ZERO IS THE HONEST
    // READING for a process that has flushed nothing — the point is that the
    // keys EXIST and carry the scheduler's own names, not that they are
    // non-zero.
    {
        const std::string tl = string_field("flush_trigger_last");
        bridge::ResponseBuilder rr;
        bridge::pipeline_emit_state(rr);
        const std::string all = rr.take();
        std::printf("flush_trigger_last\t%s\n", tl.c_str());
        std::printf("flush_triggers_has_cadence\t%d\n",
                    all.find("\"cadence\":") != std::string::npos ? 1 : 0);
        std::printf("flush_triggers_has_read_lane_early\t%d\n",
                    all.find("\"read_lane_early\":") != std::string::npos ? 1 : 0);
    }

    std::printf("refused_shape_cancels_grace\t%d\n", cancelled ? 1 : 0);
        std::printf("and_the_NEW_owner_leaving_REGRACES\t%d\n", regraced ? 1 : 0);
        std::printf("armed_after_regrace\t%d\n", armed_in_state() ? 1 : 0);
    }
    return 0;
}
