// exclusion_armed_probe.cpp — IS THE EXCLUSION ARMED BY A MEASUREMENT? (ck-029l)
//
// `exclusion_armed` used to be republished from a field resolved ONCE at episode
// open, out of the environment: armed meant "a horizon was declared", and it was
// re-emitted at every close without ever consulting whether a game-day had been
// observed. So an episode that declared the registered horizon and then observed
// NOTHING published exclusion_armed:true — X applied to a tracker with no days,
// which is a vacuous pass wearing the shape of a measured one.
//
// A FIELD RESOLVED AT OPEN CANNOT WITNESS A STATE AT CLOSE. armed is now
// armed-by-horizon AND has_day(), with the abstention NAMED as no_day_observed
// rather than folded into the pre-existing unarmed reason.
//
// WHY THIS PROBE AND NOT A GAP. My first instrument was going to script a
// 400-game-day session gap. That would have exercised observed_game_days and the
// span arithmetic — ck-2ngg's field — and greened beside the defect, because the
// defect's state is has_day() == false: ZERO days, the trivial state of a fresh
// ledger, not a long one. The gap was the neighbouring claim.
//
// WHY NO EXTRACTION. The feed (note_day) is in conductor_loop and the decision is
// in pipeline_emit_state, which is the split that suggests ck-5tg7's treatment.
// It is not needed: pipeline_emit_state takes only a ResponseBuilder, and BOTH
// inputs are publicly settable — the armed side through setenv plus
// pipeline_episode_init_from_env, the day side by calling
// unavailability().note_day() or not calling it. The conductor never runs here.
// clock_release_probe (ck-1var) is the standing proof that pipeline.cpp links on
// a host; the comment saying otherwise was an inherited assumption, and this is
// the second bead that nearly inherited it.
//
// EVERY ROW READS THE REAL EMITTED JSON, not armed_effective. A mutant that
// publishes a literal `true` is exactly the shape that survived two lanes in
// ck-8o5j (a shipping condition a constant satisfies is a rubber stamp), and a
// bool read back off the real emit cannot be faked by one.
//
// CELL ORDER IS FORCED, and the reason matters for reading the output:
// has_day() is MONOTONE — the ledger has no reset — so every no-day cell must
// run before the first note_day(). Within the no-day cells only the ARMED one
// trips the refusal log, so there is exactly one triggering cell by
// construction, and cell 1b re-drives it to prove the log is latched rather
// than asserting the latch in a comment.
#include "pipeline.h"
#include "episode_policy.h"   //: kRegisteredHorizon*, kUnarmedNoHorizon
#include "unavailability.h"   //: the ledger whose has_day() is the second input
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
// ── READING THE REAL EMIT ──────────────────────────────────────────────────
// Three-valued on purpose. "<absent>" is not false and "<null>" is not a reason:
// a field the emitter never wrote and a field it deliberately nulled are
// different observations, and collapsing either into a value is how a probe
// reports a state the bridge never published.
static std::string emit_now() {
    bridge::ResponseBuilder r;
    bridge::pipeline_emit_state(r);
    return r.take();
}

static std::string bool_field(const std::string& s, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t i = s.find(k);
    if (i == std::string::npos) return "<absent>";
    const size_t b = i + k.size();
    if (s.compare(b, 4, "true") == 0)  return "true";
    if (s.compare(b, 5, "false") == 0) return "false";
    if (s.compare(b, 4, "null") == 0)  return "<null>";
    return "<unparsed>";
}

static std::string string_or_null_field(const std::string& s, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t i = s.find(k);
    if (i == std::string::npos) return "<absent>";
    const size_t b = i + k.size();
    if (s.compare(b, 4, "null") == 0) return "<null>";
    if (s[b] != '"') return "<unparsed>";
    const size_t e = s.find('"', b + 1);
    return e == std::string::npos ? "<unterminated>" : s.substr(b + 1, e - b - 1);
}

static int g_failures = 0;
static void row(const char* name, bool ok, const std::string& detail) {
    printf("%-52s %s  %s\n", name, ok ? "PASS" : "FAIL", detail.c_str());
    if (!ok) ++g_failures;
}

// THE REFUSAL LOG, read back rather than greped. g_logfile is ours here, so the
// line the bridge writes when it refuses to arm is an OBSERVABLE, not a literal
// in the source that a `strings` run happens to find.
static const char* kRefusalFragment = "exclusion REFUSED";
static long        g_log_mark = 0;

static std::string log_since_mark(FILE* f, const char* path) {
    fflush(f);
    FILE* rd = fopen(path, "rb");
    if (!rd) return std::string();
    fseek(rd, 0, SEEK_END);
    const long end = ftell(rd);
    if (end <= g_log_mark) { fclose(rd); return std::string(); }
    std::string buf(static_cast<size_t>(end - g_log_mark), '\0');
    fseek(rd, g_log_mark, SEEK_SET);
    const size_t n = fread(&buf[0], 1, buf.size(), rd);
    buf.resize(n);
    fclose(rd);
    g_log_mark = end;
    return buf;
}

static void arm_by_horizon(bool armed) {
    if (armed) {
        setenv("CK3_EPISODE_HORIZON_START", bridge::kRegisteredHorizonStart, 1);
        setenv("CK3_EPISODE_HORIZON_END",   bridge::kRegisteredHorizonEnd,   1);
    } else {
        unsetenv("CK3_EPISODE_HORIZON_START");
        unsetenv("CK3_EPISODE_HORIZON_END");
    }
    bridge::pipeline_episode_init_from_env();
}

int main() {
    // HELD CONSTANT ACROSS ALL FIVE CELLS. Mode and pipeline mode are not the
    // variable under test; they are set to the condition in which this defect
    // actually mattered, so no cell differs from another in anything but the two
    // inputs named in its label.
    setenv("CK3_PIPELINE_MODE", "pipeline", 1);
    setenv("CK3_EPISODE_MODE", "experiment", 1);
    setenv("CK3_EPISODE_ID", "ep-029l-probe", 1);
    bridge::pipeline_init_from_env();

    char logpath[] = "/tmp/ck3-029l-probe-XXXXXX";
    const int fd = mkstemp(logpath);
    FILE* lf = fd >= 0 ? fdopen(fd, "w+") : nullptr;
    bridge::g_logfile = lf;

    // ── CELL 1  armed by horizon, NO day observed — THE DEFECT ─────────────
    arm_by_horizon(true);
    {
        const std::string s = emit_now();
        const std::string armed  = bool_field(s, "exclusion_armed");
        const std::string reason = string_or_null_field(s, "exclusion_unarmed_reason");
        row("c1_armed_horizon_NO_day__armed_is_FALSE", armed == "false",
            "exclusion_armed=" + armed + " — armed over zero observation is a "
            "vacuous pass, and a horizon declared at open cannot witness it");
        row("c1_armed_horizon_NO_day__reason_is_no_day_observed",
            reason == "no_day_observed", "exclusion_unarmed_reason=" + reason);
        const std::string got = lf ? log_since_mark(lf, logpath) : std::string();
        row("c1_the_refusal_is_LOGGED_once",
            got.find(kRefusalFragment) != std::string::npos,
            "the refusal line is read back out of g_logfile, not greped for in "
            "the source — THIS is the cell that carries the log assertion");
    }

    // ── CELL 1b  the same state again — THE LATCH, EXECUTED ────────────────
    // The refusal is guarded by a process-wide static atomic, so it fires at most
    // ONCE per process. A reader of a later run who sees no line beside a second
    // armed-with-no-day cell must not file that silence as a defect, so the
    // silence is asserted here instead of being explained in a comment.
    {
        const std::string s = emit_now();
        row("c1b_same_state_still_refuses",
            bool_field(s, "exclusion_armed") == "false" &&
            string_or_null_field(s, "exclusion_unarmed_reason") == "no_day_observed",
            "the verdict is re-derived every emit — it is not itself latched");
        const std::string got = lf ? log_since_mark(lf, logpath) : std::string();
        row("c1b_the_log_line_does_NOT_repeat",
            got.find(kRefusalFragment) == std::string::npos,
            "latched by a process-wide static: EXPECTED silence here, and the "
            "reason cell 1 is the only cell that can carry the log assertion");
    }

    // ── CELL 2  NOT armed, NO day — PRECEDENCE ─────────────────────────────
    // The row that is easy to omit. With both absent, no_day_observed must NOT
    // swallow the reason that was already there: "nobody declared a horizon" and
    // "a horizon was declared and nothing was observed" are different defects
    // with different repairs, and reporting the second for the first loses the
    // only information the field carries.
    arm_by_horizon(false);
    {
        const std::string s = emit_now();
        const std::string reason = string_or_null_field(s, "exclusion_unarmed_reason");
        row("c2_no_horizon_NO_day__armed_is_FALSE",
            bool_field(s, "exclusion_armed") == "false",
            "exclusion_armed=" + bool_field(s, "exclusion_armed"));
        row("c2_no_horizon_NO_day__keeps_the_DECLARED_reason",
            reason == bridge::kUnarmedNoHorizon,
            "exclusion_unarmed_reason=" + reason + " — and NOT no_day_observed");
    }

    // ── from here on a day HAS been observed; has_day() is monotone ─────────
    bridge::unavailability().note_day(389348, 1);

    // ── CELL 3  armed by horizon, day observed — THE PASS ───────────────────
    arm_by_horizon(true);
    {
        const std::string s = emit_now();
        const std::string reason = string_or_null_field(s, "exclusion_unarmed_reason");
        row("c3_armed_horizon_WITH_day__armed_is_TRUE",
            bool_field(s, "exclusion_armed") == "true",
            "exclusion_armed=" + bool_field(s, "exclusion_armed") +
            " — the ONE cell in which arming is a claim about a measurement");
        row("c3_armed_horizon_WITH_day__reason_is_null", reason == "<null>",
            "exclusion_unarmed_reason=" + reason + " — armed carries no reason");
    }

    // ── CELL 4  NOT armed, day observed ────────────────────────────────────
    arm_by_horizon(false);
    {
        const std::string s = emit_now();
        const std::string reason = string_or_null_field(s, "exclusion_unarmed_reason");
        row("c4_no_horizon_WITH_day__armed_is_FALSE",
            bool_field(s, "exclusion_armed") == "false",
            "exclusion_armed=" + bool_field(s, "exclusion_armed") +
            " — an observed day does not arm what was never declared");
        row("c4_no_horizon_WITH_day__keeps_the_DECLARED_reason",
            reason == bridge::kUnarmedNoHorizon,
            "exclusion_unarmed_reason=" + reason);
    }

    // WHICH CELL CARRIED THE LOG, stated in the output so a reader of a later
    // run does not have to reconstruct it from the source.
    printf("\nlog assertion: carried by CELL 1, the first and only cell that can.\n"
           "  The refusal is latched by a process-wide static, so cell 1b — the\n"
           "  same armed-with-no-day state — correctly shows NO line, and that\n"
           "  silence is itself asserted rather than assumed. Cells 2 and 4 are\n"
           "  unarmed and cell 3 has a day, so none of the three can trip it.\n");

    if (lf) { fclose(lf); remove(logpath); }
    printf("\n%s (%d failing rows)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
