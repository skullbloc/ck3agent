// pipeline.cpp — R1b: the wired conductor (ck-6kvq.9). See pipeline.h.
//
// Design of record: docs/design/event-pipeline-conductor.md §2.2-§2.4, §2.8,
// §2.10, §8; decisions G1, G2, G11.
//
// THE ONE INVARIANT THAT MATTERS MOST. `pipeline_mode = legacy` is the default
// (G11) and must be INDISTINGUISHABLE from the pre-change bridge. Every entry
// point here begins with one relaxed atomic load and returns immediately when
// the mode is legacy; no thread is started, no buffer exists, no clock is
// touched, and the reader thread's existing ladder runs untouched.

#include "thread_registry.h"
#include "pipeline.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT
#include "board.h"

#include "bridge.h"
#include "ck3_offsets.h"
#include "action_dispatch_internal.h"
#include "snapshot.h"
#include "session.h"
#include "flush_apply.h"
#include "ticket_table.h"
#include "action_args.h"
#include "drain_registry.h"
#include "pipeline_frames.h"
#include "pulse_ledger.h"
#include "clock_leak_backstop.h"
#include "tick_bookkeeping.h"
#include "episode_policy.h"
#include "unavailability.h"
#include "clock_rate.h"   // ck-ijfa
#include "game_date.h"
#include "speed_read.h"   // ck-k57a

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <set>
#include <setjmp.h>
#include <signal.h>
#include <pthread.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bridge {

// SIGSEGV recovery (defined in bridge_init.cpp).
extern thread_local sigjmp_buf g_recovery_point;
extern thread_local bool g_in_protected_region;

// ============================================================================
// Mode
// ============================================================================
static std::atomic<int> g_pipeline_mode{static_cast<int>(PipelineMode::Legacy)};

// `pipeline_mode_name` moved to pipeline_mode.cpp (ck-6kvq.4 / ck-1ek9) — it
// was defined here AND there, which is a duplicate symbol in the .so.

PipelineMode pipeline_mode() {
    return static_cast<PipelineMode>(g_pipeline_mode.load(std::memory_order_relaxed));
}

bool pipeline_active() {
    return g_pipeline_mode.load(std::memory_order_relaxed) !=
           static_cast<int>(PipelineMode::Legacy);
}

bool pipeline_owns_clock() {
    return g_pipeline_mode.load(std::memory_order_relaxed) ==
           static_cast<int>(PipelineMode::Pipeline);
}

bool is_clock_verb(const char* action_type) {
    if (!action_type || !action_type[0]) return false;
    return strcmp(action_type, "pause_game") == 0 ||
           strcmp(action_type, "unpause_game") == 0 ||
           strcmp(action_type, "set_speed") == 0 ||
           strcmp(action_type, "pause") == 0 ||
           strcmp(action_type, "set_pause") == 0;
}

void pipeline_init_from_env() {
    // ck-1ek9: the parse is `parse_pipeline_mode` (pipeline_mode.cpp) rather
    // than a second copy of the same ladder. It is pure, it takes the string
    // instead of reading the environment, and it reports whether the value was
    // RECOGNISED — so a misspelling is still said out loud here rather than
    // starting a silently legacy episode that the bundle labels otherwise.
    const char* m = getenv("CK3_PIPELINE_MODE");
    bool recognised = true;
    const PipelineMode mode = parse_pipeline_mode(m, &recognised);
    if (!recognised && g_logfile) {
        fprintf(g_logfile,
            "[CK3-Bridge][pipeline] CK3_PIPELINE_MODE=%s is not one of "
            "{legacy,pipeline,null} — falling back to legacy (G11)\n", m);
    }
    g_pipeline_mode.store(static_cast<int>(mode), std::memory_order_relaxed);
    if (g_logfile) {
        fprintf(g_logfile, "[CK3-Bridge][pipeline] mode=%s\n", pipeline_mode_name(mode));
        fflush(g_logfile);
    }
}

// ============================================================================
// THE POLARITY AUTHORITY (§2.2)
// ============================================================================
//
// Derived from the bridge's own classifiers, in the order the reader thread
// already resolves them. The gateway's copy (mcp_server.py:129-178) becomes a
// cached timeout hint that must AGREE with this; `get_action_polarity` below
// publishes the table so a disagreement is a loud error rather than a silent
// mis-route. They already disagree in one place today — query_conquest_targets
// is an is_deferred_drain_action here and sits in the gateway's
// _UNPAUSED_READS — which is exactly how a new handler ends up mis-routed.
//
// The ORDER is load-bearing:
//   1. deferred-drain  -> TICK_READ   (needs a live tick for an ENGINE VERDICT)
//   2. game-thread     -> TICK_WRITE  (tier-3 mutation, drains in on_tick)
//   3. other mutation  -> TICK_WRITE  (declare_war/send_interaction/... enqueue
//                                      an intent that only executes on a tick)
//   4. bridge-internal -> ANY         (tempo, liveness, identity, rosters)
//   5. game-state      -> PAUSED      (live object-graph reads)
//   6. otherwise       -> UNKNOWN     (resolves to PAUSED, and is COUNTED)
//
// Step 3 is why is_bridge_internal_query alone is not the ANY set: the bridge
// classifies declare_war as "safe to call from the IPC thread", which is true,
// but the intent it enqueues does not execute until a tick runs.
Polarity classify_polarity(const char* action_type) {
    if (!action_type || !action_type[0]) return Polarity::UNKNOWN;
    if (is_deferred_drain_action(action_type)) return Polarity::TICK_READ;
    if (is_game_thread_action(action_type))    return Polarity::TICK_WRITE;
    if (strcmp(action_type, "respond_event") == 0) return Polarity::TICK_WRITE;
    if (action_is_mutation(action_type) && !is_bridge_management_verb(action_type))
        return Polarity::TICK_WRITE;
    if (is_bridge_internal_query(action_type)) return Polarity::ANY;
    if (is_game_state_query(action_type))      return Polarity::PAUSED;
    return Polarity::UNKNOWN;
}

bool action_routes_to_drain(const char* action_type) {
    if (!action_type || !action_type[0]) return false;
    // The reader thread's existing carve-out, reproduced exactly: a call whose
    // real work happens on the game thread but whose HANDLER is IPC-thread-safe
    // (the bounded-poll deferred queries — is_bridge_internal_query) is served
    // inline; everything else that needs a tick goes to g_msg_queue. The
    // graph-walkers (query_dejure_progress, query_conquest_targets) are NOT
    // IPC-thread-safe while unpaused, and the tick phase is unpaused by
    // construction, so they route to the drain.
    if (is_deferred_drain_action(action_type))
        return !is_bridge_internal_query(action_type);
    if (is_game_thread_action(action_type)) return true;
    if (strcmp(action_type, "respond_event") == 0) return true;
    return false;   // inline-dispatchable mutation (queue_intent, declare_war, ...)
}

// ============================================================================
// THE APPLYING FLUSH, for handlers (ck-6kvq.12, R4)
// ============================================================================
// Thread-local rather than a member of the conductor: the only consumer is a
// handler running synchronously underneath serve(), and a thread-local keeps
// every other thread (the reader in legacy mode, the game thread) answering 0
// with no coordination at all.
namespace {
thread_local uint64_t t_current_flush_id = 0;

struct ScopedFlushId {
    uint64_t prev;
    explicit ScopedFlushId(uint64_t id) : prev(t_current_flush_id) {
        t_current_flush_id = id;
    }
    ~ScopedFlushId() { t_current_flush_id = prev; }
    ScopedFlushId(const ScopedFlushId&) = delete;
    ScopedFlushId& operator=(const ScopedFlushId&) = delete;
};
}  // namespace

uint64_t pipeline_current_flush_id() { return t_current_flush_id; }

// ============================================================================
// Small JSON helpers
// ============================================================================
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20) { char esc[8]; snprintf(esc, sizeof(esc), "\\u%04x", uc); out += esc; }
        else if (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

static uint64_t mono_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// Render a game DAY NUMBER as the bridge's canonical "YYYY.MM.DD", through
// ck-05mg's single calendar (ck-6kvq.10 publishes `last_activity` and every
// span endpoint through here, and R5 asks for REAL dates).
static std::string game_date_for_day(int64_t day) {
    // Day 0 is year 0 January 1 — a date no CK3 save can hold, and what the
    // unavailability ledger reads before it has ever been shown a game day. Say
    // so rather than publishing "0000.01.01", which a consumer would parse as a
    // real date.
    if (day <= 0) return std::string("unknown");
    char buf[24];   // %04d can run past 4 digits on a nonsense day; leave room
    ck3::game_date::format_total_days(static_cast<int32_t>(day), buf, sizeof(buf));
    return std::string(buf);
}

// ck-05mg: this MUST go through game_date.h. The first cut of this function
// open-coded the 365/30 calendar, which is wrong by up to 4 days and renders 5
// days a year as month 13 — and it is the function that stamps
// game_date_executed on EVERY pipeline-attributed action, i.e. the spine R4/R9
// and every paper mediator read. An open-coded site here would have re-imported
// the exact bug ck-05mg paid to eradicate, into the one lane the whole
// paper-1066 pack runs on. scripts/test_game_calendar.py section D fails the
// build if a calendar is ever open-coded outside game_date.h again.
static std::string current_game_date() {
    uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
    if (!gs) return std::string("unknown");
    int32_t raw_date = safe_read<int32_t>(gs + ck3::game_state::GAME_DATE);
    char buf[16];
    ck3::game_date::format_raw(raw_date, buf, sizeof(buf));
    return std::string(buf);
}

// The game's DAY NUMBER: monotone, exact, and the only sane input to a cadence
// or to §2.7's unavailability spans. Do arithmetic on THIS, never on the
// formatted "YYYY.MM.DD" above — that string is correct since ck-05mg, but it is
// a calendar rendering and subtracting two of them is not a day count.
// INT64_MIN means "the game state is not loaded", which every caller reads as
// "no day yet".
static int64_t current_game_day() {
    uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
    if (!gs) return INT64_MIN;
    int32_t raw_date = safe_read<int32_t>(gs + ck3::game_state::GAME_DATE);
    return static_cast<int64_t>(ck3::game_date::raw_to_total_days(raw_date));
}

// ck-k57a: 0 means NO READING, and the two callers below already read it that
// way — ClockRateMeter::note ignores a speed <= 0 ("never observed", see
// clock_rate.cpp:35) and the flush scheduler re-asserts the resting speed when
// the current one does not match it, which is the right move under ignorance.
// Before this bead a root failure returned a fabricated 3 (silently identical
// to the default resting speed, so the restore step was skipped on exactly the
// reads that could not be trusted) and a FAULTED read returned 0 + 1 == 1, a
// legal speed. Both now abstain. See bridge/include/speed_read.h.
static int current_game_speed() {
    uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
    if (!gs) return bridge::speed::classify(false, false, 0).legacy_int();
    const int fault_before = safe_read_fault_count();
    const int32_t raw = safe_read<int32_t>(gs + ck3::game_state::GAME_SPEED);
    const bool faulted = safe_read_fault_count() != fault_before;
    return bridge::speed::classify(true, faulted, raw).legacy_int();
}

// ============================================================================
// Conductor state
// ============================================================================
namespace {

// The per-call payload record (PendingCall) and the drain correlation both
// live in DrainRegistry now — engine-free, and therefore host-testable.
// ck-6kvq.19: correlation used to be keyed by the client's OPTIONAL request_id,
// so every call from a client that sent none executed and then never settled.

struct Conductor {
    FlushScheduler sched;
    TicketTable    tickets;

    // Payload store + drain hand-off/settlement. Owns its own lock.
    DrainRegistry  drain;

    std::thread           thr;
    std::atomic<bool>     running{false};
    std::condition_variable wake_cv;
    std::mutex              wake_mu;

    // Telemetry (§2.10).
    std::atomic<uint64_t> flush_id{0};
    std::atomic<uint64_t> last_flush_ms{0};
    std::atomic<uint64_t> last_flush_wall_ms{0};
    std::atomic<uint64_t> last_paused_ms{0};
    std::atomic<uint64_t> clock_drift_events{0};
    std::atomic<uint64_t> unclassified_total{0};

    // ck-y6fp: WHO OPENED THE EPISODE. The clock release is a state transition
    // with no inverse on the failure path — a refusal after it left the
    // deploy-13 arm-C world running 601 game-days with no episode, driver or
    // agent. The inverse is re-arming the hold, and this is the thing whose
    // disappearance triggers it.
    //
    // KEYED ON THE SESSION ID, NEVER ON THE SOCKET. The harness client
    // reconnects transparently, and a drop DETACHES rather than destroys: the
    // resume token survives and resume_by_token returns THE SAME session id
    // (driven in acceptance_session_identity). A deadman keyed on the
    // connection would fire on a transient drop and freeze a live episode at
    // 0.0000 game-days/sec — ck-zen6's own failure, caused by the fix for it.
    std::mutex  episode_owner_mu;
    std::string episode_owner_session;
    //: ck-tpvn: 0 = nobody is pending. Set when the owner is DESTROYED, which
    //: is not by itself evidence the episode ended — the harness client
    //: reconnects in every mode. Cleared by a declaration arriving inside the
    //: window, or by the grace expiring and the hold going back on.
    uint64_t    episode_owner_gone_since_ms = 0;
};

Conductor& C() {
    static Conductor c;
    return c;
}

}  // namespace

// ============================================================================
// THE PULSE LEDGER — engine glue (R1c, §2.9)
// ============================================================================
namespace {

// The one place the ledger meets the game. Every Tier-A pulse is a plain data
// store into a mapped, writable engine global, so read-back and restore are
// ordinary loads and stores; `mapped()` is the same guard the pulse sites
// themselves use before touching the address.
class EnginePulseMemory : public PulseMemory {
public:
    bool mapped(uintptr_t addr) const override {
        return addr != 0 && is_mapped(addr);
    }
    uint64_t read(uintptr_t addr, uint8_t width) const override {
        switch (width) {
            case 1:  return *reinterpret_cast<volatile uint8_t*>(addr);
            case 2:  return *reinterpret_cast<volatile uint16_t*>(addr);
            case 4:  return *reinterpret_cast<volatile uint32_t*>(addr);
            default: return *reinterpret_cast<volatile uint64_t*>(addr);
        }
    }
    void write(uintptr_t addr, uint8_t width, uint64_t v) override {
        switch (width) {
            case 1:  *reinterpret_cast<volatile uint8_t*>(addr)  = static_cast<uint8_t>(v);  break;
            case 2:  *reinterpret_cast<volatile uint16_t*>(addr) = static_cast<uint16_t>(v); break;
            case 4:  *reinterpret_cast<volatile uint32_t*>(addr) = static_cast<uint32_t>(v); break;
            default: *reinterpret_cast<volatile uint64_t*>(addr) = v;                        break;
        }
    }
};

EnginePulseMemory& pulse_mem() {
    static EnginePulseMemory m;
    return m;
}

// Common tail for all three sweep anchors: log every leak loudly, latch the
// verdict, and — in EXPERIMENT mode — latch the episode abort with it. The
// latch itself lives in the pure ledger so the whole containment decision is
// host-tested (test_pulse_ledger.cpp) rather than only reachable with a game
// attached.
size_t report_leaks(std::vector<PulseLeak>& leaks) {
    if (leaks.empty()) return 0;
    const bool experiment = episode_is_experiment();
    const bool was_aborted = pulse_episode_aborted();
    for (const PulseLeak& l : leaks) {
        const std::string j = l.to_json();
        if (g_logfile) {
            fprintf(g_logfile, "[CK3-Bridge][pipeline] PULSE_LEAK %s\n", j.c_str());
            fflush(g_logfile);
        }
        pulse_latch_verdict(l, experiment);
    }
    if (!was_aborted && pulse_episode_aborted() && g_logfile) {
        fprintf(g_logfile,
            "[CK3-Bridge][pipeline] EPISODE ABORT reason=pulse_leak — experiment "
            "mode; refusing all further control-session work. The episode's "
            "validity is already gone (design 2.9).\n");
        fflush(g_logfile);
    }
    return leaks.size();
}

}  // namespace

uint64_t pulse_enter_global(const char* global, const char* site, uintptr_t addr,
                            uint8_t width, uint64_t saved) {
    if (addr == 0) return 0;
    return pulse_ledger().enter(global, site, addr, width, saved,
                                C().flush_id.load(std::memory_order_relaxed),
                                mono_ms());
}

void pulse_exit_global(uint64_t handle) {
    pulse_ledger().exit(handle);
}

void pulse_exit_addr(uintptr_t addr) {
    if (addr == 0) return;
    pulse_ledger().exit_newest_for_addr(addr);
}

size_t pulse_sweep_drain() {
    std::vector<PulseLeak> leaks;
    pulse_ledger().sweep(PulseScope::Drain,
                         C().flush_id.load(std::memory_order_relaxed), mono_ms(),
                         pulse_mem(), &leaks, /*self_thread_only=*/true);
    return report_leaks(leaks);
}

size_t pulse_sweep_ipc() {
    // Same rule as the drain anchor: this thread's inline dispatch is over, so
    // any pulse THIS thread still has open is leaked.
    std::vector<PulseLeak> leaks;
    pulse_ledger().sweep(PulseScope::Drain,
                         C().flush_id.load(std::memory_order_relaxed), mono_ms(),
                         pulse_mem(), &leaks, /*self_thread_only=*/true);
    return report_leaks(leaks);
}

size_t pulse_sweep_flush(uint64_t flush_id) {
    std::vector<PulseLeak> leaks;
    pulse_ledger().sweep(PulseScope::Flush, flush_id, mono_ms(),
                         pulse_mem(), &leaks, /*self_thread_only=*/false);
    return report_leaks(leaks);
}

bool pipeline_episode_aborted() {
    return pulse_episode_aborted();
}

const char* pipeline_abort_reason() {
    return pulse_abort_reason();
}

// ============================================================================
// UNAVAILABILITY ACCOUNTING — engine glue (R3 as REVERSED, §2.7)
// ============================================================================
namespace {

// Bead .13's hook. Carried, not acted on: with the gate withdrawn a spent agent
// changes no clock behaviour. It stays on the wire because "silent because
// spent" and "silent because capped" are different holes in the record and the
// bundle has to be able to tell them apart.
std::mutex                   g_exhausted_mu;
std::set<std::string>        g_exhausted CK3_EARLY_INIT;

std::set<std::string> exhausted_snapshot() {
    std::lock_guard<std::mutex> lk(g_exhausted_mu);
    return g_exhausted;
}

}  // namespace

void pipeline_set_budget_exhausted(const char* session_id, bool exhausted) {
    if (!session_id || !*session_id) return;
    std::lock_guard<std::mutex> lk(g_exhausted_mu);
    if (exhausted) g_exhausted.insert(session_id);
    else           g_exhausted.erase(session_id);
}

void pipeline_episode_init_from_env() {
    episode_policy().init_from_env();
    const EpisodePolicy p = episode_policy().get();
    // ck-2ngg: X is the RULE's answer for THIS episode's declared horizon, or
    // UNARMED. The log says which, in words, because "X=0" read as a threshold
    // is exactly the misreading the armed bit exists to prevent.
    const std::string x_desc =
        p.unavail_exclusion_armed
            ? (std::to_string((long long)p.unavail_exclusion_days) + " game-days")
            : ("UNARMED (" + p.unavail_exclusion_unarmed_reason + ")");
    unavailability().configure(p.unavail_anomaly_wall_s, p.unavail_exclusion_days,
                               p.unavail_exclusion_armed);
    // ── ck-on4u: THE FLUSH PLANNER RESTS AT THE DECLARED SPEED ─────────────
    // Before this, `resting_speed` was a 3 in a header that nothing on the live
    // path could reach, and the plan's restore step asserted it over the
    // pre-registered pin of 2 — once, on the first flush that touched the
    // clock, through the effects vtable and so with no "Action: set_speed" line
    // to find. Here, and nowhere else, is where the declaration becomes the
    // bridge's behaviour. It runs BEFORE pipeline_start(), so the conductor
    // thread never observes the old value and no lock is needed.
    // ── ck-zen6: THE CLOCK STARTS WHEN THE EPISODE STARTS ──────────────
    // PIPELINE MODE ONLY, and the scoping is the whole safety of it: null mode
    // is E0's observer-only FREE RUN and does not take clock ownership, so
    // gating it on a declaration would freeze the baseline. Asked of the mode
    // rather than of a flag somebody has to remember to set.
    C().sched.set_hold_clock_until_observer(pipeline_owns_clock());
    if (g_logfile) {
        fprintf(g_logfile,
            "[CK3-Bridge][pipeline] clock-start gate: %s. %s\n",
            pipeline_owns_clock() ? "ARMED" : "off",
            pipeline_owns_clock()
                ? "the world rests PAUSED until the episode observer declares; "
                  "bring-up and preflight do not consume game time"
                : "this mode does not own the clock (null = E0's free run)");
        fflush(g_logfile);
    }
    if (!C().sched.set_resting_speed(p.game_speed) && g_logfile) {
        fprintf(g_logfile,
            "[CK3-Bridge][pipeline] WARNING: resting speed %d is not a speed; "
            "the planner keeps %d\n",
            p.game_speed, C().sched.config().resting_speed);
    }
    if (g_logfile) {
        fprintf(g_logfile,
            "[CK3-Bridge][pipeline] episode: mode=%s id='%s' "
            "unavail_anomaly_wall_s=%lld X=%s (fraction=%g horizon=%s..%s "
            "%lld game-days) (%s)\n",
            episode_mode_name(p.mode), p.episode_id.c_str(),
            (long long)p.unavail_anomaly_wall_s,
            x_desc.c_str(), p.unavail_exclusion_fraction,
            p.horizon_start_game_date.empty() ? "(undeclared)"
                                              : p.horizon_start_game_date.c_str(),
            p.horizon_end_game_date.empty() ? "(undeclared)"
                                            : p.horizon_end_game_date.c_str(),
            (long long)p.horizon_game_days,
            p.mode == EpisodeMode::Experiment
                ? "EXPERIMENT — claims are PINNED, ai_ctrl+0x28 will not be "
                  "re-enabled for a claimed character, and a gap longer than "
                  "the WALL-CLOCK anomaly gate is recorded as the GAME-DAYS it "
                  "spanned"
                : "free play — Q5 orphan behaviour unchanged");
        // ck-on4u: the speed the planner will rest at, and WHERE IT CAME FROM.
        // "declared" and "the pre-registered default" are the same number today
        // and a different fact always; a log that prints only the number cannot
        // tell an operator which one they are looking at.
        fprintf(g_logfile,
            "[CK3-Bridge][pipeline] resting game speed=%d (%s; %s=%s)\n",
            C().sched.config().resting_speed,
            p.game_speed_declared ? "DECLARED by the harness"
                                  : "pre-registered default — nothing declared one",
            kGameSpeedEnv,
            p.game_speed_declared ? getenv(kGameSpeedEnv) : "(unset)");
        // ck-aqio. KEYED ON present && !declared, NOT on a non-empty refusal:
        // under the byte rule the empty string IS a refusal (set to nothing),
        // and a guard on `!refused.empty()` skips exactly that case.
        //
        // AND THE RAW TEXT IS RENDERED, NEVER ECHOED. With CK3_EPISODE_GAME_
        // SPEED=$'2\r' the old %s printed the carriage return, the cursor
        // returned, and the rest of the line overwrote what came before it —
        // the operator read "CK3_EPISODE_GAME_SPEED='2' is not a speed in 1..5":
        // a perfectly valid value refused for no visible reason, in the one
        // message that exists to save them. The sealed bundle was honest all
        // along (json escapes it); only the log lied, which is the worse half to
        // get wrong, because the log is what they are holding in that second.
        if (p.game_speed_present && !p.game_speed_declared) {
            const std::string shown = render_bytes(p.refused_game_speed_env);
            fprintf(g_logfile,
                "[CK3-Bridge][pipeline] WARNING: %s='%s' (%zu byte%s) is not "
                "exactly one of \"1\"..\"5\" and was REFUSED; the pre-registered "
                "default %d stands. The rule is the RAW BYTES: no surrounding "
                "space, no leading zero, no sign.\n",
                kGameSpeedEnv, shown.c_str(),
                p.refused_game_speed_env.size(),
                p.refused_game_speed_env.size() == 1 ? "" : "s",
                kRegisteredGameSpeed);
        }
        if (p.game_speed > kGameSpeedSinkCap) {
            // A DECLARATION THE SINK CANNOT HONOUR. game_set_speed caps every
            // write at 3, unconditionally and regardless of caller, so the world
            // will run at 3 while the bundle declares 4 or 5 and §7.3 records
            // speed_mismatch for the whole horizon. Say it at init, where it is
            // still cheap, rather than letting the episode discover it at seal.
            fprintf(g_logfile,
                "[CK3-Bridge][pipeline] WARNING: declared game speed %d is above "
                "game_set_speed's unconditional cap of %d — every write will land "
                "as %d and the episode will read as speed_mismatch.\n",
                p.game_speed, kGameSpeedSinkCap, kGameSpeedSinkCap);
        }
        // A RETIRED KNOB THAT SILENTLY DOES NOTHING IS A TRAP. Say so.
        if (!p.ignored_x_days_env.empty()) {
            fprintf(g_logfile,
                "[CK3-Bridge][pipeline] WARNING: CK3_UNAVAIL_X_DAYS='%s' is "
                "RETIRED (ck-2ngg) and was IGNORED. X is 5%% of the DECLARED "
                "horizon; set CK3_EPISODE_HORIZON_START/_END, and "
                "CK3_UNAVAIL_EXCLUSION_FRACTION if the fraction itself must "
                "move.\n", p.ignored_x_days_env.c_str());
        }
        if (!p.refused_exclusion_fraction_env.empty()) {
            fprintf(g_logfile,
                "[CK3-Bridge][pipeline] WARNING: CK3_UNAVAIL_EXCLUSION_FRACTION"
                "='%s' is not a number in (0,1] and was REFUSED; the published "
                "default %g stands.\n",
                p.refused_exclusion_fraction_env.c_str(),
                p.unavail_exclusion_fraction);
        }
        if (p.mode == EpisodeMode::Experiment && !p.unavail_exclusion_armed) {
            fprintf(g_logfile,
                "[CK3-Bridge][pipeline] WARNING: the unavailability exclusion is "
                "UNARMED (%s) — no X will be judged and the seal marks "
                "unavailability_over_x NOT EVALUABLE.\n",
                p.unavail_exclusion_unarmed_reason.c_str());
        }
        fflush(g_logfile);
    }
}

void pipeline_note_session_activity(const Session* session) {
    // EXPERIMENT MODE ONLY, tested FIRST and lock-free: §2.7's covariate is
    // per-EPISODE, free play has no episode, and this runs on the reader thread
    // for every envelope. In free play it is one relaxed atomic load — which is
    // what keeps ck-6kvq.9's "legacy is indistinguishable, in cost too" true.
    if (!episode_is_experiment()) return;
    // §2.7: "any submission OR read resets it". OBSERVER TRAFFIC NEVER COUNTS —
    // an observe session is never registered with the ledger, so this is
    // belt-and-braces, and the ledger ignores unknown ids anyway.
    if (!session || !session->is_control()) return;
    const int64_t day = current_game_day();
    if (day == INT64_MIN) return;   // no world loaded: there is no game-day yet
    // ck-ijfa: BOTH UNITS. The wall stamp turns each gap into a record with a
    // game-day span AND a wall-second span, so the covariate's calibration can
    // be CHECKED instead of assumed. It changes no gate.
    unavailability().note_activity(session->session_id, day, mono_ms());
}

// ============================================================================
// The real FlushEffects — the ONLY place in the flush machinery that touches
// the engine.
// ============================================================================
namespace {

class BridgeFlushEffects : public FlushEffects {
public:
    bool pause() override   { return do_pause_game("{}"); }
    bool unpause() override { return do_unpause_game("{}"); }
    bool set_speed(int spd) override {
        char p[64];
        snprintf(p, sizeof(p), "{\"speed\":%d}", spd);
        return do_set_speed(p);
    }

    uint64_t now_ms() const override { return mono_ms(); }

    bool routes_to_drain(const BufferedCall& call) const override {
        return action_routes_to_drain(call.action.c_str());
    }

    std::string serve(const BufferedCall& call) override {
        PendingCall pc;
        if (!take_payload(call.ticket, &pc)) {
            return "{\"success\":false,\"reason\":\"ticket_payload_lost\",\"phase\":\"serve\"}";
        }

        // Reinstall the submitting session so ownership / observe-mode checks
        // at the dispatch choke point see the SAME context they would have seen
        // on the reader thread.
        ClientContext ctx;
        ctx.session = g_session_registry.find(pc.session_id);
        ClientContextGuard guard(&ctx);

        // ck-6kvq.12: publish the applying flush to the handler. serve_one()
        // has already called mark_running(ticket, flush_id), so the ticket
        // carries the authoritative id — the conductor does not have to be
        // trusted to pass it down a second path.
        Ticket tk;
        ScopedFlushId flush_guard(C().tickets.find(call.ticket, &tk) ? tk.flush_id : 0);

        std::string response;
        g_in_protected_region = true;
        if (sigsetjmp(g_recovery_point, 1) == 0) {
            dispatch_action_with_response(pc.action.c_str(), pc.inner_json.c_str(), &response);
        } else {
            response = "{\"success\":false,\"reason\":\"memory_access_fault\","
                       "\"phase\":\"serve\"}";
            if (g_logfile) fprintf(g_logfile,
                "[CK3-Bridge][pipeline] SIGSEGV serving %s (ticket=%llu)\n",
                pc.action.c_str(), (unsigned long long)call.ticket);
        }
        g_in_protected_region = false;
        return response;
    }

    bool enqueue_for_drain(const BufferedCall& call) override {
        // ck-6kvq.19: the envelope carries the BRIDGE-MINTED TICKET, so the
        // game thread can settle this call whether or not its client ever sent
        // a request_id, and two calls that share one request_id cannot
        // collide. The payload stays in the registry until the drain reports
        // back; settle() erases both halves together.
        std::string envelope;
        if (!C().drain.hand_off(call.ticket, &envelope)) return false;
        return enqueue_action_envelope(envelope);
    }

    ExecutionStamp stamp(const BufferedCall& call) override {
        ExecutionStamp st;
        st.tick_executed = g_tick_id.load(std::memory_order_relaxed);
        st.game_date     = current_game_date();
        st.character_id  = 0;
        Session* s = g_session_registry.find(call.session_id);
        if (s) {
            std::lock_guard<std::mutex> lk(s->mutex);
            st.character_id = s->primary_character_id;
        }
        // ck-jg2b: DO NOT BORROW g_controlled_primary.
        //
        // This used to fall back to the global controlled primary whenever the
        // acting session held nothing — stamping SOMEONE ELSE'S character onto
        // this session's action. The record then named an actor that had not
        // acted, and it named it plausibly: a real id, a real character, in the
        // field a reader trusts to say who did this.
        //
        // A zero with a REASON is a fact. A borrowed id is a fabrication that
        // reads as evidence, and it is the kind that survives review because
        // nothing about it looks wrong.
        if (st.character_id == 0)
            st.attribution_reason = s ? "claimless_session" : "session_gone";
        return st;
    }

    void emit_result(const BufferedCall& call, const std::string& result_json) override {
        emit_keyed_result(call.ticket, call.action, result_json);
    }

    void note_clock_drift(const FlushPlan& plan) override {
        C().clock_drift_events.fetch_add(1, std::memory_order_relaxed);
        if (g_logfile) {
            fprintf(g_logfile,
                "[CK3-Bridge][pipeline] clock_drift_observed flush_id=%llu "
                "(all-ANY flush found the clock paused; NOT correcting it — §2.3)\n",
                (unsigned long long)plan.flush_id);
            fflush(g_logfile);
        }
    }

    void note_flush_record(const FlushPlan& plan, uint64_t wall_ms_elapsed,
                           uint64_t paused_ms) override {
        C().flush_id.store(plan.flush_id, std::memory_order_relaxed);
        C().last_flush_ms.store(plan.wall_ms, std::memory_order_relaxed);
        C().last_flush_wall_ms.store(wall_ms_elapsed, std::memory_order_relaxed);
        C().last_paused_ms.store(paused_ms, std::memory_order_relaxed);
        if (plan.unclassified)
            C().unclassified_total.fetch_add(plan.unclassified, std::memory_order_relaxed);
    }

    // Take a ticket's payload out of the pending map (inline service consumes
    // it; drain service leaves it until the drain reports back).
    static bool take_payload(uint64_t ticket, PendingCall* out) {
        return C().drain.take(ticket, out);
    }

    // Emit one result frame to the client(s).
    //
    // §8.2 BACKWARD COMPATIBILITY BY CONSTRUCTION: the frame is keyed by the
    // client's OWN request_id, the same key it already blocks on today
    // (mcp_server.py:954 mints one for every call). A ticket-ignorant client
    // therefore still completes every call in pipeline mode; it just
    // experiences the call as slower, bounded by the flush period. The ticket
    // id rides along as an EXTRA field, which such a client ignores.
    //
    // ck-6kvq.18: the frame is also TYPED, `"type":"result"`. It used to carry
    // no type at all, so a client that sent no request_id received a frame with
    // neither a type nor a key — literally unroutable, and impossible to
    // skip-list the way tick_start/observation/notification are skip-listed.
    // The type is appended LAST, after the handler's own keys are spliced in,
    // so a handler that one day emits its own top-level "type" cannot shadow
    // the frame type (JSON last-key-wins in rapidjson and in python json).
    static void emit_keyed_result(uint64_t ticket, const std::string& action,
                                  const std::string& result_json) {
        Ticket t;
        const bool have = C().tickets.find(ticket, &t);
        std::string rid = have ? t.request_id : std::string();

        std::string frame = "{";
        if (!rid.empty()) {
            frame += "\"request_id\":\"";
            frame += json_escape(rid);
            frame += "\",";
        }
        frame += build_envelope_clock_fragment();
        // The unforgeable attribution spine (§2.8), minted by the bridge and
        // never reported by the agent. ck-0lph moved the SHAPE into
        // pipeline_frames.h so the drain's reply can mint the same one — this
        // was the only place it existed, which is why no game-thread verb ever
        // carried it. Byte-identical to what this emitted before.
        frame_append_attribution(frame, ticket, have ? &t : nullptr,
                                 [](const std::string& v) { return json_escape(v); });
        // The splice + close lives in pipeline_frames.h so it can be proved on
        // a host: it rewrites EVERY pipeline reply, and pipeline.cpp cannot be
        // linked without the game. See test_pipeline_frames.
        frame_append_result_body(frame, result_json, json_escape(action), "result");
        send_observation(frame.c_str(), frame.size());
    }
};

BridgeFlushEffects& effects() {
    static BridgeFlushEffects fx;
    return fx;
}

}  // namespace

// ============================================================================
// Ingress
// ============================================================================
namespace {

// Refuse, typed, on the same envelope the client sent.
//
// ck-6kvq.18: `"type":"refused"`. Like the result frame this used to be
// untyped, so a client that sent no request_id could neither route it nor
// skip-list it. It is built entirely here — nothing is spliced in — so the
// type can sit at the head.
void emit_refusal(const std::string& request_id, const char* action,
                  const char* reason, const char* hint, uint64_t retry_after_ms) {
    std::string frame = "{\"type\":\"refused\",";
    if (!request_id.empty()) {
        frame += "\"request_id\":\"";
        frame += json_escape(request_id);
        frame += "\",";
    }
    frame += build_envelope_clock_fragment();
    frame += ",\"accepted\":false,\"success\":false,\"action\":\"";
    frame += json_escape(action ? action : "");
    frame += "\",\"reason\":\"";
    frame += reason;
    frame += "\"";
    if (retry_after_ms) {
        frame += ",\"retry_after_ms\":";
        frame += std::to_string(retry_after_ms);
    }
    if (hint && hint[0]) {
        frame += ",\"hint\":\"";
        frame += json_escape(hint);
        frame += "\"";
    }
    frame += "}";
    send_observation(frame.c_str(), frame.size());
}

// The acceptance frame (§2.4). NOTE the key is `for_request_id`, NOT
// `request_id`: a legacy client routes by `request_id` and would otherwise
// resolve its future on the ACK instead of on the result. A ticket-aware
// client correlates on `for_request_id`. THIS MUST NOT CHANGE.
//
// ck-6kvq.18 — WHAT THAT DOES AND DOES NOT BUY. The asymmetry protects exactly
// one kind of client: one that routes STRICTLY by request_id and discards
// anything else (mcp_server.py, bridge/harness/client.py:196,
// scripts/overlay_poller.py:1079 — all verified clean). It does NOT protect a
// client that returns the first frame that is not a known push, or that treats
// "no request_id" as "close enough". Three shipped clients did exactly that and
// read this ack AS their result, one frame out of phase forever, silently.
// The ack has always carried `"type":"accepted"`; the fix is on the client side
// (skip it), plus typing the frames that had no type at all so the skip list
// can be exhaustive. See scripts/test_pipeline_client_correlation.py.
void emit_accepted(const std::string& request_id, const SubmitResult& sr,
                   const char* action) {
    std::string frame = "{\"type\":\"accepted\",\"accepted\":true,\"ticket\":\"";
    frame += ticket_id_to_string(sr.ticket);
    frame += "\",\"polarity\":\"";
    frame += polarity_name(sr.polarity);
    frame += "\",\"flush_eta_ms\":";
    frame += std::to_string(sr.flush_eta_ms);
    frame += ",\"action\":\"";
    frame += json_escape(action ? action : "");
    frame += "\"";
    if (!request_id.empty()) {
        frame += ",\"for_request_id\":\"";
        frame += json_escape(request_id);
        frame += "\"";
    }
    frame += "}";
    send_observation(frame.c_str(), frame.size());
}

// ---- `check` (ANY-polarity; NEVER waits for a flush) ----------------------
void handle_check(const rapidjson::Value& inner, const std::string& request_id) {
    std::string frame = "{";
    if (!request_id.empty()) {
        frame += "\"request_id\":\"";
        frame += json_escape(request_id);
        frame += "\",";
    }
    frame += build_envelope_clock_fragment();
    const bool check_understood = action_has(inner, "tickets") || action_has(inner, "ticket");
    frame += ",\"action\":\"check\",\"success\":";
    frame += check_understood ? "true" : "false";
    frame += ",\"results\":[";

    bool first = true;
    auto one = [&](const char* tid_str) {
        if (!first) frame += ",";
        first = false;
        const uint64_t id = ticket_id_from_string(tid_str);
        Ticket t;
        if (id == 0 || !C().tickets.find(id, &t)) {
            // A typed refusal, not silence (§2.4 acceptance). An id we have
            // never minted and an id that aged out of the ring are DIFFERENT
            // facts, so they get different reasons.
            frame += "{\"ticket\":\"";
            frame += json_escape(tid_str ? tid_str : "");
            frame += "\",\"state\":\"unknown\",\"reason\":\"unknown_ticket\"}";
            return;
        }
        frame += "{\"ticket\":\"";
        frame += ticket_id_to_string(t.id);
        frame += "\",\"state\":\"";
        frame += ticket_state_name(t.state);
        frame += "\",\"action\":\"";
        frame += json_escape(t.action);
        frame += "\",\"polarity\":\"";
        frame += polarity_name(t.polarity);
        frame += "\",\"flush_id\":";
        frame += std::to_string(t.flush_id);
        frame += ",\"game_date\":\"";
        frame += json_escape(t.game_date_executed);
        frame += "\",\"session_id\":\"";
        frame += json_escape(t.session_id);
        frame += "\",\"character_id\":";
        frame += std::to_string(t.character_id);
        frame += ",\"tick_executed\":";
        frame += std::to_string(t.tick_executed);
        if (!t.request_id.empty()) {
            frame += ",\"request_id\":\"";
            frame += json_escape(t.request_id);
            frame += "\"";
        }
        if (t.state == TicketState::Pending || t.state == TicketState::Running) {
            const uint64_t period = C().sched.config().period_ms;
            const uint64_t since  = mono_ms() - C().sched.last_flush_ms();
            frame += ",\"flush_eta_ms\":";
            frame += std::to_string(since >= period ? 0 : period - since);
        }
        if (!t.result_json.empty()) {
            frame += ",\"result\":";
            frame += t.result_json;
        }
        frame += "}";
    };

    // ck-6kvq.15: the arguments may be flat or under "params" — a real client
    // frames them. Reaching straight for inner["tickets"] is what made every
    // check answer success with an empty result set.
    const bool has_many = action_has(inner, "tickets");
    const bool has_one  = action_has(inner, "ticket");
    if (has_many) {
        const rapidjson::Value& a = action_args(inner, "tickets")["tickets"];
        if (a.IsArray()) {
            for (rapidjson::SizeType i = 0; i < a.Size(); ++i)
                one(a[i].IsString() ? a[i].GetString() : "");
        }
    } else if (has_one) {
        const rapidjson::Value& a = action_args(inner, "ticket")["ticket"];
        if (a.IsString()) one(a.GetString());
    }

    frame += "]";
    // An empty result set is only ever legitimate for an explicitly empty
    // `tickets` array. "I could not find what you asked about" must NEVER
    // render as success with nothing in it — that ambiguity is precisely what
    // let this handler look healthy while being wholly non-functional.
    if (!has_many && !has_one)
        frame += ",\"reason\":\"missing_tickets\",\"hint\":\"check takes "
                 "{tickets:[...]} or {ticket:...}, flat or under params\"";
    frame += "}";
    send_observation(frame.c_str(), frame.size());
}

// ---- `cancel` (best-effort, before the flush that would run it) ------------
void handle_cancel(const rapidjson::Value& inner, const std::string& request_id) {
    // ck-6kvq.15: same params-fallback bug as `check` — every framed cancel
    // was answered unknown_ticket regardless of the ticket's real state.
    const rapidjson::Value& cargs = action_args(inner, "ticket");
    const char* tid = (cargs.HasMember("ticket") && cargs["ticket"].IsString())
                          ? cargs["ticket"].GetString() : "";
    const uint64_t id = ticket_id_from_string(tid);
    Ticket t;
    bool cancelled = false;
    const char* reason = "unknown_ticket";
    if (id != 0 && C().tickets.find(id, &t)) {
        if (t.state == TicketState::Pending) {
            // Withdraw the buffered DESCRIPTOR too, not just the payload.
            // Failing the ticket alone left the call in the flush buffer, where
            // it still consumed one of the flush's bounded paused/tick slots
            // (§2.8's fairness budget, spent on work nobody wants) and still
            // ran through the apply layer's serve path. It is best-effort by
            // design: false here means the flush already claimed it, and the
            // apply layer's Pending->Running guard settles that race by
            // skipping it.
            C().sched.cancel(id);
            C().tickets.fail(id, "{\"success\":false,\"reason\":\"cancelled\","
                                 "\"phase\":\"buffered\"}", mono_ms());
            C().drain.drop(id);
            cancelled = true;
            reason = "cancelled";
        } else {
            reason = "already_running";
        }
    }
    std::string frame = "{";
    if (!request_id.empty()) {
        frame += "\"request_id\":\"";
        frame += json_escape(request_id);
        frame += "\",";
    }
    frame += build_envelope_clock_fragment();
    frame += ",\"action\":\"cancel\",\"success\":";
    frame += cancelled ? "true" : "false";
    frame += ",\"ticket\":\"";
    frame += json_escape(tid);
    frame += "\",\"reason\":\"";
    frame += reason;
    frame += "\"}";
    send_observation(frame.c_str(), frame.size());
}

// ---- `get_action_polarity` — THE polarity authority, published -------------
//
// Mode-independent on purpose: the gateway must be able to sync its table (and
// detect a disagreement) BEFORE anyone flips the mode. It reads no game memory
// and takes no lock, so it is safe in any clock state and in any mode.
void handle_get_action_polarity(const rapidjson::Value& inner,
                                const std::string& request_id) {
    std::string frame = "{";
    if (!request_id.empty()) {
        frame += "\"request_id\":\"";
        frame += json_escape(request_id);
        frame += "\",";
    }
    frame += build_envelope_clock_fragment();
    frame += ",\"action\":\"get_action_polarity\",\"success\":true,\"authority\":\"bridge\"";

    // Single-action form.
    if (inner.HasMember("action_name") && inner["action_name"].IsString()) {
        const char* a = inner["action_name"].GetString();
        frame += ",\"action_name\":\"";
        frame += json_escape(a);
        frame += "\",\"registered\":";
        frame += is_registered_action(a) ? "true" : "false";
        frame += ",\"polarity\":\"";
        frame += polarity_name(resolve_polarity(classify_polarity(a)));
        frame += "\",\"routes_to_drain\":";
        frame += action_routes_to_drain(a) ? "true" : "false";
        frame += "}";
        send_observation(frame.c_str(), frame.size());
        return;
    }

    // Whole-table form, generated from the SAME action_registry.inc table that
    // generates the dispatch chain, so the published table cannot drift from
    // the surface it describes.
    frame += ",\"polarity\":{";
    bool first = true;
#define BRIDGE_ACTION(NAME, HANDLER)                                     \
    do {                                                                 \
        if (!first) frame += ",";                                        \
        first = false;                                                   \
        frame += "\"" NAME "\":\"";                                      \
        frame += polarity_name(resolve_polarity(classify_polarity(NAME))); \
        frame += "\"";                                                   \
    } while (0);
#include "action_registry.inc"
#undef BRIDGE_ACTION
    frame += "}}";
    send_observation(frame.c_str(), frame.size());
}

}  // namespace

// ============================================================================
// pipeline_handle_envelope — the reader thread's first stop
// ============================================================================
bool pipeline_handle_envelope(const std::string& line, Session* session) {
    const bool active = pipeline_active();

    // LEGACY FAST EXIT. One relaxed atomic load plus a substring test, and the
    // reader thread's ladder runs with no extra JSON parse. `legacy` is the
    // default (G11) and must be indistinguishable from the pre-change bridge in
    // cost as well as in behaviour.
    if (!active && line.find("get_action_polarity") == std::string::npos) return false;

    rapidjson::Document env;
    env.Parse(line.c_str());
    if (env.HasParseError() || !env.IsObject()) return false;
    if (!env.HasMember("actions") || !env["actions"].IsArray()) return false;
    const rapidjson::Value& actions = env["actions"];
    if (actions.Empty()) return false;

    // --- Mode-independent introspection ------------------------------------
    // Only `get_action_polarity` is answered in legacy mode; every other
    // pipeline verb is a pipeline-mode frame and falls through to the legacy
    // ladder (where it gets the usual typed unknown_action).
    bool all_introspection = true;
    for (rapidjson::SizeType i = 0; i < actions.Size(); ++i) {
        const rapidjson::Value& in = actions[i];
        if (!in.IsObject() || !in.HasMember("action") || !in["action"].IsString()) {
            all_introspection = false; break;
        }
        if (strcmp(in["action"].GetString(), "get_action_polarity") != 0) {
            all_introspection = false; break;
        }
    }
    if (all_introspection) {
        for (rapidjson::SizeType i = 0; i < actions.Size(); ++i) {
            const rapidjson::Value& in = actions[i];
            std::string rid;
            if (in.HasMember("request_id") && in["request_id"].IsString())
                rid = in["request_id"].GetString();
            handle_get_action_polarity(in, rid);
        }
        return true;
    }

    if (!active) return false;   // LEGACY: the ladder below runs untouched.

    // SYNCHRONOUS FAST PATH (§2.4). An envelope whose every member is
    // ANY-polarity does not need the clock and is served inline by the existing
    // ladder. The ENTIRE ops ladder polls these — ck3_game_selfheal.sh,
    // operator_page.sh, overlay_poller.py and run_monitor.py all depend on
    // get_tick_info answering synchronously, and making it async would break
    // every watchdog on the host for no gain.
    //
    // The test is over the WHOLE envelope, not per-action: the legacy ladder
    // consumes an envelope whole or not at all, so handing it a half-buffered
    // batch would double-answer the rest. A mixed envelope is therefore
    // buffered entirely; its ANY members still cost the world nothing (they are
    // served in the flush's clock-silent ServeAny step).
    bool all_any = true;
    for (rapidjson::SizeType i = 0; i < actions.Size(); ++i) {
        const rapidjson::Value& in = actions[i];
        if (!in.IsObject() || !in.HasMember("action") || !in["action"].IsString()) {
            all_any = false; break;
        }
        const char* a = in["action"].GetString();
        if (!is_registered_action(a) ||
            resolve_polarity(classify_polarity(a)) != Polarity::ANY ||
            (pipeline_owns_clock() && is_clock_verb(a))) {
            all_any = false; break;
        }
    }
    if (all_any) return false;

    const std::string session_id = session ? session->session_id : std::string("anon");

    for (rapidjson::SizeType i = 0; i < actions.Size(); ++i) {
        const rapidjson::Value& in = actions[i];
        if (!in.IsObject() || !in.HasMember("action") || !in["action"].IsString()) continue;
        const char* action = in["action"].GetString();

        std::string rid;
        if (in.HasMember("request_id") && in["request_id"].IsString())
            rid = in["request_id"].GetString();

        // `check` and `cancel` are ANY-polarity and never wait for a flush.
        if (strcmp(action, "check") == 0)  { handle_check(in, rid);  continue; }
        if (strcmp(action, "cancel") == 0) { handle_cancel(in, rid); continue; }

        // `turn_done {checkpoint}` was RETIRED 2026-09-12 with the G3'
        // reversal (§2.10). There is no gate for it to release, and session
        // activity for the unavailability covariate is derived from the
        // bridge's own attributed call record — no explicit verb is needed. A
        // client that still sends it falls through to the usual typed
        // unknown_action rather than being silently accepted.

        // §2.9 CONTAINMENT: a latched pulse leak means this episode's validity
        // is already gone. Refuse further control-session work rather than
        // adding more contaminated actions to a bundle nobody can use.
        if (pipeline_episode_aborted()) {
            emit_refusal(rid, action, "episode_aborted",
                         "A pulse leak was detected (pipeline.pulse_leak); the "
                         "episode's validity is gone and the bridge is refusing "
                         "further control-session work. See ck-6kvq.1's abort "
                         "ladder, reason pulse_leak.",
                         0);
            continue;
        }

        // G1: the bridge owns the clock in pipeline mode.
        if (pipeline_owns_clock() && is_clock_verb(action)) {
            emit_refusal(rid, action, "clock_owned_by_pipeline",
                         "In pipeline mode the bridge's flush loop owns the clock; "
                         "a session cannot pause, unpause or set the speed. "
                         "Set CK3_PIPELINE_MODE=legacy to drive the clock directly.",
                         0);
            continue;
        }

        if (!is_registered_action(action)) {
            emit_refusal(rid, action, "unknown_action", nullptr, 0);
            continue;
        }

        // SYNCHRONOUS FAST PATH (§2.4). ANY-polarity calls do not need the
        // clock, and the ENTIRE ops ladder polls them — ck3_game_selfheal.sh,
        // operator_page.sh, overlay_poller.py and run_monitor.py all depend on
        // get_tick_info answering synchronously. Making them async would break
        // every watchdog on the host for no gain.
        // Buffer it. (An all-ANY envelope already returned above; an ANY
        // member of a MIXED envelope is buffered with the rest and served in
        // this flush's clock-silent ServeAny step.)
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        in.Accept(w);

        CallRequest req;
        req.session_id = session_id;
        req.action     = action;
        req.polarity   = classify_polarity(action);

        const uint64_t now = mono_ms();
        SubmitResult sr = C().sched.submit(req, now);
        if (!sr.accepted) {
            // Back-pressure instead of silent loss (§2.8).
            emit_refusal(rid, action, sr.reason.c_str(),
                         "The pipeline buffer is full; retry after retry_after_ms.",
                         sr.retry_after_ms);
            continue;
        }

        PendingCall pc;
        pc.ticket     = sr.ticket;
        pc.session_id = session_id;
        pc.request_id = rid;
        pc.action     = action;
        pc.inner_json.assign(sb.GetString(), sb.GetSize());
        C().drain.put(sr.ticket, pc);
        C().tickets.open(sr.ticket, session_id, rid, action, sr.polarity, now);
        emit_accepted(rid, sr, action);
    }

    // Nudge the conductor: an all-ANY or size-bound buffer flushes early.
    C().wake_cv.notify_one();
    return true;
}

// ============================================================================
// The drain hook — a ticket handed to the game thread is settled BY the game
// thread, never by the mere fact of having been enqueued.
// ============================================================================
// ck-0lph. Build the attribution spine for a ticket the game-thread drain has
// just settled, so the drain's OWN reply — the only reply such a call ever
// gets — carries what every inline-serviced call already carried.
//
// Returns false when the ticket did not resolve. It does NOT then hand back a
// spine of empty strings: at this call site that would be indistinguishable
// from a real record for a session with no id, and the caller has somewhere
// honest to put the absence instead.
bool pipeline_attribution_fragment(uint64_t ticket, std::string* out) {
    if (!out || ticket == 0 || !pipeline_active()) return false;
    Ticket t;
    if (!C().tickets.find(ticket, &t)) return false;
    out->clear();
    frame_append_attribution(*out, ticket, &t,
                             [](const std::string& v) { return json_escape(v); });
    return true;
}

void pipeline_note_drain_result(uint64_t envelope_ticket, const char* request_id,
                                const char* action_type,
                                const std::string& response_json) {
    if (!pipeline_active()) return;

    // ck-6kvq.19: correlate by the bridge-minted ticket the conductor stamped
    // on the envelope. NOT by request_id — that field is optional on the wire
    // and not required to be unique, and keying on it is exactly why a call
    // from a request_id-less client ran and then never settled. `request_id`
    // is carried only for the trace below.
    (void)request_id;
    if (envelope_ticket == 0) return;          // not a conductor envelope
    if (!C().drain.settle(envelope_ticket)) return;  // not ours / already settled
    const uint64_t ticket = envelope_ticket;

    const uint64_t now = mono_ms();
    BufferedCall bc;
    bc.ticket = ticket;
    bc.action = action_type ? action_type : "";

    if (response_json.empty()) {
        C().tickets.fail(ticket,
            "{\"success\":false,\"reason\":\"game_thread_no_reply\",\"phase\":\"drain\"}",
            now);
    } else {
        Session* s = nullptr;
        Ticket t;
        uint32_t char_id = 0;
        bool ticket_known = false;
        if (C().tickets.find(ticket, &t)) {
            ticket_known = true;
            s = g_session_registry.find(t.session_id);
            if (s) { std::lock_guard<std::mutex> lk(s->mutex); char_id = s->primary_character_id; }
        }
        // ck-jg2b: the same borrow, on the drain-settled path. Same rule.
        std::string reason;
        if (char_id == 0)
            reason = (ticket_known && s) ? "claimless_session" : "session_gone";
        C().tickets.complete(ticket, response_json,
                             C().flush_id.load(std::memory_order_relaxed),
                             char_id,
                             g_tick_id.load(std::memory_order_relaxed),
                             current_game_date(), now, reason);
    }
    // The drain already emitted its own reply (bridge_init.cpp), keyed by the
    // client's request_id when it sent one — which is exactly what a legacy
    // client is waiting on (§8.2). We do NOT emit a second copy; a ticket-aware
    // client collects the same result via `check`.
}

// ============================================================================
// Session lifecycle
// ============================================================================
void pipeline_declare_episode_observer(const char* session_id,
                                       const char* client_name,
                                       Session::Mode minted_mode) {
    if (!session_id || !*session_id) return;
    // ck-m3n5: THE APPLIER HAS NO POLICY. The whole decision — mode floor first,
    // then first-wins — is episode_observer_decision() in session.cpp, pure and
    // driven directly by a host probe. This function's only job is to act on it
    // and to say out loud which way it went.
    //
    // It is also the ONLY caller of set_privileged_reader, so a privilege this
    // refuses cannot be conferred by any route. That is what makes the floor a
    // property of the PRIVILEGE rather than of whoever calls it: the mutant that
    // logs REFUSED and declares anyway now gets nothing, where before it got the
    // read-lane trigger on a control session and passed the lane 9/9.
    const std::string held = C().sched.config().privileged_reader;
    switch (episode_observer_decision(held, session_id, minted_mode)) {
    case ObserverDecision::RefusedByMode:
        if (g_logfile) {
            fprintf(g_logfile,
                "[CK3-Bridge][pipeline] episode_observer REFUSED to session %s "
                "(client '%s'): it is a CONTROL session, and the episode "
                "observer is the READER. The session stands; only the privilege "
                "is refused. If this is the meter lane it is handshaking in the "
                "wrong mode; if it is the driver the declaration is on the wrong "
                "client.\n",
                session_id, client_name ? client_name : "?");
            fflush(g_logfile);
        }
        return;
    case ObserverDecision::RefusedByFirstWins:
        if (held != session_id && g_logfile) {
            fprintf(g_logfile,
                "[CK3-Bridge][pipeline] episode observer ALREADY DECLARED by %s; "
                "refusing the privilege to %s (client '%s'). The session stands; "
                "only the privilege is refused — two privileged readers would be "
                "two things driving the cadence.\n",
                held.c_str(), session_id, client_name ? client_name : "?");
            fflush(g_logfile);
        }
        return;
    case ObserverDecision::Confer:
        break;
    }
    C().sched.set_privileged_reader(session_id);
    if (g_logfile) {
        fprintf(g_logfile,
            "[CK3-Bridge][pipeline] EPISODE OBSERVER declared: %s (client '%s'). "
            "Its submission now serves the READ lane early; buffered WRITES stay "
            "on the cadence.\n",
            session_id, client_name ? client_name : "?");
        fflush(g_logfile);
    }
}

std::string pipeline_episode_observer() {
    return C().sched.config().privileged_reader;
}

void pipeline_note_episode_open(const char* episode_id) {
    if (!episode_id || !*episode_id) return;   // a declaration with no episode

    // STORE THE ID FIRST, AND BEFORE THE HOLD CHECK (ck-kc32). Two independent
    // things happen on a declaration: the bridge LEARNS WHICH EPISODE IT IS IN,
    // and — if the clock hold is armed — it RELEASES THE CLOCK. Only the second
    // is once-per-episode. Putting the store after the guard below would publish
    // the id only when the hold happened to be armed, so legacy and free-play
    // launches (never armed) and every declaration after the first would learn
    // the id and discard it — the same defect this bead fixes, one branch over.
    episode_policy().set_episode_id(episode_id);

    // ck-2r46. THE CARRIER LEARNS ITS FILENAME HERE AND NOWHERE ELSE. It
    // initialises from the environment before this point, when the id is still
    // empty — which is why it used to open board-unnamed.jsonl and why a
    // relaunch could never rehydrate. Adopting (never truncating) at the one
    // moment the id becomes known is what makes the file per-episode.
    board_carrier_adopt_episode_id(episode_id);

    // ck-y6fp: REMEMBER WHO OPENED IT, from the dispatch context the choke point
    // already installs (ipc_server.cpp and the flush path both wrap the handler
    // in a ClientContextGuard). No handler signature changes and no new
    // parameter threaded through the dispatch table: the acting session is
    // already ambient at this point, and a wider refactor to carry what is
    // already here would have been the expensive wrong answer.
    if (g_current_client_context && g_current_client_context->session) {
        std::lock_guard<std::mutex> lk(C().episode_owner_mu);
        C().episode_owner_session = g_current_client_context->session->session_id;
        // ck-tpvn: A DECLARATION CANCELS A PENDING GRACE, BECAUSE IT
        // ESTABLISHES AN OWNER — which is the question the grace actually asks.
        // NOT "was the declaration ACCEPTED": the bridge is never told.
        // do_declare_save_identity computes rec.decision, writes it into the
        // REPLY, and calls this unconditionally, so a refused declaration is
        // indistinguishable here from an accepted one (ck-9rto asked exactly
        // this, and was right that it cannot be keyed on the decision).
        //
        // IT DOES NOT NEED TO BE, and the invariant is why: the cancel is a
        // TRANSFER of ownership, not an escape hatch. The line above records the
        // declaring session as the new owner, so ITS departure opens a NEW
        // grace. The world therefore cannot run unattended for longer than one
        // grace whoever declares and whatever the decision — driven in
        // clock_release_probe (refused_shape_cancels_grace /
        // and_the_NEW_owner_leaving_REGRACES / armed_after_regrace).
        C().episode_owner_gone_since_ms = 0;
    }

    if (!C().sched.config().hold_clock_until_observer) return;  // already open
    // DISARM THE HOLD. The planner's next flush falls through to its ordinary
    // restore — unpause, rest at the declared speed — and the conductor's
    // one-shot below starts the world even if no flush has work.
    C().sched.set_hold_clock_until_observer(false);
    if (g_logfile) {
        fprintf(g_logfile,
            "[CK3-Bridge][pipeline] EPISODE OPEN ('%s'): releasing the clock "
            "hold. The world runs from here at the declared resting speed. "
            "Bring-up and preflight cost no game time.\n", episode_id);
        fflush(g_logfile);
    }
}

void pipeline_register_session(const char* session_id) {
    if (!pipeline_active() || !session_id || !session_id[0]) return;
    C().sched.register_session(session_id);
}

void pipeline_unregister_session(const char* session_id) {
    if (!pipeline_active() || !session_id || !session_id[0]) return;
    C().sched.unregister_session(session_id);
}

// ---- §2.7 unavailability ledger: session lifecycle (mode-independent) ------
void pipeline_note_session_open(const Session* session) {
    if (!episode_is_experiment()) return;             // per-episode only
    if (!session || !session->is_control()) return;   // observers never count
    const int64_t day = current_game_day();
    if (day == INT64_MIN) return;
    unavailability().note_session(session->session_id, day, mono_ms());   // ck-ijfa
}

void pipeline_note_session_detached(const char* session_id) {
    if (!session_id || !session_id[0]) return;
    unavailability().note_attached(session_id, false);
}

void pipeline_note_session_ended(const char* session_id) {
    if (!session_id || !session_id[0]) return;
    unavailability().note_session_ended(session_id);
    pipeline_note_episode_owner_gone(session_id, "session_ended");
}

// ck-j6lk: THE NON-PARTICIPANT'S EXIT. Identical to the above in every respect but
// ONE — the ledger FORGETS the record instead of marking it ended — because that one
// difference is the entire defect shakedown 3 exposed. The bridge destroyed both
// driver helpers there (its log says so twice) and unavailability_game_days still
// reported both, ended, each with an open 364-day span against an X of 18.
//
// THE OWNER-GONE NOTIFICATION IS KEPT, WITH THE SAME LITERAL. Not a new `why` value:
// `session_lost` is a member of run_summary.schema.json's closed
// termination.reason enum, so inventing `session_forgotten` would put a third value
// into a vocabulary with a mirror to keep — ck-9rto's shakedown-1 trap, where the
// bridge emitted a reason the vocabulary lacked and the bundle sealed-refused. The
// session genuinely did end; only the record's fate differs, and changing one
// variable is the whole point of a forward fix.
void pipeline_forget_session(const char* session_id) {
    if (!session_id || !session_id[0]) return;
    unavailability().forget_session(session_id);
    pipeline_note_episode_owner_gone(session_id, "session_ended");
}

// ck-y6fp: THE INVERSE OF THE RELEASE, ON THE ONE EVENT THAT MEANS THE EPISODE
// IS NOT COMING BACK.
//
// TWO CALLERS BECAUSE THE BRIDGE HAS TWO WAYS OF LOSING A SESSION, and covering
// only one would leave exactly half the modes leaking:
//
//   EXPERIMENT MODE  a dropped socket DETACHES (claims pinned, token kept) and
//                    becomes LOST only past max_detach_s. So the trigger is the
//                    sweep, and the bridge's EXISTING bound is the bound — no
//                    second timer beside the one it already keeps.
//   OTHERWISE        the session is DESTROYED outright, and
//                    pipeline_note_session_ended fires.
//
// A TRANSIENT DROP CANNOT REACH EITHER. resume_by_token re-attaches the same
// session id before the sweep can name it, which is measured rather than
// assumed in acceptance_session_identity.
//
// AND THIS FIRES AT NORMAL EPISODE END TOO — the driver's client closes in its
// `finally`, so an episode that ran to completion re-arms the hold. THAT IS
// INTENDED, not a side effect: ck-y6fp's invariant is that the world is at the
// baseline unless an episode is OPEN, and a finished episode is not an open one.
// The alternative — leaving the world running after the last episode — is the
// same unbounded drift this bead exists to stop, arriving by the happy path.
namespace {

// ck-tpvn: HOW LONG WE WAIT FOR THE DRIVER TO COME BACK after its session was
// DESTROYED, before pausing the world.
//
// DERIVED FROM RECONNECT LATENCY — a property of the HARNESS CLIENT, which
// reconnects and resumes in every mode. 60 s is generous against a reconnect
// measured in seconds; the cost of being generous is bounded game-time drift,
// and the cost of being tight is a FROZEN LIVE EPISODE, which is the defect this
// bead exists to remove.
//
// EQUAL TO kClockLeakMinSpanMs TODAY BY COINCIDENCE, NOT BY DEFINITION, and the
// first cut of this line wrote `= kClockLeakMinSpanMs` on the argument that both
// answer "how long may the world run unattended". ck-t5bc refused that and was
// right: the backstop does not STOP anything, it DETECTS; and this is not about
// the world being unattended but about how long the DRIVER takes to return. Two
// different physical quantities.
//
// THE TEST THAT SETTLES IT is not how the sentence reads but whether a change
// justified by one silently moves the other in a direction nobody argued for —
// and it does, both ways. A node that shows reconnect taking 90 s must GROW this
// one, and would have made the leak detector 50% slower as a side effect. A
// release/state-update race that turns out to need only 10 s would tighten the
// detector and SILENTLY REINTRODUCE THIS BEAD'S FREEZE. The failure modes are
// not symmetric either: detector too short is a false report, grace too short is
// a frozen live episode. Share the mechanics, parameterise the policy.
constexpr uint64_t kOwnerGraceMs = 60000;

// The re-arm itself, once something has decided the episode is over.
void rearm_hold_locked_free(const char* why, const char* session_id) {
    if (!pipeline_owns_clock()) return;        // nothing to re-arm
    if (C().sched.config().hold_clock_until_observer) return;   // already held
    C().sched.set_hold_clock_until_observer(true);
    if (g_logfile) {
        fprintf(g_logfile,
            "[CK3-Bridge][pipeline] EPISODE OWNER GONE (%s, session %s): "
            "RE-ARMING the clock hold. The world rests PAUSED at the registered "
            "speed from here. A refusal after the release otherwise burns "
            "unbounded game time off the registered start with no episode, "
            "driver or agent.\n", why, session_id);
        fflush(g_logfile);
    }
}

}  // namespace

void pipeline_note_episode_owner_gone(const char* session_id, const char* why) {
    if (!session_id || !session_id[0]) return;
    // ck-tpvn: WHICH WAY DID WE LOSE IT? The two are not the same event.
    //   session_lost  the detached sweep already waited out max_detach_s. The
    //                 bound has BEEN served, so re-arm now.
    //   session_ended the session was DESTROYED this instant. That is not
    //                 evidence the episode ended — the harness client reconnects
    //                 in every mode and will come back under a new id — so start
    //                 a grace window and let a declaration cancel it.
    const bool already_bounded = (why && strcmp(why, "session_lost") == 0);
    {
        std::lock_guard<std::mutex> lk(C().episode_owner_mu);
        if (C().episode_owner_session.empty()) return;
        if (C().episode_owner_session != session_id) return;
        C().episode_owner_session.clear();
        if (!already_bounded) {
            C().episode_owner_gone_since_ms = mono_ms();
            if (g_logfile) {
                fprintf(g_logfile,
                    "[CK3-Bridge][pipeline] episode owner %s DESTROYED (%s): "
                    "clock hold NOT re-armed yet — %llus grace, because the "
                    "harness client reconnects and a destroyed session is not a "
                    "finished episode. A declaration inside the window cancels "
                    "this.\n", session_id, why ? why : "",
                    (unsigned long long)(kOwnerGraceMs / 1000));
                fflush(g_logfile);
            }
            return;
        }
    }
    rearm_hold_locked_free(why ? why : "", session_id);
}

bool pipeline_tick_owner_grace(uint64_t now_ms) {
    {
        std::lock_guard<std::mutex> lk(C().episode_owner_mu);
        const uint64_t since = C().episode_owner_gone_since_ms;
        if (since == 0) return false;                  // nobody pending
        if (now_ms < since) {                          // clock went backwards
            C().episode_owner_gone_since_ms = now_ms;
            return false;
        }
        if (now_ms - since < kOwnerGraceMs) return false;
        C().episode_owner_gone_since_ms = 0;
    }
    rearm_hold_locked_free("owner_grace_expired", "(destroyed)");
    return true;
}

// ============================================================================
// The timer thread (§2.3)
// ============================================================================
namespace {

//: ck-epzs: carried across conductor ticks. Conductor-thread only, so no lock.
ClockLeakState g_clock_leak_state;

void conductor_loop() {
    bridge::register_thread(bridge::ThreadRole::PipelineConductor);   // ck-duv5
    // ck-zen6: ONE SHOT, so a later legitimate unpause is not re-paused
    // behind the episode's back.
    bool observer_gate_paused_once = false;
    bool observer_gate_started_once = false;
    // The watchdog's SIGALRM targets the game thread; block it here so a flush
    // can never absorb it (same posture as the reader thread).
    {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGALRM);
        pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    }

    if (g_logfile) {
        fprintf(g_logfile, "[CK3-Bridge][pipeline] conductor thread started "
                "(period=%llums)\n",
                (unsigned long long)C().sched.config().period_ms);
        fflush(g_logfile);
    }

    while (C().running.load(std::memory_order_relaxed)) {
        {
            std::unique_lock<std::mutex> lk(C().wake_mu);
            C().wake_cv.wait_for(lk, std::chrono::milliseconds(100));
        }
        if (!C().running.load(std::memory_order_relaxed)) break;

        // A flush only makes sense once the game is actually loaded; before
        // that there is no clock to own and no handler that can answer.
        if (get_session_state() != SessionState::ACTIVE) continue;

        // ck-tpvn: close the owner grace if it has expired. Cheap, and it must
        // run on the same cadence as the backstop below — a bound nobody ticks
        // is a bound that never fires.
        pipeline_tick_owner_grace(mono_ms());

        // ── ck-epzs: THE CLOCK-LEAK BACKSTOP ──────────────────────────────
        // ck-y6fp's trigger keys on the session that opened the episode, and is
        // therefore gated on a declaration having been SEEN. This is not: it
        // keys on the flush counter, the one witness not conditioned on the
        // state being violated. It notices when the narrow trigger was never
        // armed — a release with no owner recorded, a future releaser, a path
        // nobody has thought of.
        //
        // SAMPLED HERE AND JUDGED IN A PURE LEAF, so the rule can be driven on
        // a host including the flip (a healthy episode must not trip it).
        {
            ClockLeakSample cls;
            cls.owns_clock       = pipeline_owns_clock();
            cls.hold_armed       = C().sched.config().hold_clock_until_observer;
            const EpisodePolicy epl = episode_policy().get();
            cls.episode_id_empty = epl.episode_id.empty();
            cls.free_play        = (epl.mode == EpisodeMode::FreePlay);
            cls.flush_id         = C().flush_id.load(std::memory_order_relaxed);
            cls.now_ms           = mono_ms();
            if (clock_leak_backstop_step(cls, &g_clock_leak_state)) {
                C().sched.set_hold_clock_until_observer(true);
                if (g_logfile) {
                    fprintf(g_logfile,
                        "[CK3-Bridge][pipeline] CLOCK LEAK BACKSTOP: the flush "
                        "loop has been driving the clock for %llus with NO "
                        "episode (episode_id empty, mode free_play, hold not "
                        "armed). RE-ARMING the hold. ck-y6fp's trigger did not "
                        "fire, so either no owner was recorded at the release "
                        "or the release did not come from a declaration.\n",
                        (unsigned long long)(kClockLeakMinSpanMs / 1000));
                    fflush(g_logfile);
                }
            }
        }

        // ── ck-zen6: THE CLOCK DOES NOT ADVANCE AT LOAD ────────────────
        // The planner rests the world PAUSED while no observer holds the
        // privilege, but a plan only touches the clock on a flush that has WORK
        // — an all-ANY heartbeat touches nothing, by an invariant the planner
        // calls absolute and which exists so the agent hot path cannot perturb
        // the world. So between the world going ACTIVE and the driver's first
        // preflight read, nothing would have stopped it running at ~1.9
        // game-days per second. ONE SHOT, here, closes that window; the planner
        // holds it from then on.
        if (!observer_gate_paused_once &&
            C().sched.config().hold_clock_until_observer &&
            C().sched.config().privileged_reader.empty()) {
            observer_gate_paused_once = true;
            game_pause();
            if (g_logfile) {
                fprintf(g_logfile,
                    "[CK3-Bridge][pipeline] world ACTIVE and no episode observer "
                    "declared: PAUSING. Bring-up and preflight are not the "
                    "episode and must not consume game time; the clock starts "
                    "when the observer declares.\n");
                fflush(g_logfile);
            }
        }

        // ── ck-zen6: THE DECLARATION DRIVES THE CLOCK ──────────────────────
        // The mirror of the one-shot pause above. A plan only acts on a flush
        // that HAS WORK, and while an observer that declares is also an observer
        // that reads, the guarantee should not rest on that: the declaration
        // itself starts the world, at the DECLARED SPEED, from the conductor
        // thread that owns the clock.
        // RELEASED BY THE EPISODE OPENING, AND BY NOTHING ELSE. The observer
        // release was retired (ck-8o5j): the only declarer in the tree is the
        // STANDALONE meter lane, which an operator can start before the driver
        // opens the episode, and its declaration then released the clock through
        // preflight — ck-zen6's own defect by the other door.
        if (observer_gate_paused_once && !observer_gate_started_once &&
            !C().sched.config().hold_clock_until_observer) {
            observer_gate_started_once = true;
            // UNPAUSE ONLY. THE SPEED IS THE PLANNER'S, and deliberately not
            // written here: a direct game_set_speed() from this thread would be
            // a SECOND writer of the clock speed outside the plan, which is
            // exactly what ck-on4u established must not exist — a second writer
            // has no "Action: set_speed" line to find and cannot be attributed.
            // acceptance_resting_speed's D2 row counts those writers and CAUGHT
            // my first version adding one.
            //
            // The planner has already driven the paused world to the declared
            // resting speed (its armed branch emits SetSpeed while holding the
            // pause), so unpausing here resumes AT THE PIN.
            const int resting = C().sched.config().resting_speed;
            game_unpause();
            if (g_logfile) {
                fprintf(g_logfile,
                    "[CK3-Bridge][pipeline] episode begun: STARTING "
                    "the clock. The planner holds the world at the declared "
                    "resting speed %d, so it resumes at the registered pin; "
                    "observed start now equals declared start.\n", resting);
                fflush(g_logfile);
            }
        }


        const uint64_t now = mono_ms();

        // ---- R3 as REVERSED (§2.7): THE CLOCK RUNS -------------------------
        // A checkpoint clock gate stood here until 2026-09-12. The progenitor
        // reversed G3': in experiment mode the world does NOT wait for an agent,
        // and what an agent's silence costs the experiment is recorded instead.
        // All that is left at this site is the recording — one game-day read per
        // conductor iteration, which is what turns a static "last activity" into
        // a growing span while nobody is talking.
        // ck-zffq: THE DAY IS READ ONCE AND THE TWO CONSUMERS ARE SCOPED
        // SEPARATELY, because they are measuring different things.
        const int64_t day = current_game_day();
        if (day != INT64_MIN) {
            const TickBookkeeping bk = tick_bookkeeping(episode_is_experiment());

            // §2.7's covariate IS episode-scoped: what an agent's silence costs
            // THE EXPERIMENT is meaningless without an experiment.
            if (bk.note_unavailability)
                unavailability().note_day(day, now);

            // ck-ijfa: the clock rate, MEASURED in the condition the episode is
            // actually running in — the conversion factor between the
            // covariate's unit (game-days) and the only unit an operator or a
            // reviewer can ask a question in (wall-clock). The condition travels
            // with the number; see clock_rate.h.
            //
            // ck-zffq: AND IT IS NO LONGER GATED ON AN EPISODE EXISTING. It was,
            // and ck-zgh4 measured the consequence: on a world that had run away
            // unattended the meter read measured=false, samples=0, game_days=0
            // WHILE THE CLOCK ADVANCED 3,351 GAME-DAYS. AN INSTRUMENT GATED ON
            // THE CONDITION IT WOULD DETECT CANNOT DETECT ITS VIOLATION — it
            // went blind in precisely the situation where game time is being
            // spent with nobody watching, which is the one situation where
            // somebody would want to ask the rate afterwards.
            //
            // THE RATE IS A PROPERTY OF THE WORLD, NOT OF THE EPISODE. Nothing
            // about game-days-per-wall-second requires an experiment to be
            // running, and `measured:false` is supposed to mean NOT MEASURED
            // rather than NOT MEASURABLE HERE.
            //
            // COST, since E0 x8 runs on the node: one extra mutex-guarded
            // bookkeeping call per conductor iteration in the modes that
            // previously skipped it. No engine call — `note` touches nothing but
            // its own fields, and the reads it is passed were already being
            // taken for the line above.
            // UNCONDITIONAL ON PURPOSE (ck-zffq): the policy goes IN and the
            // meter honours it. There is no gate here for a later caller to
            // re-add, which is how the original defect got written.
            clock_rate().note(bk, day, now, current_game_speed(), is_paused());
        }

        const FlushTrigger trig = C().sched.due(now);
        if (trig == FlushTrigger::None) continue;

        ClockReading clock;
        clock.paused = is_paused();
        clock.speed  = current_game_speed();

        FlushPlan plan = C().sched.plan_flush(now, clock, trig);
        apply_flush_plan(plan, effects(), C().tickets);
        C().tickets.expire_older_than(now);

        // §2.9: ASSERT THE PULSE LEDGER IS EMPTY AT THE END OF EVERY FLUSH.
        pulse_sweep_flush(plan.flush_id);
    }

    if (g_logfile) {
        fprintf(g_logfile, "[CK3-Bridge][pipeline] conductor thread stopped\n");
        fflush(g_logfile);
    }
}

}  // namespace

void pipeline_start() {
    // `pipeline` AND `null` both run the flush loop: null mode is E0's
    // observer-only free-run, and "free-runs at a fixed speed" is exactly what
    // the plan's restore-the-resting-state step already does. What null does
    // NOT do is take clock OWNERSHIP (pipeline_owns_clock() is false for it), so
    // a null-mode session can still drive the tempo directly. `legacy` starts
    // no thread at all.
    const PipelineMode m = pipeline_mode();
    if (m != PipelineMode::Pipeline && m != PipelineMode::Null) return;
    if (C().running.exchange(true)) return;
    C().thr = std::thread(conductor_loop);
}

void pipeline_stop() {
    if (!C().running.exchange(false)) return;
    C().wake_cv.notify_all();
    if (C().thr.joinable()) C().thr.join();
}

// ============================================================================
// Telemetry (§2.10)
// ============================================================================
void pipeline_emit_state(ResponseBuilder& r) {
    r.key("pipeline");
    r.begin_object();
    r.key("mode"); r.value_string(pipeline_mode_name(pipeline_mode()));
    r.key("flush_id"); r.value_uint(C().sched.last_flush_id());
    // ck-6kvq.9 left a choice open here; ck-6kvq.10 RESOLVES IT IN FAVOUR OF THE
    // AGE. The field name implies a stamp, but (a) a monotonic stamp is
    // meaningless to any reader outside this process, (b) G9's liveness probe
    // wants the age, and (c) ck-6kvq.1's shipped abort ladder ALREADY reads
    // `last_flush_ms` as an age ("no flush for ${pl_flush_ms}ms", max 120000) —
    // emitting a boot-relative stamp there would abort a perfectly healthy
    // episode within two minutes of load. The consumer wins.
    //
    //   last_flush_ms      = AGE in ms since the last flush (0 = none yet)
    //   last_flush_age_ms  = the same number, under the name that says so
    //   last_flush_mono_ms = the raw monotonic stamp, for in-process debugging
    const uint64_t lf  = C().sched.last_flush_ms();
    const uint64_t now = mono_ms();
    const uint64_t age = (lf == 0) ? 0 : (now > lf ? now - lf : 0);
    r.key("last_flush_ms"); r.value_uint(age);
    r.key("last_flush_age_ms"); r.value_uint(age);
    r.key("last_flush_mono_ms"); r.value_uint(lf);
    r.key("last_flush_wall_ms"); r.value_uint(C().last_flush_wall_ms.load(std::memory_order_relaxed));
    r.key("last_flush_paused_ms"); r.value_uint(C().last_paused_ms.load(std::memory_order_relaxed));
    r.key("buffer_depth"); r.value_uint(C().sched.buffer_depth());
    // ---- R3 as REVERSED: the gate's two fields, now CONSTANT (§2.7) -------
    // The checkpoint clock gate was withdrawn on 2026-09-12. `gate_state` and
    // `waiting_on` stay on the wire because ck-6kvq.1's SHIPPED abort ladder
    // reads them and keys its hold timer on "$gate_state|$waiting_on"; removing
    // the keys would change that ladder's behaviour on a bridge it was not
    // rebuilt against, for no gain. "inert" is never in its held set, so the
    // gate can never fire — which is precisely the withdrawal. The
    // `gate_held_beyond_max` termination reason retires with it (§6.3).
    r.key("gate_state"); r.value_string("inert");
    r.key("waiting_on"); r.begin_array(); r.end_array();

    // ---- R4: the board's volume, PUBLISHED ONLY WHEN THE BOARD EXISTS ------
    // This key is absent in arms A, C and E0 and in free play, and that absence
    // is load-bearing, not tidiness. get_tick_info is agent-reachable; a
    // `board:{enabled:false}` block would tell an arm-A agent that a channel
    // exists somewhere and that it has been denied one, which is exactly the
    // third condition decision G5 rejected when it chose ABSENT over
    // present-and-refused. The meter lane reads volume from here in arm B and
    // records zero in arm A because there is nothing to read.
    if (board().enabled()) {
        r.key("board"); r.value_raw(board().stats_json());
    }

    // ---- R3 as REVERSED: unavailability, the measured covariate (§2.7) ----
    const EpisodePolicy ep = episode_policy().get();
    r.key("episode_mode"); r.value_string(episode_mode_name(ep.mode));
    r.key("pins_claims"); r.value_bool(episode_pins_claims());

    //: ck-j6lk: HOW MANY SESSIONS DECLARED `episode_seat`, as THIS OBSERVER saw it,
    //: so the validator can cross-check it against the roster's own count and name a
    //: mismatch (`seat_declaration_mismatch`) instead of inferring one.
    //:
    //: NAMED `episode_seat_sessions`, NOT `declared_seats`, and the name is the point.
    //: scripts/agent_activity.py has published `declared_seats` since ck-gp1g — the
    //: DRIVER's count, `len(rows)` off the roster. Those two numbers are the two
    //: operands of this very comparison, and a check whose entire value is that they
    //: are produced independently must not name them with one word. ck-9rto caught
    //: that we had both chosen it; the pre-existing key keeps it and the newcomer
    //: yields. This spelling says which FIELD is counted (`episode_seat`) and in what
    //: unit (sessions), so neither operand can be mistaken for the other in a bundle.
    //:
    //: It closes the remaining hole in the three-state rule. State (a) destroys a
    //: session that neither declared itself a seat nor ever claimed — which is right
    //: for a driver helper and wrong for an agent that FAILED TO DECLARE and then
    //: never got far enough to claim. The bridge cannot tell those apart, and it
    //: should not guess: it publishes what it was told, the roster publishes what was
    //: declared, and a disagreement between two independently produced numbers is a
    //: named defect rather than a silent exemption. One declaration, two readers.
    //:
    //: Counted over ALL sessions including DETACHED ones — a detached seat is still a
    //: session (see the registry's own note), and a count that dropped them would
    //: disagree with the roster for a reason that is not a defect.
    {
        size_t episode_seat_sessions = 0;
        g_session_registry.for_each([&](const Session& s) {
            if (s.declared_seat.load(std::memory_order_relaxed)) ++episode_seat_sessions;
        });
        r.key("episode_seat_sessions");
        r.value_uint(static_cast<uint32_t>(episode_seat_sessions));
    }

    {
        const std::vector<UnavailSession> us = unavailability().snapshot();

        // `last_activity` — §2.10's own key: per CONTROL session, the game date
        // of its most recent submission or read. Compact by design; the ladder,
        // the overlay poller and the harness all poll this endpoint.
        r.key("last_activity");
        r.begin_object();
        for (const UnavailSession& u : us) {
            r.key(u.session_id.c_str());
            r.begin_object();
            r.key("game_date"); r.value_string(game_date_for_day(u.last_activity_day).c_str());
            r.key("day"); r.value_int(u.last_activity_day);
            r.key("silent_game_days"); r.value_int(u.silent_game_days);
            r.key("unavailable_game_days"); r.value_int(u.unavailable_game_days);
            r.key("attached"); r.value_bool(u.attached);
            r.key("ended"); r.value_bool(u.ended);
            r.end_object();
        }
        r.end_object();

        // The full accounting, including the spans the episode seal writes into
        // run_summary.json as `unavailability_game_days` (§6.1). Bounded: at most
        // kMaxRetainedSpans spans per session, and a session with that many
        // stalls is already far past X.
        r.key("unavailability");
        r.begin_object();
        r.key("current_day"); r.value_int(unavailability().current_day());
        r.key("current_game_date");
        r.value_string(game_date_for_day(unavailability().current_day()).c_str());
        // ck-ijfa: the GATE is in WALL-SECONDS (agent punctuality), X is in
        // GAME-DAYS (experimental cost). `min_span_days` is RETIRED — the
        // game-day gate it named could not tell "the agent was late" from "the
        // world ran fast". Both numbers are PUBLISHED so a bundle always says
        // which it ran under.
        r.key("anomaly_wall_seconds"); r.value_int(ep.unavail_anomaly_wall_s);

        // ── ck-2ngg: X IS A RULE, AND IT SHOWS ITS WORKING ─────────────────
        // `exclusion_days` stays the field every consumer already reads, but it
        // is now COMPUTED for THIS episode's declared horizon rather than the
        // constant 365 (which is the rule's answer at the registered 20-year
        // horizon and at no other). Everything needed to re-derive it offline
        // travels beside it — the fraction and the horizon it was measured over
        // — which is ck-ijfa's property applied to X.
        //
        // `exclusion_armed` is the REQUIRED companion: false means no horizon
        // was declared, so nothing was judged. A consumer reading
        // `episode_invalid:false` alone cannot tell an unjudged episode from a
        // clean one.
        r.key("exclusion_days"); r.value_int(ep.unavail_exclusion_days);
        // ── ck-029l: ARMED IS A CLAIM ABOUT A MEASUREMENT, SO IT NEEDS ONE ─────
        //
        // unavail_exclusion_armed is resolved ONCE at episode open from the
        // declared horizon (episode_policy.cpp:134, from env only) and republished
        // unchanged forever after. It never consulted whether a single game-day
        // was ever OBSERVED. All five real set28 bundles therefore published
        // exclusion_armed:true with exclusion_days:365 over
        // observed_game_days:0, current_day:0, current_game_date:'unknown' and
        // first_observed_day:null — armed, with nothing to apply X to, and
        // exclusion_unarmed_reason:null reading as "no problem".
        //
        // A FIELD WRITTEN AT OPEN CANNOT WITNESS A STATE AT CLOSE. The horizon
        // decision is sound; what was missing is that publishing it as `armed`
        // asserts the exclusion COULD BE APPLIED, and it cannot be applied to a
        // tracker that has seen no days.
        //
        // This is NOT a broken feed. note_day is gated on
        // tick_bookkeeping(episode_is_experiment()).note_unavailability, which is
        // CORRECT — §2.7's covariate is episode-scoped and "what an agent's
        // silence costs THE EXPERIMENT" is meaningless without an experiment. The
        // gate was closed because set28 ran free_play, and that is ck-tt91's root
        // cause (CK3_EPISODE_MODE never exported), one criterion over from the
        // claim pin. Making the feed run in free play would record a covariate
        // outside the only condition that gives it meaning.
        //
        // So the repair is to make the ABSTENTION VISIBLE rather than to
        // manufacture the observation.
        const bool armed_has_day = unavailability().has_day();
        const bool armed_effective = ep.unavail_exclusion_armed && armed_has_day;
        if (ep.unavail_exclusion_armed && !armed_has_day) {
            static std::atomic<bool> said{false};
            bool expect = false;
            if (said.compare_exchange_strong(expect, true) && g_logfile) {
                fprintf(g_logfile,
                    "[CK3-Bridge][pipeline] exclusion REFUSED: armed by the "
                    "declared horizon but NO GAME-DAY OBSERVED — reporting "
                    "unarmed with reason=no_day_observed. X cannot be applied to "
                    "a tracker with no days; armed over zero observation is a "
                    "vacuous pass.\n");
                fflush(g_logfile);
            }
        }
        r.key("exclusion_armed"); r.value_bool(armed_effective);
        r.key("exclusion_unarmed_reason");
        if (armed_effective) r.value_null();
        else if (!armed_has_day && ep.unavail_exclusion_armed)
            r.value_string("no_day_observed");   //: the abstention, NAMED
        else r.value_string(ep.unavail_exclusion_unarmed_reason.c_str());
        r.key("exclusion_fraction");
        r.value_double_prec(ep.unavail_exclusion_fraction, 6);
        r.key("horizon");
        r.begin_object();
        r.key("declared"); r.value_bool(ep.horizon_declared);
        r.key("start_game_date");
        if (ep.horizon_start_game_date.empty()) r.value_null();
        else r.value_string(ep.horizon_start_game_date.c_str());
        r.key("end_game_date");
        if (ep.horizon_end_game_date.empty()) r.value_null();
        else r.value_string(ep.horizon_end_game_date.c_str());
        r.key("game_days"); r.value_int(ep.horizon_game_days);
        // ── DECLARED vs OBSERVED ───────────────────────────────────────────
        // X is 5% of the DECLARED horizon, as ruled — so the tolerance is a
        // property of the episode's CONFIGURATION, not of what it observed. An
        // episode that declares twenty game-years and stops after one has a
        // budget proportional to an observation it never made. That is legal
        // and sometimes deliberate (a stall-leg instrument test declares the
        // registered horizon and runs for minutes), but it must be LEGIBLE:
        // without this field "declared 20, ran 20" and "declared 20, ran 1" are
        // the same bundle. Never fed back into the threshold.
        r.key("observed_game_days");
        r.value_int(unavailability().observed_game_days());
        r.key("first_observed_day");
        if (unavailability().has_day()) r.value_int(unavailability().first_day());
        else r.value_null();
        r.end_object();
        // Retired / refused env knobs, PUBLISHED as ignored rather than
        // swallowed. null is "nobody set it".
        r.key("ignored_x_days_env");
        if (ep.ignored_x_days_env.empty()) r.value_null();
        else r.value_string(ep.ignored_x_days_env.c_str());
        r.key("refused_exclusion_fraction_env");
        if (ep.refused_exclusion_fraction_env.empty()) r.value_null();
        else r.value_string(ep.refused_exclusion_fraction_env.c_str());

        // ck-ijfa: THE RATE, MEASURED, WITH ITS CONDITION. Both pre-registered
        // thresholds above are in GAME-DAYS and every question anyone asks about
        // them is in WALL-CLOCK; this object is the only honest bridge between
        // the two, and `measured:false` means NOT MEASURED — a consumer must say
        // so rather than substitute an estimate from another condition, which is
        // the defect that filed this bead.
        {
            const ClockRateSample cr = clock_rate().get();
            r.key("clock_rate");
            r.begin_object();
            r.key("measured"); r.value_bool(cr.measured);
            r.key("game_days"); r.value_int(cr.game_days);
            r.key("wall_seconds"); r.value_double(cr.wall_seconds);
            if (cr.measured) {
                r.key("game_days_per_wall_second");
                r.value_double_prec(cr.game_days_per_wall_second, 9);
                r.key("game_years_per_wall_hour");
                r.value_double_prec(cr.game_years_per_wall_hour, 9);
            } else {
                r.key("game_days_per_wall_second"); r.value_null();
                r.key("game_years_per_wall_hour");  r.value_null();
            }
            // THE CONDITION. A rate without one is not evidence (ck-ijfa).
            r.key("condition");
            r.begin_object();
            r.key("episode_mode"); r.value_string(episode_mode_name(ep.mode));
            r.key("arm"); r.value_string(ep.arm.c_str());
            r.key("speed_min"); r.value_int(cr.speed_min);
            r.key("speed_max"); r.value_int(cr.speed_max);
            r.key("samples"); r.value_uint(cr.samples);
            r.key("paused_samples"); r.value_uint(cr.paused_samples);
            r.key("reanchors"); r.value_uint(cr.reanchors);
            r.end_object();
            r.end_object();
        }
        r.key("day_regressions"); r.value_uint(unavailability().day_regressions());
        // The bridge FLAGS; the analysis excludes. Never silently dropped.
        bool invalid = false;
        for (const UnavailSession& u : us) invalid = invalid || u.over_threshold;
        r.key("episode_invalid"); r.value_bool(invalid);
        r.key("sessions");
        r.begin_array();
        for (const UnavailSession& u : us) {
            r.begin_object();
            r.key("session_id"); r.value_string(u.session_id.c_str());
            r.key("first_day"); r.value_int(u.first_day);
            r.key("first_game_date"); r.value_string(game_date_for_day(u.first_day).c_str());
            r.key("last_activity_day"); r.value_int(u.last_activity_day);
            r.key("last_activity_game_date");
            r.value_string(game_date_for_day(u.last_activity_day).c_str());
            r.key("activity_count"); r.value_uint(u.activity_count);
            // RAW: every forward gap, both units — kept so an analysis can
            // re-derive a different gate without re-running an episode.
            r.key("silent_game_days"); r.value_int(u.silent_game_days);
            r.key("silent_wall_seconds"); r.value_double(u.silent_wall_seconds);
            r.key("gap_count"); r.value_uint(u.gap_count);
            // THE COVARIATE: ANOMALOUS gaps only (wall duration > the gate).
            // `unavailable_game_days` is the GAME-DAYS those gaps SPANNED — what
            // the absence cost the experiment — and is what X is tested against.
            r.key("unavailable_game_days"); r.value_int(u.unavailable_game_days);
            r.key("unavailable_wall_seconds"); r.value_double(u.unavailable_wall_seconds);
            r.key("span_count"); r.value_uint(u.span_count);
            // A gap with no wall stamp cannot be judged and is NOT counted.
            // Published so that is visible rather than an invisible zero.
            r.key("wall_measured"); r.value_bool(u.wall_measured);
            r.key("unjudged_gap_count"); r.value_uint(u.unjudged_gap_count);
            r.key("spans_truncated"); r.value_bool(u.spans_truncated);
            // ck-48lf: EXACT extrema over every gap, beside the truncated
            // itemisation, so a reader never derives a maximum from `spans`.
            r.key("max_span_wall_seconds"); r.value_double(u.max_span_wall_seconds);
            r.key("max_span_game_days"); r.value_int(u.max_span_game_days);
            r.key("over_threshold"); r.value_bool(u.over_threshold);
            r.key("attached"); r.value_bool(u.attached);
            r.key("ended"); r.value_bool(u.ended);
            r.key("spans");
            r.begin_array();
            for (const UnavailSpan& sp : u.spans) {
                r.begin_object();
                r.key("start_day"); r.value_int(sp.start_day);
                r.key("end_day"); r.value_int(sp.end_day);
                r.key("days"); r.value_int(sp.days);
                r.key("start_game_date"); r.value_string(game_date_for_day(sp.start_day).c_str());
                r.key("end_game_date"); r.value_string(game_date_for_day(sp.end_day).c_str());
                r.key("open"); r.value_bool(sp.open);
                // ck-ijfa: EVERY GAP IN BOTH UNITS. `wall_measured:false` means
                // the endpoints carried no stamp, so the duration is UNKNOWN —
                // never a game-day figure converted at an assumed rate.
                r.key("wall_measured"); r.value_bool(sp.wall_measured);
                r.key("wall_seconds"); r.value_double(sp.wall_seconds);
                r.key("anomalous"); r.value_bool(sp.anomalous);
                r.end_object();
            }
            r.end_array();
            r.end_object();
        }
        r.end_array();
        r.end_object();
    }

    // Bead .13's hook, carried not acted on: which control sessions are spent.
    {
        const std::set<std::string> ex = exhausted_snapshot();
        r.key("budget_exhausted");
        r.begin_array();
        for (const std::string& sid : ex) r.value_string(sid.c_str());
        r.end_array();
    }

    // ---- R1c: the pulse ledger (ck-6kvq.10, §2.9) -------------------------
    // NULL while clean, an OBJECT once latched — and NEVER an empty object.
    // ck-6kvq.1 treats any non-empty, non-"0", non-"false" value of this key as
    // the `pulse_leak` abort, so `{}` on a healthy run would be a false abort.
    // The verdict is LATCHED: a later clean flush must not erase the evidence
    // that the episode's condition silently changed.
    {
        const PulseVerdict pv = pulse_verdict();
        r.key("pulse_leak");
        if (pv.leaked) r.value_raw(pv.first.to_json());
        else           r.value_null();
        r.key("pulse_leak_count"); r.value_uint(pv.count);
        r.key("pulses_live"); r.value_uint(pulse_ledger().live());
        r.key("pulses_entered"); r.value_uint(pulse_ledger().entered_total());
        r.key("pulse_ledger_overflow"); r.value_uint(pulse_ledger().overflow_count());
        r.key("episode_aborted"); r.value_bool(pipeline_episode_aborted());
        const char* ar = pipeline_abort_reason();
        r.key("abort_reason"); r.value_string(ar ? ar : "");
    }

    // THE EPISODE THE BRIDGE BELIEVES IT IS IN. Never omitted, so the lanes
    // coding against this shape keep a stable key set.
    //
    // ck-kc32 CORRECTS WHAT USED TO BE THIS COMMENT. It said ck-6kvq.2 owned
    // minting this and that "until it lands" the bridge published whatever
    // CK3_EPISODE_ID it was handed. ck-6kvq.2 CLOSED on 2026-09-12 having
    // shipped its harness half and not this one, so the sentence pointed at a
    // landing that had already happened and described a wait that was never
    // going to end. Two seats read it and concluded the field was still
    // waiting on someone else; meanwhile the gate that compares this to the
    // driver's id could not pass, because a launch-time env var cannot carry an
    // id minted at bundle open. The bridge now learns it from
    // declare_save_identity — see pipeline_note_episode_open.
    r.key("episode_id"); r.value_string(ep.episode_id.c_str());
    r.key("tickets_open"); r.value_uint(C().tickets.size());
    r.key("tickets_evicted"); r.value_uint(C().tickets.evicted_count());
    r.key("clock_drift_events"); r.value_uint(C().clock_drift_events.load(std::memory_order_relaxed));
    r.key("unclassified_calls"); r.value_uint(C().unclassified_total.load(std::memory_order_relaxed));
    r.key("flush_period_ms"); r.value_uint(C().sched.config().period_ms);
    // ck-43u3: THE TRIGGER, OBSERVED RATHER THAN INFERRED FROM A LATENCY.
    // `flush_trigger_last` is the snapshot; `flush_triggers` is the one that
    // answers the question, because "are my reads served early" is a
    // distribution and not an event. A reader comparing cadence against
    // read_lane_early sees immediately whether the declared observer's
    // privilege is doing anything for this episode.
    r.key("flush_trigger_last");
    r.value_string(flush_trigger_name(C().sched.last_trigger()));
    r.key("flush_triggers"); r.begin_object();
    for (int i = 0; i < 6; ++i) {
        const FlushTrigger ft = static_cast<FlushTrigger>(i);
        r.key(flush_trigger_name(ft));
        r.value_uint(C().sched.trigger_count(ft));
    }
    r.end_object();
    // ck-on4u: THE SPEED THE PLANNER RESTS AT, OBSERVED RATHER THAN DECLARED.
    // The same argument ck-ock8 made for the flush period: the harness's own
    // declaration cannot witness whether the bridge received it, and this
    // scheduler is the SECOND writer of the game speed — the one with no
    // "Action: set_speed" line behind it. `resting_speed_declared` separates
    // "the harness established §9.4's condition" from "nothing did and the
    // pre-registered default stands", which are the same number and a different
    // fact.
    r.key("resting_speed"); r.value_int(C().sched.config().resting_speed);
    r.key("resting_speed_declared"); r.value_bool(ep.game_speed_declared);
    r.key("resting_speed_env"); r.value_string(kGameSpeedEnv);
    // ck-h6nx (D'): WHO DROVE THE CADENCE. Recorded so it is auditable after
    // the fact rather than inferred from who happened to be plugged in. EMPTY
    // MEANS NOBODY DECLARED — the arms then behave exactly as before this bead,
    // which is the honest reading of an absent declaration and not a silent
    // default to some session the bridge picked for itself.
    r.key("episode_observer"); r.value_string(pipeline_episode_observer().c_str());

    // ── ck-8o5j: THE CLOCK-START HOLD, AS ITS OWN FIELD ────────────────────
    // A reader needs to distinguish "the hold is armed and the world is frozen"
    // from "the episode is running", and NOTHING ON THE WIRE SAID SO. `gate_state`
    // is NOT that field: it is the WITHDRAWN checkpoint clock gate, hardcoded to
    // "inert" a few lines above, constant in every mode on every healthy bridge
    // — it stays only because ck-6kvq.1's shipped abort ladder keys a hold timer
    // on it. Anything reading `gate_state` for the clock-start hold reads a
    // constant and learns nothing about this.
    //
    //   clock_hold_armed   true  = no episode has opened; the world is PAUSED at
    //                              the declared speed and will not advance
    //                      false = the episode opened (or this mode never holds)
    //   clock_hold_mode    which mode's rule is in force, so "false" can be read
    //                      as "released" rather than "never armed"
    //
    // ck-9rto's launch cross-check needs exactly this: an id that landed is
    // NECESSARY AND NOT SUFFICIENT, because a bridge can agree the episode is
    // open and still be holding the clock — the same silent stall, one field
    // over.
    r.key("clock_hold_armed");
    r.value_bool(C().sched.config().hold_clock_until_observer);
    r.key("clock_hold_mode");
    r.value_string(pipeline_owns_clock() ? "pipeline" : "not_held");
    r.end_object();
}

}  // namespace bridge
