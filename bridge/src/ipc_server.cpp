// Unix domain socket IPC server.
// Listens for agent connections and relays messages.
// Background reader thread continuously reads NDJSON from the client
// and pushes complete messages to a thread-safe queue.

#include "instance_paths.h"  // ck-qn03
#include "envelope_shape.h"
#include "observation_route.h"   //: ck-h4pn
#include "inflight.h"
#include "reply_record.h"
#include "refusal_code.h"
#include "refusal_record.h"
#include "skip_held_heir.h"
#include "drain_provenance.h"
#include "scope_slot_policy.h"
#include "pipeline_frames.h"   // ck-324r: the shared spine builder + escaper
#include "management_ledger_frame.h"
#include "episode_participant.h"  //: ck-h4pn: the ONE composition body, which mgmt_ledger_probe also calls
#include "log_archive.h"
#include "instance_paths.h"
#include "thread_registry.h"
#include "bridge.h"
#include "ck3_offsets.h"
#include "game_date.h"
#include "offset_version.h"
#include "build_provenance.h"
#include "build_id_witness.h"   // ck-nkyc leg (e)
#include "snapshot.h"
#include "session.h"
#include "pipeline.h"
#include "episode_policy.h"
#include "client_slot.h"
#include "world_identity.h"   // ck-6kvq.5: ONE identity, shared with ck-9fc3
#include "world_epoch.h"      // ck-hxh7's world_epoch(), the "did the world change" half

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <signal.h>
#include <setjmp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <time.h>
#include <algorithm>
#include <atomic>

namespace bridge {

// SIGSEGV recovery (defined in bridge_init.cpp)
extern thread_local sigjmp_buf g_recovery_point;
extern thread_local bool g_in_protected_region;

static int g_server_fd = -1;
// ck-xqol: see the init_priority note on g_clients below — these are assigned by
// start_ipc_server(), which dll_init calls before this TU's default-priority
// initializers would otherwise run (which would then clobber the handles and
// silently turn stop_ipc_server()'s join into a no-op).
static std::thread g_server_thread __attribute__((init_priority(101)));
static std::thread g_reader_thread __attribute__((init_priority(101)));
static std::mutex g_client_mutex;
static std::atomic<bool> g_server_running{false};
static char g_socket_path[256] = {};
static int g_send_timeout_ms = 2000;

// ck-jh76: the reply site needs the send OUTCOME, but send_observation has many
// callers and widening its signature would touch all of them. A thread-local
// pointer, set only by the reply site that cares, keeps every other caller
// byte-identical — and leaves the outcome UNRECORDED (never a fabricated "ok")
// for callers that do not opt in.
thread_local bridge::SendOutcome* g_last_send_outcome = nullptr;


// #123: live tick + game_date stamping for EVERY response.
//
// The MCP-side envelope {ok,data,error,tick,game_date} reads its tick/game_date
// from cached tick_start/observation pushes — but while the game is PAUSED (the
// required state for reads) the bridge pushes no tick_start, so the cache stays
// 0/"unknown" and EVERY non-get_tick_info response carries stale values.
//
// Fix: stamp the LIVE tick + date into the common response-emit path (right where
// request_id is injected), so every handler's reply carries them uniformly. This
// reads the same global game-clock do_get_tick_info reads. Returns a JSON fragment
// like  "tick":1234,"game_date":"1097.03.21",  (no leading/trailing comma) so the
// caller can splice it after the request_id field. Best-effort: on a null game
// state it emits  "tick":N,"game_date":"unknown" .  Non-object responses ('[' …)
// are skipped by the caller, matching the existing request_id behaviour.
std::string build_envelope_clock_fragment() {
    std::string frag;
    frag.reserve(48);
    uint64_t tick = g_tick_id.load(std::memory_order_relaxed);
    char buf[64];
    snprintf(buf, sizeof(buf), "\"tick\":%llu,", (unsigned long long)tick);
    frag += buf;

    uintptr_t gs = safe_read_offset<uintptr_t>(ck3::globals::GAME_STATE_ROOT);
    if (gs) {
        // ck-05mg: render through the one shared calendar helper (real CK3
        // month lengths), never a local 30-day-month copy.
        int32_t raw_date = safe_read<int32_t>(gs + ck3::game_state::GAME_DATE);
        char ymd[16];
        ck3::game_date::format_raw(raw_date, ymd, sizeof(ymd));
        char date_str[40];
        snprintf(date_str, sizeof(date_str), "\"game_date\":\"%s\"", ymd);
        frag += date_str;
    } else {
        frag += "\"game_date\":\"unknown\"";
    }
    return frag;
}

// --- Multi-client slot table (ck-multiclient) ---
// The bridge now serves up to MAX_CLIENTS concurrent clients (e.g. the MCP
// server + a per-turn UserPromptSubmit hook). Responses to a client's own
// request are routed back to that client via a thread-local target set by the
// reader thread before it processes a slot's lines (so e.g. a handshake_ack
// goes only to the connecting client). Game-thread egress (on_tick drains,
// event notifications) has no thread-local target set and therefore BROADCASTS
// to all live clients; each client filters by the request_id it sent (the
// async MCP listener routes by request_id; the hook's query loop matches it),
// so cross-client lines are harmlessly ignored.
//
// CONCURRENCY INVARIANT: a slot's `fd` and `gen` are written ONLY while BOTH
// g_client_mutex (membership) AND that slot's send_mtx are held; they may be
// read under EITHER lock. send_to_slot() validates `gen` under send_mtx so a
// released-and-reused slot can never receive a stale write (no cross-client
// leak). Peer diagnostics captured via SO_PEERCRED at accept(); peer_pid is
// probed via kill(pid,0) to reap a slot whose owning process died.
// ClientSlot, MAX_CLIENTS and the slot lifecycle now live in
// include/client_slot.h + src/client_slot.cpp (ck-xqol) so they can be
// host-compiled by test/test_client_slot.cpp.  The concurrency invariant above
// is restated there.

// ck-xqol — INIT ORDER.  `dll_init` (the __attribute__((constructor)) in
// bridge_init.cpp) is .init_array[1], AHEAD of every C++ dynamic initializer in
// this library, and it calls bridge_init() -> start_ipc_server() ->
// std::thread(reader_loop).  Without a priority, this TU's globals are
// constructed at .init_array[3] and the reader thread can reach a slot table
// that is still raw .bss — which is exactly the crash this bead fixes (see
// client_slot.h).  init_priority puts these initializers in .init_array.101,
// which the linker orders BEFORE the unprioritised entries, so the table, the
// queue and the thread handles all exist before dll_init can start anything.
//
// slot_is_open() makes the lifecycle correct even if this ordering is ever lost
// again; the priority makes the window not exist.  Both, deliberately.
//
// The other init_priority(101) globals of this TU are g_server_thread,
// g_reader_thread (above), g_queue_cv and g_msg_queue (below) — every object
// here that start_ipc_server() or the threads it spawns can touch.
static ClientSlot g_clients[MAX_CLIENTS] __attribute__((init_priority(101)));

// Reader-thread response routing: set to the originating slot (idx + its gen
// snapshot) while processing that slot's inbound lines, so send_observation()
// routes the reply back to the requester instead of broadcasting. Left at -1
// on the game thread, where send_observation() broadcasts (events + on_tick
// drains, which clients then filter by request_id).
static thread_local int      g_send_target_slot = -1;
static thread_local uint32_t g_send_target_gen  = 0;

// --- Message queue (thread-safe) ---
// ck-xqol: same init-order reason as g_clients (see the init_priority note above).
// g_msg_queue in particular HEAP-ALLOCATES in its constructor, so a push_back
// from a thread dll_init started would be written into raw .bss and then
// dropped on the floor when the real constructor finally ran.
static std::mutex g_queue_mutex;
static std::condition_variable g_queue_cv __attribute__((init_priority(101)));
static std::deque<std::string> g_msg_queue __attribute__((init_priority(101)));

// --- Accept-loop liveness diagnostics (ck-7tzu) ---
// Monotonic seconds since bridge load, updated by server_loop after poll() returns
// and again after accept()+mutex-swap completes. Divergence between the two (or
// either staying stale) proves where the thread is wedged.
static std::atomic<uint64_t> g_accept_loop_last_poll_ts{0};
static std::atomic<uint64_t> g_accept_loop_last_accept_ts{0};
static std::atomic<uint64_t> g_accept_loop_last_swap_ts{0};
static std::atomic<uint64_t> g_accept_loop_total_accepts{0};

static uint64_t ipc_monotonic_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ull + ts.tv_nsec / 1000000ull;
}

static uint64_t ipc_monotonic_secs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec);
}

// True iff `pid` refers to a live process. kill(pid, 0) sends no signal but
// still walks the task list, so ESRCH means the pid is gone. EPERM means
// alive-but-owned-by-someone-else, which we still treat as alive.
static bool ipc_peer_pid_alive(int pid) {
    if (pid <= 0) return false;
    if (kill(static_cast<pid_t>(pid), 0) == 0) return true;
    return errno != ESRCH;
}

// Must be called with g_client_mutex held. Closes the slot's fd (under its
// send_mtx so an in-flight send finishes/aborts cleanly and the fd can't be
// written after close), bumps the generation (so any pending send targeting
// this slot is rejected by the gen check), and resets the slot to "free".
//
// Deliberately does NOT touch g_agent_mode: with a per-turn hook that
// connects/disconnects every turn, resetting steering on disconnect would
// wipe the MCP client's AI control every turn. Steering persists; control is
// managed explicitly via take_control / release_control.
static void release_slot_locked(int idx, const char* reason) {
    if (idx < 0 || idx >= MAX_CLIENTS) return;
    ClientSlot& s = g_clients[idx];

    // ck-xqol: the close + field reset is now client_slot_release(), which is a
    // no-op on a slot that is not open — never constructed, never accepted, or
    // already released.  It also hands the session back instead of tearing it
    // down, because session teardown needs the registry and the pipeline and
    // those do not belong in the slot unit.
    const bridge::SlotReleaseResult rel = bridge::client_slot_release(s, ::close);
    if (!rel.released) return;

    if (bridge::g_logfile) {
        fprintf(bridge::g_logfile,
            "[CK3-Bridge] Releasing client slot=%d fd=%d peer_pid=%d (%s)\n",
            idx, rel.fd, rel.peer_pid, reason ? reason : "");
        fflush(bridge::g_logfile);
    }

    if (rel.session) {
        // ck-xqol: the session id is copied BY VALUE.  Every call below used to
        // pass `s.session->session_id`, a reference that lives INSIDE the very
        // object destroy() erases from sessions_; it survived only because
        // destroy() happens to stop reading the parameter before the erase.
        // That is a trap for the next edit, not a design.
        const std::string session_id = rel.session->session_id;
        // ck-6kvq.9: drop the session from the flush scheduler's registered set
        // so early-flush condition (b) ("everyone has spoken") cannot wait
        // forever on a socket that is gone. Buffered calls are NOT dropped —
        // their tickets survive and are still flushed and still attributed.
        bridge::pipeline_unregister_session(session_id.c_str());

        // R3 (ck-6kvq.10, design §2.7): THE CLAIM PIN — the half of R3 that
        // SURVIVED the 2026-09-12 G3' reversal. In an EXPERIMENT episode a
        // dropped socket must NOT destroy the session. Destroying it releases
        // its claims, which re-enables `ai_ctrl+0x28` for a character the
        // episode's frozen arm says is agent-controlled — an uncontrolled change
        // to the condition mid-episode. The clock is no longer held for that
        // agent (the gate is withdrawn: the world keeps moving and the
        // character's last personality steer persists), but the ARM never
        // changes. The gap is instead RECORDED as unavailability in game-days
        // and, past X, invalidates the episode.
        //
        // NOTE the pin is now on EXPERIMENT MODE, not on an armed gate — bead
        // .5's resume rebinds this same session and re-registers it.
        //
        // This deliberately overrides docs/design/multi-agent-control.md Q5 for
        // experiment runs. FREE PLAY IS NOT AN EPISODE, so Q5's orphan-to-native
        // behaviour below is exactly what it has always been.
        //: ck-j6lk: THE PIN IS KEYED ON CLAIMS, because a session that has never
        //: held one is not a seat.
        //:
        //: The shakedown-2 episode sealed INVALID on unavailability_over_x with the
        //: agent blameless — its seat had 70 activities and zero unavailable days.
        //: The cost came from two driver helpers, episode-driver-C-steer-probe (0
        //: activities) and episode-driver-C-speed-pin (2), which handshake in CONTROL
        //: mode, do their one job and close. Because this branch pinned every
        //: detached control session whether or not it held a claim, each accrued 364
        //: silent days against an X of 18 — so EVERY registered episode would have
        //: come out invalid, and the metric would have been measuring the driver.
        //:
        //: R3 protects claims. A session holding none has nothing for the pin to
        //: protect: destroying it releases nothing, re-enables no ai_ctrl+0x28, and
        //: cannot change the arm's frozen condition. So it is destroyed, exactly as
        //: in free play, and accrues nothing.
        //:
        //: THE DETACHED NOTE IS SUPPRESSED, not merely followed by an ended note.
        //: pipeline_note_session_detached is what enters the unavailability tracker;
        //: firing it and then ending would leave a zero-length span and a detach
        //: event in the evidence for something that was never a seat. A non-seat
        //: should be absent from that record, not present with a zero.
        //:
        //: claims_ever, NOT the current claim set: a session that claimed, released
        //: and then detached IS a seat and keeps today's behaviour exactly. Those two
        //: cases are indistinguishable in every other field (see the field's own note
        //: in session.h), which is why the field had to exist before this branch could
        //: be written.
        //:
        //: FREE PLAY IS UNTOUCHED — the else arm below still notes detached and then
        //: ended, byte for byte as before. This narrows only the experiment pin.
        //: THREE STATES, ruled by the progenitor ("amend, re-freeze, then fix"):
        //:   (a) NOT declared and claims_ever == 0 -> not a participant. Suppress the
        //:       detached note, end it, destroy it, accrue nothing.
        //:   (b) DECLARED, claimed or not -> today's behaviour exactly. A seat that
        //:       dies in ck-3ru8's pre-claim window stays a MEASURED HOLE, which is
        //:       the point: the fix must not make a genuine agent failure invisible.
        //:   (c) claims_ever > 0 -> today's behaviour exactly. This also covers a
        //:       seat whose declaration went missing, so a dropped field cannot
        //:       un-seat something that actually steered.
        //: Only (a) changes. (b) and (c) are the same code path they always were.
        uint64_t claims_ever = 0;
        bool declared_seat = false;
        if (rel.session) {
            claims_ever = rel.session->claims_ever.load(std::memory_order_relaxed);
            declared_seat = rel.session->declared_seat.load(std::memory_order_relaxed);
        }
        const bool never_a_participant =
            bridge::never_an_episode_participant(declared_seat, claims_ever);

        if (bridge::episode_pins_claims() && never_a_participant) {
            if (bridge::g_logfile) {
                fprintf(bridge::g_logfile,
                    "[CK3-Bridge] session %s UNDECLARED CLAIMLESS DETACH — "
                    "destroying, not pinning (experiment mode); episode_seat absent "
                    "and claims_ever=0, so it was never a participant and accrues "
                    "NO unavailability\n",
                    session_id.c_str());
                fflush(bridge::g_logfile);
            }
            //: ck-j6lk, second pass: FORGET, not end. Destroying the session was
            //: necessary and NOT SUFFICIENT — shakedown 3 on deploy-32 shows this
            //: branch firing twice and unavailability_game_days still reporting both
            //: helpers, ended, each with an open 364-day span over an X of 18. The
            //: registry forgot them; the ledger kept accruing, because its
            //: note_session_ended contract is to keep the record — correct for a
            //: destroyed SEAT and wrong for something that was never a participant.
            //: My first acceptance graded the registry and passed while the episode
            //: still sealed invalid: the ledger is a different observer and needed
            //: its own row.
            bridge::pipeline_forget_session(session_id.c_str());
            bridge::g_session_registry.destroy(session_id);
            //: THIS RETURN STEPS OVER NOTHING TODAY: the `if (rel.session)` block it
            //: leaves is the last statement of this function, so returning here and
            //: falling through are the same. That is a property of the current shape,
            //: not a guarantee — add cleanup after that block and this return silently
            //: skips it for claimless sessions only, which is the worst shape of bug
            //: to find. acceptance_j6lk asserts the block is still last, so the next
            //: edit reds a row instead of inheriting a hazard.
            return;
        }

        bridge::pipeline_note_session_detached(session_id.c_str());
        if (bridge::episode_pins_claims()) {
            // ck-6kvq.5: .10 stopped the DESTROY; this is the other half — the
            // session moves to DETACHED and stays RESUMABLE BY TOKEN.  The
            // slot-generation argument is the guard: if the client already
            // reconnected and rebound this session to another slot, the registry
            // refuses this close and NOTHING changes.  Without it a late reap of
            // a socket that is already dead (peer-pid probe, keepalive sweep)
            // takes a LIVE session's claims offline.
            // ck-xqol hands back `bound_gen`, the generation the session was
            // bound at, captured BEFORE the release bumped it — which is
            // exactly the binding this close is entitled to tear down.
            const bool detached = bridge::g_session_registry.detach_from_slot(
                session_id, idx, rel.bound_gen, ipc_monotonic_ms());
            if (bridge::g_logfile) {
                fprintf(bridge::g_logfile,
                    "[CK3-Bridge] session %s %s — claims PINNED "
                    "(experiment mode); not destroying, not releasing, "
                    "ai_ctrl+0x28 untouched; unavailability still accruing\n",
                    session_id.c_str(),
                    detached ? "DETACHED (socket gone), resumable by token"
                             : "left ALONE (stale slot generation — it is already "
                               "bound elsewhere)");
                fflush(bridge::g_logfile);
            }
            // The detach REPORT ($CK3_EPISODE_DIR/state/detached-since) is
            // published by the 1s sweep in server_loop, NOT here: this runs with
            // g_client_mutex held and the report does file I/O.  A sub-second
            // delay is immaterial to a bound measured in seconds; stalling the
            // accept path behind a disk write is not.
        } else {
            bridge::pipeline_note_session_ended(session_id.c_str());
            bridge::g_session_registry.destroy(session_id);
        }
    }
}

// ck-6kvq.5: run a world-identity reconciliation and evict any slot whose
// session it erased.  Contract in bridge.h.
//
// ORDER MATTERS AND THE ORDER IS: null the slot's `session` pointer FIRST, then
// release the slot.  client_slot_release() HANDS THE SESSION BACK for the caller
// to tear down (ck-xqol), and after a reconciliation that pointer is already
// freed — so release_slot_locked would call pipeline_unregister_session() and
// destroy() on dead memory.  Nulling it first makes the release see no session,
// which is the truth.  And the slot is matched by the `session_id` it caches BY
// VALUE, never by dereferencing the pointer: the same lesson, one step earlier.
std::vector<std::string> ipc_reconcile_declared_world(const WorldIdentity& declared,
                                                      uint64_t current_epoch) {
    std::lock_guard<std::mutex> lock(g_client_mutex);
    std::vector<uint32_t> purged;
    std::vector<std::string> erased =
        g_session_registry.reconcile_declared_world(declared, current_epoch, &purged);
    if (erased.empty()) return erased;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        ClientSlot& s = g_clients[i];
        if (!slot_is_open(s) || s.session == nullptr) continue;
        if (std::find(erased.begin(), erased.end(), s.session_id) == erased.end())
            continue;
        // The Session object is already gone.  Drop the dangling pointer and the
        // id with it, then close the socket: a client whose world was refuted
        // must handshake fresh, and leaving it connected to a session that no
        // longer exists is how a use-after-free reaches the dispatch choke point.
        s.session = nullptr;
        s.session_id.clear();
        release_slot_locked(i, "world identity refuted this session's claims");
    }
    if (g_logfile) {
        fprintf(g_logfile,
                "[CK3-Bridge] declare_save_identity erased %zu session(s) and "
                "released %zu claim(s); their slots were evicted\n",
                erased.size(), purged.size());
        fflush(g_logfile);
    }
    return erased;
}

// M1: parse a handshake frame and create/attach a Session to the slot.
// Legacy clients (protocol_version < 2 or absent) get an implicit control
// session so single-agent clients keep working.  Unknown protocol majors are
// refused with a typed handshake_ack and the slot is released.
struct HandshakeResult {
    bridge::Session* session = nullptr;
    const char* status = "ok";
    const char* reason = "";
    // ck-6kvq.5.  `resumed` is the HONEST ECHO of what the bridge actually did
    // with a presented resume_token — the same lesson as §1.4's `session_mode`
    // echo: a client that asked to resume and silently got a fresh session
    // cannot tell that its claims are gone, and in experiment mode that is the
    // difference between a valid episode and a quietly invalid one.
    bool resumed = false;
    const char* resume_status = "";     // "" when no token was presented
    const char* identity_reason = "";
};
static HandshakeResult handle_handshake_for_slot(int idx, uint32_t read_gen,
                                                const std::string& line) {
    using namespace rapidjson;
    HandshakeResult result;
    Document doc;
    doc.Parse(line.c_str());
    if (doc.HasParseError() || !doc.IsObject()) return result;

    const Value* vtype = doc.HasMember("type") ? &doc["type"] : nullptr;
    if (!vtype || !vtype->IsString() || strcmp(vtype->GetString(), "handshake") != 0)
        return result;

    int protocol_version = 1;
    if (doc.HasMember("protocol_version") && doc["protocol_version"].IsInt())
        protocol_version = doc["protocol_version"].GetInt();

    // Refuse unknown protocol majors.  The handshake field is the major version;
    // v1 is legacy, v2 is M1.  Anything newer must be rejected so a stale bridge
    // cannot be driven by a future client.
    constexpr int SUPPORTED_PROTOCOL_MAJOR = 2;
    if (protocol_version > SUPPORTED_PROTOCOL_MAJOR) {
        result.status = "unsupported_protocol";
        result.reason = "protocol major not supported by this bridge";
        if (bridge::g_logfile) {
            fprintf(bridge::g_logfile,
                "[CK3-Bridge] Handshake v%d slot=%d REFUSED: unsupported protocol major "
                "(supported <= %d)\n",
                protocol_version, idx, SUPPORTED_PROTOCOL_MAJOR);
            fflush(bridge::g_logfile);
        }
        return result;
    }

    const char* client_name = "unknown";
    if (doc.HasMember("client_name") && doc["client_name"].IsString())
        client_name = doc["client_name"].GetString();

    // ── ck-h6nx (D'): THE EPISODE DECLARES ITS OBSERVER ────────────────────
    // ONE named participant of ONE episode, declared here and recorded in the
    // bundle. NEVER `mode == observe`: keyed on the mode, any attached
    // diagnostic would start driving the flush cadence of the world it is
    // measuring. The driver knows its own meter-lane session and says so.
    //
    // FIRST DECLARATION WINS, and the bridge does not arbitrate: a second
    // claimant is REFUSED THE PRIVILEGE (not the session) and logged, because
    // two privileged readers would be two things driving the cadence and the
    // episode would have no single answer to "who drove it".
    const bool declares_observer =
        doc.HasMember("episode_observer") && doc["episode_observer"].IsBool()
        && doc["episode_observer"].GetBool();

    //: ck-j6lk: the SEAT declaration, read exactly as the observer's is — present,
    //: boolean, and true. Absent or non-boolean is NOT declared, which is the
    //: conservative reading: a session that does not say it is a seat is only
    //: exempt from the pin if it has also never claimed (state a), and any claim at
    //: all puts it back on today's behaviour (state c). So a missing declaration
    //: can never silently un-seat something that actually steered.
    const bool declares_seat =
        doc.HasMember("episode_seat") && doc["episode_seat"].IsBool()
        && doc["episode_seat"].GetBool();

    bridge::Session::Mode mode = bridge::Session::Mode::CONTROL;
    if (doc.HasMember("mode") && doc["mode"].IsString())
        mode = bridge::session_mode_from_string(doc["mode"].GetString(),
                                                bridge::Session::Mode::CONTROL);

    // ck-6kvq.4 (R8, §6.3), landed by ck-1ek9: in `null` mode NOBODY gets a
    // control session. That is the enforcement behind E0's "bridge attached, no
    // steering, zero tokens" — without it, "no control sessions" was a
    // convention that nothing in the bridge checked, and the two guards that
    // rest on it could not fire.
    //
    // DOWNGRADE, never reject. A rejection would take the read-only tools down
    // with it: the self-heal probe, the overlay poller and the harness all still
    // handshake as protocol v1, and v1 is force-upgraded to CONTROL below — so a
    // flat refusal would blind the ops ladder for the whole of an unattended
    // episode, which is a worse failure than a downgraded session. Downgrading
    // routes every mutation through the observe-mode refusal that already exists
    // and is already comprehensive (dispatch_router.cpp:2298, session.cpp:229,
    // session.cpp:1201 — all `reason:"observe_mode"`), instead of adding a
    // second gate that could disagree with the first.
    //
    // The ack echoes the mode the bridge ACTUALLY minted (it is read off
    // `session->mode`), so a client is never left believing it can steer.
    const bool null_mode_downgrade =
        !bridge::mode_admits_control_sessions(bridge::pipeline_mode());
    if (null_mode_downgrade && mode != bridge::Session::Mode::OBSERVE) {
        mode = bridge::Session::Mode::OBSERVE;
        if (bridge::g_logfile) {
            fprintf(bridge::g_logfile,
                "[CK3-Bridge][pipeline] handshake from '%s' downgraded to observe "
                "(%s): pipeline mode=%s admits no control sessions\n",
                client_name, bridge::pipeline_mode_control_downgrade_reason(),
                bridge::pipeline_mode_name(bridge::pipeline_mode()));
            fflush(bridge::g_logfile);
        }
    }

    std::string resume_token;
    if (doc.HasMember("resume_token") && doc["resume_token"].IsString())
        resume_token = doc["resume_token"].GetString();

    // ck-6kvq.5: the save/world the client VOUCHES FOR.  Supplied, never derived
    // — world_identity.h explains at length why the bridge cannot work this out
    // itself.  The spelling is ck-6kvq.2's run_summary.json, so there is one:
    //   "world_identity": {"episode_id":..., "save_sha256":..., "start_date_year":N}
    //
    // Carrying it here does NOT replace `declare_save_identity`; it IS a
    // declaration, routed through the same single entry point below, because
    // ck-9fc3 says so in as many words ("its `world` field must call
    // world_identity_declare() rather than keep a parallel store").  A client
    // that prefers the standalone action still works: the resume then abstains
    // and SessionRegistry::reconcile_declared_world picks the answer up when the
    // declaration lands.
    bridge::WorldIdentity presented;
    if (doc.HasMember("world_identity") && doc["world_identity"].IsObject()) {
        const Value& w = doc["world_identity"];
        if (w.HasMember("episode_id") && w["episode_id"].IsString())
            presented.episode_id = w["episode_id"].GetString();
        if (w.HasMember("save_sha256") && w["save_sha256"].IsString())
            presented.save_sha256 = w["save_sha256"].GetString();
        if (w.HasMember("start_date_year") && w["start_date_year"].IsInt())
            presented.start_date_year =
                static_cast<uint32_t>(w["start_date_year"].GetInt());
    }
    // Fall back to whatever the harness has already declared, so a client that
    // declared through the standalone action before reconnecting is not treated
    // as having vouched for nothing.
    const bool client_declared = !presented.empty();
    if (!client_declared) presented = bridge::world_identity_declared();

    const uint64_t current_epoch = bridge::world_epoch();
    const uint64_t now_ms        = ipc_monotonic_ms();

    bridge::Session* session = nullptr;
    uint32_t slot_gen = 0;
    bool did_resume = false;
    {
        std::lock_guard<std::mutex> lock(g_client_mutex);
        ClientSlot& s = g_clients[idx];
        // ck-xqol (release racing a handshake): the frame above was parsed
        // WITHOUT this mutex. If the socket dropped in that window the slot has
        // been released — and possibly already re-accepted by a different
        // client — so binding a session here would either orphan it on a free
        // slot (nothing releases a slot that is not open, so its claims would be
        // held forever) or hand it to the newcomer. Refuse, and let the reader's
        // own gen check drop the ack.
        if (!bridge::slot_is_open(s) || s.gen != read_gen) {
            result.status = "slot_released";
            result.reason = "the slot was released while the handshake was being parsed";
            return result;
        }
        slot_gen = s.gen;
        // Clean up any previous session on this slot (shouldn't happen on a fresh
        // accept, but guards against a re-handshake on an existing connection).
        if (s.session) {
            const std::string prev_id = s.session->session_id;
            s.session = nullptr;
            bridge::g_session_registry.destroy(prev_id);
        }

        // ── RESUME (ck-6kvq.5, design §6.2) ────────────────────────────────
        // M1 parsed the token and dropped it on the floor ("reconnection logic
        // deferred", ipc_server.cpp:121).  This is that logic.
        if (!resume_token.empty()) {
            auto r = bridge::g_session_registry.resume_by_token(
                resume_token, presented, current_epoch, idx, slot_gen, now_ms);
            result.resume_status   = r.status;
            result.identity_reason = r.identity_reason;
            if (r.ok) {
                session    = r.session;
                did_resume = true;
            } else if (bridge::g_logfile) {
                fprintf(bridge::g_logfile,
                    "[CK3-Bridge] RESUME REFUSED slot=%d status=%s identity=%s "
                    "discarded_claims=%zu — minting a FRESH session instead\n",
                    idx, r.status, r.identity_reason, r.discarded_claims.size());
                fflush(bridge::g_logfile);
            }
        }

        if (!session) {
            // Legacy clients (protocol_version < 2 or absent) get an implicit
            // control session so set_controlled_character continues to claim a
            // character.
            if (protocol_version < 2 && !null_mode_downgrade) {
                session = bridge::g_session_registry.create(
                    client_name, bridge::Session::Mode::CONTROL);
            } else {
                // ck-6kvq.4: the v1 force-upgrade to CONTROL is the ONE path
                // that could smuggle a control session into a null-mode
                // episode, so the downgrade has to be checked here too — `mode`
                // alone is not consulted on the v1 branch.
                session = bridge::g_session_registry.create(client_name, mode);
            }
        }
        // ck-6kvq.4: belt and braces on the RESUME path. A resumed session
        // carries the mode it was minted with, and `mode` above was never
        // consulted for it. In `null` a control session cannot be minted in the
        // first place — so this should be unreachable. It is asserted anyway,
        // because "should be unreachable" is how a control session gets into a
        // zero-token episode and nobody notices until the data is unusable.
        if (null_mode_downgrade && session && session->is_control()) {
            session->mode = bridge::Session::Mode::OBSERVE;
            if (bridge::g_logfile) {
                fprintf(bridge::g_logfile,
                    "[CK3-Bridge][pipeline] resumed session %s was CONTROL under "
                    "pipeline mode=%s — forced to observe (%s)\n",
                    session->session_id.c_str(),
                    bridge::pipeline_mode_name(bridge::pipeline_mode()),
                    bridge::pipeline_mode_control_downgrade_reason());
                fflush(bridge::g_logfile);
            }
        }
        // ck-xqol: attach through the guarded helper so the ONE place that binds
        // a session to a slot is the one that validates the binding. Still under
        // g_client_mutex, so `slot_gen` cannot have moved since the check above;
        // the helper re-checks anyway rather than trusting the caller.
        if (!bridge::client_slot_attach_session(s, slot_gen, session,
                                                session->resume_token.c_str())) {
            const std::string orphan_id = session->session_id;
            if (did_resume) {
                // ck-6kvq.5: a RESUMED session must NOT be destroyed here.
                // resume_by_token already rebound it to (idx, slot_gen) under
                // the registry lock, and destroying it would throw away the very
                // claims the client came back for — turning a lost race into the
                // mid-episode arm change this bead exists to prevent. Put it
                // back to DETACHED instead, still holding everything, still
                // resumable on the next attempt.
                bridge::g_session_registry.detach_from_slot(
                    orphan_id, idx, slot_gen, now_ms);
            } else {
                bridge::g_session_registry.destroy(orphan_id);
            }
            result.status = "slot_released";
            result.reason = "the slot was released while the handshake was being parsed";
            return result;
        }
        // ck-6kvq.5: stamp the id the eviction path matches on. Under the same
        // mutex as the attach, and never read back off the Session pointer.
        s.session_id = session->session_id;
    }
    result.resumed = did_resume;

    if (did_resume) {
        // The slot binding was installed by resume_by_token under the REGISTRY
        // lock, so the stale-generation guard is armed from the instant the
        // rebind happened — there is no window in which an in-flight close could
        // detach the session we just handed back.
        if (bridge::g_logfile) {
            fprintf(bridge::g_logfile,
                "[CK3-Bridge] session %s RESUMED on slot=%d gen=%u (%s) — claims "
                "INTACT, primary=%u\n",
                session->session_id.c_str(), idx, slot_gen,
                result.identity_reason, session->primary_character_id);
            fflush(bridge::g_logfile);
        }
    } else {
        bridge::g_session_registry.attach_to_slot(session, idx, slot_gen, now_ms);
        bridge::g_session_registry.set_session_world(session, presented, current_epoch);
    }

    // A CLIENT-SUPPLIED identity is a declaration: route it through ck-9fc3's
    // one entry point so quarantined steering is restored (or refused) by
    // exactly the rule that governs the standalone action, and so a refuted
    // declaration also reaps the sessions whose claims it contradicts.
    if (client_declared) {
        bridge::world_identity_declare(presented);
        bridge::ipc_reconcile_declared_world(presented, current_epoch);
    }

    // A reattach cancels the detach report the abort ladder is watching.
    bridge::session_publish_detach_state();

    if (bridge::g_logfile) {
        fprintf(bridge::g_logfile,
                "[CK3-Bridge] Handshake v%d slot=%d client='%s' mode=%s session=%s\n",
                protocol_version, idx, client_name,
                bridge::session_mode_to_string(session->mode),
                session->session_id.c_str());
        fflush(bridge::g_logfile);
    }
    // ck-6kvq.9: a live session is a flush participant (early-flush condition
    // (b) and the round-robin fairness rotation). No-op in legacy mode.
    bridge::pipeline_register_session(session->session_id.c_str());
    // ck-h6nx: and if this session declared itself the episode's observer, it
    // becomes the ONE reader whose submission serves the read lane early.
    if (declares_observer) {
        // ── ck-obsv: A CONTROL SESSION IS NEVER A LEGITIMATE EPISODE OBSERVER.
        // A FLOOR UNDER AN EXPLICIT DECLARATION, NOT MODE THROUGH A SIDE DOOR,
        // and the distinction is the whole reason this is admissible: CONFERRAL
        // by mode would be non-specific — every observe session would drive the
        // cadence and any attached diagnostic would perturb the world it
        // measures. REFUSAL by mode grants nothing; the privilege is still taken
        // only by explicit first-wins declaration, and this narrows who may
        // declare.
        //
        // WHAT IT CATCHES. First-wins is safe only if EXACTLY ONE client ever
        // sends the flag, and an episode has several sessions — the meter lane
        // (observe) and the driver's own control session at least. If both send
        // it the privilege goes to whoever handshakes first, and THE WRONG
        // WINNER FAILS SILENTLY: the read-lane trigger sits on a session that
        // does no reading, reads keep stalling exactly as before the fix, and
        // `pipeline.episode_observer` names a plausible session. This converts
        // that into a loud logged refusal.
        //
        // THE MINTED MODE, not the requested one: null mode downgrades every
        // client to observe, so judging the request would refuse a session the
        // bridge then made read-only.
        // ck-m3n5: NO BRANCH HERE ANY MORE. The mode floor moved INTO
        // pipeline_declare_episode_observer, which is the only caller of
        // set_privileged_reader — so the guarantee is a property of the
        // PRIVILEGE rather than of this call site. The if/else that used to live
        // here made it a property of one caller, and a caller that logged
        // REFUSED and then declared anyway conferred the read-lane trigger on a
        // control session while ck-ce4h's lane, reading this site's source text,
        // passed 9/9.
        //
        // THE MINTED MODE, not the requested one: null mode downgrades every
        // client to observe, so judging the request would refuse a session the
        // bridge then made read-only.
        bridge::pipeline_declare_episode_observer(
            session->session_id.c_str(), client_name, session->mode);
    }
    //: ck-j6lk: RECORD THE DECLARATION BEFORE THE SESSION CAN ACCRUE ANYTHING.
    //: pipeline_note_session_open below starts the unavailability clock, and the
    //: detach path reads this flag to decide whether the session was ever a
    //: participant — so it must be true by the time either can observe it. Set on
    //: both the minted and the RESUMED path, because this runs after they join:
    //: a resumed seat re-declares on its new handshake and must not lose the flag.
    //:
    //: Only ever set to true here, never cleared: an existing declaration survives
    //: a resume that omits the field, which is the safe direction — losing it would
    //: turn a seat into a candidate for destruction.
    if (declares_seat) {
        session->declared_seat.store(true, std::memory_order_relaxed);
        if (bridge::g_logfile) {
            fprintf(bridge::g_logfile,
                "[CK3-Bridge] session %s DECLARED A SEAT (episode_seat=true) — "
                "pinned and accruing on detach even before its first claim\n",
                session->session_id.c_str());
            fflush(bridge::g_logfile);
        }
    }

    // §2.7: a CONTROL session starts accruing unavailability the moment it
    // connects — a session that handshakes and never speaks is not available.
    bridge::pipeline_note_session_open(session);

    result.session = session;
    return result;
}

// M1: embed the originating session_id into a queued action envelope so the
// game-thread drain can reinstall the owning ClientContext.
static std::string embed_session_id(const std::string& line,
                                    const char* session_id) {
    if (!session_id || !session_id[0]) return line;
    size_t pos = line.find('{');
    if (pos == std::string::npos) return line;
    std::string out;
    out.reserve(line.size() + 64);
    out.append(line, 0, pos + 1);
    out += "\"session_id\":\"";
    for (const char* p = session_id; *p; ++p) {
        if (*p == '\\' || *p == '"') out += '\\';
        out += *p;
    }
    out += "\",";
    out.append(line, pos + 1, std::string::npos);
    return out;
}
static int connected_client_count_locked() {
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) if (bridge::slot_is_open(g_clients[i])) n++;
    return n;
}

// Backpressure threshold: if the kernel send buffer has more than this many
// bytes pending, skip the write rather than blocking the game/reader thread.
// ck-rcrr: the LARGEST reply the bridge is known to produce in normal service.
// enumerate_ai_chars walks 65,869 characters and was measured at roughly this
// size in arma7. The threshold below is derived from it rather than chosen, so
// that the relationship — not the number — is what an acceptance checks.
static constexpr int LARGEST_OBSERVED_REPLY_BYTES = 66 * 1024;

// ck-rcrr: was 32768, which NORMAL TRAFFIC GRAZED. arma7 dropped a reply at
// 33792 bytes pending — over by 1024, about 3%. A limit that ordinary replies
// cross by 3% is not a flood guard; it is an intermittent abort generator, and
// on a counting run it SELECTS ON COMPLETION, which is the one thing an
// episode-level instrument must not do.
//
// Ruled by bt-bbd 2026-09-21: raise to >= 512KB, at least 8x the largest
// observed reply, so that only a genuine flood trips it. Kept as DROP-with-
// record rather than block: blocking on the serving thread is a real hang risk,
// and ck-0zfc makes every drop counted on the heartbeat and every recorded drop
// name its rid and TIOCOUTQ margin — so a residual drop is an invalid episode
// with a named cause, not a silent selection.
//
// NOT DONE HERE, and deliberately: bounded-block-before-drop (wait up to the
// client's deadline for the reader to catch up, THEN drop with record) would
// SAVE the grazing cases outright rather than merely making them rarer. It is
// noted as further hardening, is more invasive, and is not required given the
// raise plus the record.
static constexpr int BACKPRESSURE_THRESHOLD = 8 * LARGEST_OBSERVED_REPLY_BYTES;

// Write one NDJSON frame (json_data + '\n') to a single client slot.
// Validates the slot's generation under its send_mtx so a released-and-reused
// slot can never receive a stale write (no cross-client leak). Returns true
// iff fully sent. On peer write failure we do NOT mutate slot membership here
// (that needs g_client_mutex, which we intentionally don't hold during the
// blocking poll/write); the reader loop's poll() reaps the dead fd via
// POLLHUP/EOF on its next iteration. Backpressure / send-timeout simply drop
// this frame, matching the original single-client send_observation policy.
// ck-jh76: `out` (optional) records WHICH exit fired and how far the body got.
// Every `return false` below sets it, so an unset stage in a failure record
// means this function was not the one that failed — never "we did not look".
static bool send_to_slot(int idx, uint32_t gen, const char* json_data, size_t len,
                         bridge::SendOutcome* out = nullptr) {
    auto fail = [&](bridge::SendStage st, size_t written, int e) {
        if (out) { out->stage = st; out->bytes_written = written;
                   out->bytes_total = len; out->err = e; }
        return false;
    };
    if (out) { out->bytes_total = len; }
    if (idx < 0 || idx >= MAX_CLIENTS) return fail(bridge::SendStage::kSlotClosed, 0, 0);
    ClientSlot& s = g_clients[idx];
    std::lock_guard<std::mutex> sl(s.send_mtx);
    if (!bridge::slot_is_open(s) || s.gen != gen)
        return fail(bridge::SendStage::kSlotClosed, 0, 0);  // released/reused since snapshot
    int fd = s.fd;

    int pending = 0;
    if (ioctl(fd, TIOCOUTQ, &pending) == 0 && pending > BACKPRESSURE_THRESHOLD) {
        // ck-0zfc: count EVERY drop, atomically. The previous counter was a
        // function-level `static int` incremented from both the reader thread
        // and the game thread — a data race, so even the throttled number it
        // printed was not reliable. And being one static for the whole
        // function, it was shared across every slot, session and send path: the
        // "1 total dropped" in arma7 silenced the next 98 drops ANYWHERE, with
        // six agents running. The verbose line stays throttled for volume; the
        // COUNT is now on the heartbeat and cannot be silenced.
        const uint64_t n_drops =
            bridge::g_backpressure_drops.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n_drops == 1 || (n_drops % 100) == 0) {
            if (bridge::g_logfile) fprintf(bridge::g_logfile,
                "[CK3-Bridge] Backpressure: dropping message to slot=%d "
                "(%d bytes pending, threshold %d, %llu dropped process-wide; "
                "THROTTLED to the 1st and every 100th across ALL slots — see "
                "backpressure_drops on the accept-thread heartbeat for the "
                "true count) (ck-0zfc)\n",
                idx, pending, BACKPRESSURE_THRESHOLD,
                (unsigned long long)n_drops);
        }
        if (out) { out->pending_bytes   = pending;
                   out->threshold_bytes = BACKPRESSURE_THRESHOLD; }
        return fail(bridge::SendStage::kBackpressure, 0, 0);
    }

    ssize_t sent = 0;
    while (sent < static_cast<ssize_t>(len)) {
        struct pollfd pfd = {fd, POLLOUT, 0};
        int pr = poll(&pfd, 1, g_send_timeout_ms);
        if (pr <= 0 || !(pfd.revents & POLLOUT))
            return fail(sent > 0 ? bridge::SendStage::kSendError
                                 : bridge::SendStage::kPollTimeout,
                        (size_t)sent, 0);  // drop; reader reaps if dead
        ssize_t n = send(fd, json_data + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            // peer write failed; reader's poll() will HUP and release
            return fail(bridge::SendStage::kSendError, (size_t)sent, errno);
        }
        sent += n;
    }
    // The trailing newline IS the frame terminator for a line-framed client.
    // Failing here is not a dropped reply: the body is already in the client's
    // buffer and its reader will never yield it.
    struct pollfd pfd = {fd, POLLOUT, 0};
    int pr = poll(&pfd, 1, g_send_timeout_ms);
    if (pr <= 0 || !(pfd.revents & POLLOUT))
        return fail(bridge::SendStage::kNewlineFailed, (size_t)sent, 0);
    char nl = '\n';
    if (send(fd, &nl, 1, MSG_NOSIGNAL) <= 0)
        return fail(bridge::SendStage::kNewlineFailed, (size_t)sent, errno);
    if (out) { out->stage = bridge::SendStage::kOk; out->bytes_written = (size_t)sent; }
    return true;
}

// Try to process a message inline if it contains only safe read-only queries.
// Returns true if processed (response sent), false if it should be queued.
// This allows queries to be answered even when the game is paused (no on_tick).
//
// SNAPSHOT INTEGRATION (data race fix):
// When the game is UNPAUSED, parallel AI workers are mutating game objects.
// Reading those objects from this IPC thread is a data race -> crash.
//
// Query classification:
//   TIER 1 (bridge-internal): Always safe, serve directly.
//   TIER 2 (game-state): Try snapshot first. If miss, serve live ONLY if paused.
//   TIER 3 (mutating actions): Must queue for game thread. Not handled here.
// ck-324r: LEDGER THE MANAGEMENT VERBS.
//
// session(control) and session(release) leave NO ROW in actions.jsonl, and the
// reason is structural rather than an oversight. set_controlled_character and
// release_character are is_bridge_internal_query, so classify_polarity returns
// ANY; an envelope containing only them is all-ANY; pipeline_handle_envelope
// returns false for an all-ANY envelope ("its members cost the world nothing")
// and the reader's legacy ladder serves them inline. Never ticketed, so
// emit_keyed_result never runs, so no bridge_record is ever minted.
//
// The consequence is the one we most want witnessed: a REFUSED claim —
// watchlist_full, owned_by_other — is invisible to the ledger. diag25 refused
// 2578 claims and actions.jsonl records none of them.
//
// So broadcast a record of our own. NOT a second reply: the caller's answer is
// already routed to its slot, and a second frame keyed by the same request_id
// is exactly the double-answer ck-dzxr refuses. This is an OBSERVATION, which
// is what the relay reads.
//
// No ticket is invented. The frame omits the key rather than printing
// tk_00000000 for a call that was never ticketed.
static void emit_management_ledger_record(const char* verb,
                                          const char* request_id,
                                          const std::string& response,
                                          bridge::Session* session) {
    if (!bridge::is_management_ledger_verb(verb)) return;

    //: READ UNDER THE SESSION'S OWN MUTEX, then pass the values in. The frame
    //: builder is deliberately lock-free and socket-free so a host probe can
    //: obtain producer bytes with no game running; that only works if the two
    //: things it cannot reach from a header — this lock and the envelope clock —
    //: stay on this side of the call.
    std::string sid;
    uint32_t cid = 0;
    if (session) {
        std::lock_guard<std::mutex> lk(session->mutex);
        sid = session->session_id;
        cid = session->primary_character_id;
    }

    //: ck-h4pn: the frame carries the caller's id under `caller_request_id`,
    //: NEVER `request_id`. request_id is what makes a frame a REPLY: both readers
    //: pop the caller's pending future on the FIRST frame bearing the id, before
    //: any type dispatch (agent.py's _message_loop, mcp_server.py's
    //: _bridge_listener), and ck-00w7's schema states request_id is the only
    //: discriminator. The order is not a race either: this send and the handler's
    //: own reply are both in try_inline_safe_query's TIER-1 arm, this one first,
    //: no branch between them, because dispatch_action_with_response only FILLS
    //: the buffer. Cited by FUNCTION deliberately — the line numbers here went
    //: stale once already (this site is :1053 at ep-02's sha 0c24ba48), and
    //: acceptance_h4pn asserts the ordering by measurement so this prose cannot
    //: rot silently. RENAMED RATHER THAN DROPPED (ruled by bt-bbd): the pop
    //: cannot match, the real reply resolves the future, and the value survives
    //: for decision rows instead of becoming null. The relay is unaffected —
    //: records_in admits on bridge_record being a Mapping and session_id being
    //: non-empty, and only ever COPIED request_id.
    //:
    //: The COMPOSITION lives in management_ledger_frame.h and has one body, which
    //: bridge/test/mgmt_ledger_probe.cpp calls to obtain these exact bytes. It was
    //: inline here, and the probe assembled its own copy: a producer nothing
    //: compared to, beside a relay fixture hand-shaped to the consumer. Those two
    //: agreed with each other while ep-02's bundle held zero ledger rows.
    const std::string frame = bridge::build_management_ledger_frame(
        verb, request_id, response, sid, cid,
        bridge::g_tick_id.load(std::memory_order_relaxed),
        build_envelope_clock_fragment());
    if (frame.empty()) return;   //: unreachable via the guard above; not assumed

    //: ck-h4pn: BROADCAST, not send. This emitter is reached from inside
    //: try_inline_safe_query, so it runs on the READER thread with
    //: g_send_target_slot set — and send_observation would therefore unicast this
    //: record back to the agent that made the call. The record's audience is the
    //: observer, never the caller, so the routing must not depend on which thread
    //: happens to reach the emitter.
    //:
    //: WHAT THE MISROUTING DOES *NOT* EXPLAIN. ep-02 shows zero ledger frames in
    //: the sealed bundle, and it is tempting to read that as the unicast having
    //: hidden them from the relay. ck-zgh4's read refutes that: the CALLER did not
    //: receive them either — zero record-shaped replies across 584 recorded
    //: tool_results, against 1151 management dispatches. So nobody received the
    //: frames, and the misrouting is a real defect that is nevertheless NOT the
    //: cause of that zero. An earlier version of this note asserted the causal
    //: reading and a ~386 denominator; both are WITHDRAWN as unobserved. The
    //: emitter IS compiled into the .so ep-02 loaded (ck-zgh4's strings witness on
    //: the emitter-unique literal below reads 1, and 0 on the pre-ck-324r .so), and
    //: the symptom could not have shown in that population anyway: the client
    //: builds its tool_result in redesign.py's session_impl and discards the
    //: frame, and the one field the record lacks (controlled_primary) triggers a
    //: fallback that returns the right answer — so the defect was absorbed by a
    //: fallback added for an unrelated reason. Compiled in / reached / sent /
    //: received are FOUR claims; received is the one still open.
    broadcast_observation(frame.c_str(), frame.size());
}

static bool try_inline_safe_query(const std::string& msg,
                                  bridge::Session* session) {
    // Parse the envelope with rapidjson
    rapidjson::Document envelope;
    envelope.Parse(msg.c_str());
    if (envelope.HasParseError() || !envelope.IsObject()) return false;

    // Must have "actions" array
    if (!envelope.HasMember("actions") || !envelope["actions"].IsArray()) return false;

    const rapidjson::Value& actions = envelope["actions"];
    if (actions.Empty()) return false;

    bool game_paused = is_paused();

    // First pass: verify ALL actions are servable inline. Reject (→ game-thread
    // queue) if any action is a mutating game-thread action OR an UNPAUSED
    // game-graph-walking deferred read (query_dejure_progress /
    // query_conquest_targets — race-free only on the game-thread drain when the
    // sim is live). A deferred-drain action is admitted inline when it is either
    // IPC-thread-safe (is_bridge_internal_query — any state) or the game is
    // currently paused (graph-walker, paused-safe → coordinator pulses & drains).
    for (rapidjson::SizeType i = 0; i < actions.Size(); i++) {
        const rapidjson::Value& inner = actions[i];
        if (!inner.IsObject() || !inner.HasMember("action") || !inner["action"].IsString()) {
            return false;
        }
        const char* atype = inner["action"].GetString();
        bool deferred_inline_ok =
            is_deferred_drain_action(atype) &&
            (is_bridge_internal_query(atype) || game_paused);
        if (!is_bridge_internal_query(atype) &&
            !is_game_state_query(atype) &&
            !deferred_inline_ok) {
            return false;  // Not a query we can serve inline (falls through to queue)
        }
    }

    // Second pass: process each action inline
    for (volatile rapidjson::SizeType i = 0; i < actions.Size(); i++) {
        const rapidjson::Value& inner = actions[i];

        // Copy action_type to a local buffer so it survives longjmp safely
        char action_type_buf[128] = {};
        {
            const char* at = inner["action"].GetString();
            size_t at_len = inner["action"].GetStringLength();
            if (at_len >= sizeof(action_type_buf)) at_len = sizeof(action_type_buf) - 1;
            memcpy(action_type_buf, at, at_len);
        }
        if (action_type_buf[0] == '\0') continue;

        // Serialize inner action object back to JSON string for dispatch
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
        inner.Accept(writer);
        std::string inner_json(sb.GetString(), sb.GetSize());

        // Extract request_id
        char request_id[128] = {};
        if (inner.HasMember("request_id") && inner["request_id"].IsString()) {
            const char* rid_str = inner["request_id"].GetString();
            size_t rid_len = inner["request_id"].GetStringLength();
            if (rid_len >= sizeof(request_id)) rid_len = sizeof(request_id) - 1;
            memcpy(request_id, rid_str, rid_len);
            request_id[rid_len] = '\0';
        }

        std::string response_out;

        // --- TIER 0: Deferred game-thread calls — pause-transparent drain ---
        // CB validity / acceptance / trigger / decision-eval / conquest-targets
        // do their real work on the game thread inside drain_and_execute_intents,
        // which only runs while the sim ticks. run_deferred_pause_transparent()
        // records {paused,speed}, pulses unpaused so on_tick drains the request,
        // then restores {paused,speed} exactly — so these work identically from
        // either pause state, for every caller. Bounded (never hangs): a sim that
        // is not live returns a typed drain_unavailable; a wedged lock returns
        // busy_deferred. This MUST run before the Tier-1 check because the
        // bounded-poll deferred queries are also is_bridge_internal_query and would
        // otherwise be served WITHOUT the pulse (and time out while paused).
        //
        // UNPAUSED carve-out: the two game-GRAPH-walking read handlers
        // (query_dejure_progress, query_conquest_targets) traverse live game
        // objects in their own handler body. While UNPAUSED, that body racing the
        // parallel AI workers must NOT run on the IPC thread — their proven
        // race-free path is the game-thread g_msg_queue drain (which already works
        // unpaused). So when the game is unpaused we let those fall through to the
        // queue (return false → queued); only the PAUSED case (where the game
        // thread is parked) takes the pulse-and-drain path here. The bounded-poll
        // deferred queries are IPC-thread-safe (their handler only touches the
        // g_cb_query/etc. slot; the graph work is on the game thread), so they take
        // the coordinator inline in EITHER state.
        // ck-qnft: record the in-flight request for the WHOLE dispatch below,
        // before any branch is chosen. It has to cover every tier: the timeout
        // we are chasing named get_tick_info (Tier 1) while the thread was
        // plausibly occupied by something else entirely, so scoping this to the
        // slow-looking paths would miss precisely the case it exists for.
        bridge::InflightScope inflight_scope(action_type_buf, request_id);

        const bool ipc_safe_deferred = is_bridge_internal_query(action_type_buf);
        if (is_deferred_drain_action(action_type_buf) &&
            (game_paused || ipc_safe_deferred)) {
            if (bridge::g_trace && bridge::g_logfile) fprintf(bridge::g_logfile,
                "[CK3-Bridge] Inline query (deferred-drain, paused=%d): %s (request_id=%s)\n",
                game_paused ? 1 : 0, action_type_buf, request_id);
            // The dispatch closure is sigsetjmp-guarded the same way the Tier-1/2
            // paths are; the coordinator adds only the fault-guarded pause/unpause/
            // speed primitives around it.
            const char* atype_local = action_type_buf;
            const std::string& inner_json_local = inner_json;
            bridge::run_deferred_pause_transparent(
                [&](std::string* o) {
                    g_in_protected_region = true;
                    if (sigsetjmp(g_recovery_point, 1) == 0) {
                        dispatch_action_with_response(atype_local,
                                                      inner_json_local.c_str(), o);
                    } else {
                        *o = "{\"error\":\"memory_access_fault\"}";
                    }
                    g_in_protected_region = false;
                },
                &response_out);

        // --- TIER 1: Bridge-internal queries — always safe ---
        } else if (is_bridge_internal_query(action_type_buf)) {
            if (bridge::g_trace && bridge::g_logfile) fprintf(bridge::g_logfile, "[CK3-Bridge] Inline query (bridge-internal): %s (request_id=%s)\n",
                    action_type_buf, request_id);
            g_in_protected_region = true;
            if (sigsetjmp(g_recovery_point, 1) == 0) {
                dispatch_action_with_response(action_type_buf, inner_json.c_str(), &response_out);
            } else {
                response_out = "{\"error\":\"memory_access_fault\"}";
            }
            g_in_protected_region = false;
            // ck-324r: the two management verbs get a ledger row here, because
            // this is the only place they are ever served.
            emit_management_ledger_record(action_type_buf, request_id,
                                          response_out, session);

        // --- TIER 2: Game-state queries (live reads only, no snapshot) ---
        } else if (is_game_state_query(action_type_buf)) {
            if (game_paused) {
                // Game paused: no parallel workers, safe to read live memory
                if (bridge::g_trace && bridge::g_logfile) fprintf(bridge::g_logfile, "[CK3-Bridge] Inline query (paused, live): %s (request_id=%s)\n",
                        action_type_buf, request_id);
                // #93: transient-read guard. is_paused() flips the instant a pause is
                // REQUESTED, before the engine's in-flight on_tick (which launched
                // parallel AI workers) has joined. During that unpause->pause settling
                // window the live read can catch half-updated volatile fields (liege_id,
                // realm_size, domain/vassal counts) — the spurious "vassalized" /
                // transient-realm-size values the journals record then retract.
                // g_tick_id increments only at the top of on_tick and is frozen when
                // truly paused, so a change across the read means an in-flight tick
                // fired DURING it => the result may be torn. We snapshot it, read once,
                // and if the tick moved we re-read into a second buffer and only trust a
                // byte-identical pair; otherwise we flag settled=false so the agent does
                // not record the volatile values. Pure reads, bounded to one extra
                // dispatch on the rare racing case, no new engine calls.
                uint64_t t0 = bridge::g_tick_id.load(std::memory_order_acquire);
                g_in_protected_region = true;
                // savesigs=1: restore signal mask on siglongjmp so SIGSEGV isn't
                // left blocked on this thread after the recovery (matches the
                // other callsites in bridge_init.cpp / this file).
                if (sigsetjmp(g_recovery_point, 1) == 0) {
                    dispatch_action_with_response(action_type_buf, inner_json.c_str(), &response_out);
                } else {
                    response_out = "{\"error\":\"memory_access_fault\"}";
                    if (bridge::g_logfile) fprintf(bridge::g_logfile, "[CK3-Bridge] SIGSEGV in inline query: %s\n", action_type_buf);
                }
                g_in_protected_region = false;

                uint64_t t1 = bridge::g_tick_id.load(std::memory_order_acquire);
                if (t1 != t0 && !response_out.empty() &&
                    response_out.compare(0, 8, "{\"error\"") != 0) {
                    // A tick fired during the read — the snapshot may be torn. Re-read
                    // once and confirm: trust only a byte-identical pair (mutation
                    // quiesced); otherwise mark the response unsettled.
                    std::string confirm_out;
                    uint64_t t2 = bridge::g_tick_id.load(std::memory_order_acquire);
                    g_in_protected_region = true;
                    if (sigsetjmp(g_recovery_point, 1) == 0) {
                        dispatch_action_with_response(action_type_buf, inner_json.c_str(), &confirm_out);
                    } else {
                        confirm_out.clear();
                    }
                    g_in_protected_region = false;
                    uint64_t t3 = bridge::g_tick_id.load(std::memory_order_acquire);

                    bool stable = (t3 == t2) && !confirm_out.empty() &&
                                  (confirm_out == response_out);
                    if (stable) {
                        // Second read agrees and no tick crossed it — trust it.
                        response_out = confirm_out;
                    } else {
                        // Still racing: surface the latest read but flag it unsettled so
                        // the agent does not record volatile fields from it.
                        if (!confirm_out.empty() &&
                            confirm_out.compare(0, 8, "{\"error\"") != 0) {
                            response_out = confirm_out;
                        }
                        if (!response_out.empty() && response_out[0] == '{') {
                            // Inject top-level "settled":false right after the opening brace.
                            std::string flagged = "{\"settled\":false,";
                            flagged.append(response_out, 1, std::string::npos);
                            response_out.swap(flagged);
                        }
                        if (bridge::g_trace && bridge::g_logfile)
                            fprintf(bridge::g_logfile,
                                "[CK3-Bridge] transient-read guard: %s racing (t0=%llu t1=%llu t3=%llu) -> settled=false\n",
                                action_type_buf,
                                (unsigned long long)t0, (unsigned long long)t1,
                                (unsigned long long)t3);
                    }
                }
            } else {
                // Game unpaused: refuse query, tell agent to pause first
                char err[512];
                snprintf(err, sizeof(err),
                    "{\"error\":\"game_unpaused\","
                    "\"action\":\"%s\","
                    "\"hint\":\"Game is unpaused. Pause before querying game state.\"}",
                    action_type_buf);
                response_out = err;
            }
        }

        // ck-xm08: trace dispatch → inject → send_observation for bridge_timeout triage.
        // Logs at each boundary so a dropped response reveals which stage ate it.
        if (bridge::g_trace && bridge::g_logfile) {
            fprintf(bridge::g_logfile,
                "[CK3-Bridge] BT_TRACE dispatch_done: action=%s rid=%s resp_empty=%d resp_size=%zu\n",
                action_type_buf, request_id, response_out.empty() ? 1 : 0, response_out.size());
            fflush(bridge::g_logfile);
        }

        // Inject request_id + live tick/game_date (#123) into response.
        if (!response_out.empty() && response_out[0] == '{') {
            std::string injected = "{";
            if (request_id[0]) {
                injected += "\"request_id\":\"";
                injected += request_id;
                injected += "\",";
            }
            injected += build_envelope_clock_fragment();  // "tick":N,"game_date":"..."
            injected += ",";
            injected += response_out.substr(1);
            response_out = std::move(injected);
        }

        if (!response_out.empty()) {
            // ck-jh76: capture the target BEFORE the send — send_to_slot may
            // clear or the slot may be reused, and a target read afterwards
            // would describe a different state than the one that was used.
            const int      rr_slot = g_send_target_slot;
            const uint32_t rr_gen  = g_send_target_gen;
            bridge::SendOutcome rr_out;
            g_last_send_outcome = &rr_out;
            bool sent_ok = send_observation(response_out.c_str(), response_out.size());
            g_last_send_outcome = nullptr;
            // ck-55dx: the elapsed comes from ck-qnft's in-flight record, which
            // already stamped this request's start on the same CLOCK_MONOTONIC
            // the log prefix uses. If the seqlock read is not stable, or the
            // slot is no longer busy, the elapsed is UNKNOWN and says so — it
            // is never reported as 0, which would read as an instant reply.
            const bridge::InflightView rr_v =
                bridge::inflight_read(bridge::inflight_now_ms());
            const uint64_t rr_elapsed = (rr_v.stable && rr_v.busy)
                                      ? rr_v.age_ms : bridge::kElapsedUnknown;
            bridge::record_reply(action_type_buf, request_id, sent_ok,
                                 response_out.size(), rr_slot, rr_gen, &rr_out,
                                 rr_elapsed);
        } else {
            bridge::record_empty_reply(action_type_buf, request_id);
        }
    }

    return true;
}

// ck-09q4u.9: Tier-3 paused-precondition rejection.
//
// Tier-3 game-thread actions (currently change_realm_law — see
// is_game_thread_action in snapshot.cpp) are not safe to execute inline from
// the reader thread; they call engine functions that must run on the game
// thread. The reader queues them to g_msg_queue and on_tick drains the queue.
//
// But on_tick only fires while the game is unpaused. If the agent dispatches
// a Tier-3 action while the game is paused, the message sits in g_msg_queue
// forever and the agent times out — with no signal from the bridge about why.
//
// Worse: do_unpause_game returns success even when the underlying
// game_unpause() silently no-ops (CJominiIdler singleton null in
// load/menu/transition states). So an agent that "unpaused" right before the
// Tier-3 action can be paused without knowing it.
//
// This helper catches both cases: it inspects the envelope and, if any action
// is classified is_game_thread_action AND the game is currently paused,
// rejects the entire envelope inline with a structured `game_paused` error.
// The agent gets fast, actionable feedback ("unpause first, then retry")
// instead of a 10s timeout.
//
// Returns true if the envelope was handled (rejection sent), false if the
// caller should fall through to its normal queue path.
static bool try_game_thread_paused_rejection(const std::string& msg) {
    rapidjson::Document envelope;
    envelope.Parse(msg.c_str());
    if (envelope.HasParseError() || !envelope.IsObject()) return false;
    if (!envelope.HasMember("actions") || !envelope["actions"].IsArray()) return false;

    const rapidjson::Value& actions = envelope["actions"];
    if (actions.Empty()) return false;

    // Require ALL inner actions to be game-thread actions. Mixed envelopes are
    // exotic in practice (the BridgeSession + LLM agent both send single-action
    // envelopes) and we don't want to partially-reject a multi-action batch.
    for (rapidjson::SizeType i = 0; i < actions.Size(); i++) {
        const rapidjson::Value& inner = actions[i];
        if (!inner.IsObject() || !inner.HasMember("action") || !inner["action"].IsString()) {
            return false;
        }
        if (!is_game_thread_action(inner["action"].GetString())) {
            return false;
        }
    }

    // All actions are game-thread. If the game is unpaused, on_tick will
    // drain normally — fall through to the queue.
    if (!is_paused()) return false;

    // Game is paused: emit one rejection per inner action, preserving
    // request_id when supplied (matches the inline path in
    // try_inline_safe_query).
    for (rapidjson::SizeType i = 0; i < actions.Size(); i++) {
        const rapidjson::Value& inner = actions[i];
        const char* action_type = inner["action"].GetString();

        char request_id[128] = {};
        if (inner.HasMember("request_id") && inner["request_id"].IsString()) {
            const char* rid_str = inner["request_id"].GetString();
            size_t rid_len = inner["request_id"].GetStringLength();
            if (rid_len >= sizeof(request_id)) rid_len = sizeof(request_id) - 1;
            memcpy(request_id, rid_str, rid_len);
            request_id[rid_len] = '\0';
        }

        std::string resp;
        resp.reserve(256);
        resp += '{';
        if (request_id[0]) {
            resp += "\"request_id\":\"";
            resp += request_id;
            resp += "\",";
        }
        resp += build_envelope_clock_fragment();  // #123: "tick":N,"game_date":"..."
        resp += ',';
        resp += "\"success\":false,\"action\":\"";
        resp += action_type;
        resp += "\",\"reason\":\"game_paused\","
                "\"hint\":\"Tier-3 action requires unpaused game; "
                "on_tick drains the queue while ticking. "
                "Call unpause_game (and verify is_paused() flipped) before retrying.\"}";

        send_observation(resp.c_str(), resp.size());

        if (bridge::g_logfile) {
            fprintf(bridge::g_logfile,
                "[CK3-Bridge] Tier-3 paused-precondition reject: action=%s rid=%s "
                "(on_tick won't drain while paused)\n",
                action_type, request_id[0] ? request_id : "(none)");
            fflush(bridge::g_logfile);
        }
    }

    return true;
}

// D1-C + N1 + N2 (bridge-investigation-20260801): last-line guard on the
// g_msg_queue fall-through.
//
// Anything that reaches this point would be pushed to g_msg_queue, which
// on_tick drains ONLY while the game is unpaused. Two hang classes lived here:
//
//   D1-C: an UNREGISTERED action name (query_titles, overlay's query_title,
//   any typo) has no handler at all — queued while paused it never drains and
//   the caller times out with no reply; even unpaused it wastes a tick just to
//   learn "unknown_action". We fast-fail these inline in ANY pause state via
//   is_registered_action() (dispatch_router.cpp — generated from the same
//   table as the dispatch chain, so it cannot drift). Deliberately NOT gated
//   on the snapshot.cpp classifier lists: legit Tier-3/unclassified actions
//   must still queue.
//
//   N1: a REGISTERED action in no classifier (the 22-name audit batch, now
//   classified — this guard is the belt-and-braces for the NEXT unclassified
//   one) queued while paused also never drains. While paused we reject it
//   inline with the same typed `game_paused` shape the Tier-3 rejection uses,
//   so the CLASS of silent paused hangs is dead even for future omissions.
//   While UNPAUSED, registered actions fall through untouched — queueing
//   behavior is byte-identical to before.
//
//   N2 rider: "handshake" sent as an ACTION lands here too (it is not a
//   registered action) and gets a typed redirect instead of the historical
//   silent stall.
//
// Envelopes are consumed whole or not at all (mirrors
// try_game_thread_paused_rejection): a partially-rejected envelope would
// still queue and the drain would then emit duplicate replies for the
// rejected members. Concretely: while paused every member gets a typed reply
// (unknown_action or game_paused); while unpaused the envelope is consumed
// only when ALL members are unregistered (pure unknown_action batch), else it
// falls through to the queue exactly as before.
//
// Returns true if the envelope was handled (replies sent), false if the
// caller should fall through to its normal queue path.
static bool try_queue_fallthrough_rejection(const std::string& msg) {
    rapidjson::Document envelope;
    envelope.Parse(msg.c_str());
    if (envelope.HasParseError() || !envelope.IsObject()) return false;
    if (!envelope.HasMember("actions") || !envelope["actions"].IsArray()) return false;

    const rapidjson::Value& actions = envelope["actions"];
    if (actions.Empty()) return false;

    const bool game_paused = is_paused();

    bool all_unregistered = true;
    for (rapidjson::SizeType i = 0; i < actions.Size(); i++) {
        const rapidjson::Value& inner = actions[i];
        if (!inner.IsObject() || !inner.HasMember("action") || !inner["action"].IsString()) {
            return false;
        }
        if (is_registered_action(inner["action"].GetString())) {
            all_unregistered = false;
        }
    }

    // Unpaused + at least one registered member: the queue drains normally on
    // the next tick — keep today's behavior exactly (a mixed envelope's
    // unknown members still get their unknown_action from the drain).
    if (!game_paused && !all_unregistered) return false;

    // Consume the envelope: one typed reply per inner action, preserving
    // request_id when supplied (matches try_game_thread_paused_rejection).
    for (rapidjson::SizeType i = 0; i < actions.Size(); i++) {
        const rapidjson::Value& inner = actions[i];
        const char* action_type = inner["action"].GetString();
        const bool registered = is_registered_action(action_type);

        char request_id[128] = {};
        if (inner.HasMember("request_id") && inner["request_id"].IsString()) {
            const char* rid_str = inner["request_id"].GetString();
            size_t rid_len = inner["request_id"].GetStringLength();
            if (rid_len >= sizeof(request_id)) rid_len = sizeof(request_id) - 1;
            memcpy(request_id, rid_str, rid_len);
            request_id[rid_len] = '\0';
        }

        std::string resp;
        resp.reserve(384);
        resp += '{';
        if (request_id[0]) {
            resp += "\"request_id\":\"";
            resp += request_id;
            resp += "\",";
        }
        resp += build_envelope_clock_fragment();  // #123: "tick":N,"game_date":"..."
        resp += ',';
        if (!registered) {
            // D1-C fast-fail: same shape as dispatch_action's unknown tail.
            resp += "\"success\":false,\"error\":\"unknown_action\",\"action\":\"";
            resp += action_type;
            resp += '"';
            if (strcmp(action_type, "handshake") == 0) {
                // N2: misframed handshake-as-action — typed redirect.
                resp += ",\"hint\":\"handshake is a frame type, not an action: "
                        "send {\\\"type\\\":\\\"handshake\\\"} as its own NDJSON line.\"";
            }
            resp += '}';
        } else {
            // N1 belt-and-braces: registered but headed for g_msg_queue while
            // paused — on_tick will not drain it, so refuse loudly now.
            resp += "\"success\":false,\"action\":\"";
            resp += action_type;
            resp += "\",\"reason\":\"game_paused\","
                    "\"hint\":\"This action drains on the game thread via the "
                    "message queue, which does not run while the game is paused. "
                    "Call unpause_game (and verify paused=false via get_tick_info) "
                    "before retrying.\"}";
        }

        send_observation(resp.c_str(), resp.size());

        if (bridge::g_logfile) {
            fprintf(bridge::g_logfile,
                "[CK3-Bridge] Queue fall-through reject: action=%s rid=%s %s (paused=%d)\n",
                action_type, request_id[0] ? request_id : "(none)",
                registered ? "game_paused" : "unknown_action",
                game_paused ? 1 : 0);
            fflush(bridge::g_logfile);
        }
    }

    return true;
}

// Background reader thread — polls ALL connected client slots, reads NDJSON,
// answers inline (routing the reply to the originating slot via the
// thread-local target) or queues mutating actions for on_tick.
static void reader_loop() {
    bridge::register_thread(bridge::ThreadRole::IpcReader);   // ck-duv5
    // Mask SIGALRM on this thread — the watchdog timer targets the game thread
    // via SIGEV_THREAD_ID, but as belt-and-suspenders we block SIGALRM here
    // so it cannot be delivered to the reader thread even if alarm() is used.
    {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGALRM);
        pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    }

    static constexpr size_t READ_BUF_SIZE = 65536;
    char read_buf[READ_BUF_SIZE];

    // Reader-thread-local per-slot framing + keepalive state. seen_gen detects
    // slot reuse (a released slot re-adopted by a new client) and resets the
    // accumulator so frames from the old and new client never run together.
    std::string line_bufs[MAX_CLIENTS];
    int idle_cycles[MAX_CLIENTS] = {0};
    uint32_t seen_gen[MAX_CLIENTS] = {0};

    auto reset_if_reused = [&](int idx, uint32_t cur_gen) {
        if (seen_gen[idx] != cur_gen) {
            seen_gen[idx] = cur_gen;
            line_bufs[idx].clear();
            idle_cycles[idx] = 0;
        }
    };

    while (g_server_running.load()) {
        // Snapshot live slot fds + gens for poll().
        struct pollfd pfds[MAX_CLIENTS];
        int      slot_of[MAX_CLIENTS];
        uint32_t gen_of[MAX_CLIENTS];
        int npf = 0;
        {
            std::lock_guard<std::mutex> lock(g_client_mutex);
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (bridge::slot_is_open(g_clients[i])) {
                    pfds[npf].fd = g_clients[i].fd;
                    pfds[npf].events = POLLIN;
                    pfds[npf].revents = 0;
                    slot_of[npf] = i;
                    gen_of[npf] = g_clients[i].gen;
                    npf++;
                }
            }
        }
        if (npf == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        int ret = poll(pfds, npf, 200);  // 200ms timeout
        if (ret < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ret == 0) {
            // Keepalive sweep: bump each live slot's idle counter; every ~5s
            // (25 * 200ms) probe peer liveness and ping. Collect ping targets
            // under the lock, then send without it (send_to_slot takes send_mtx
            // and can block up to g_send_timeout_ms).
            int ping_idx[MAX_CLIENTS]; uint32_t ping_gen[MAX_CLIENTS]; int np = 0;
            {
                std::lock_guard<std::mutex> lock(g_client_mutex);
                for (int p = 0; p < npf; p++) {
                    int idx = slot_of[p];
                    if (!bridge::slot_is_open(g_clients[idx]) ||
                        g_clients[idx].gen != gen_of[p]) continue;
                    reset_if_reused(idx, g_clients[idx].gen);
                    if (++idle_cycles[idx] >= 25) {
                        idle_cycles[idx] = 0;
                        if (g_clients[idx].peer_pid > 0 &&
                            !ipc_peer_pid_alive(g_clients[idx].peer_pid)) {
                            if (bridge::g_logfile) fprintf(bridge::g_logfile,
                                "[CK3-Bridge] Keepalive: slot=%d peer_pid=%d gone (ESRCH), releasing\n",
                                idx, g_clients[idx].peer_pid);
                            release_slot_locked(idx, "peer pid ESRCH");
                            continue;
                        }
                        ping_idx[np] = idx; ping_gen[np] = g_clients[idx].gen; np++;
                    }
                }
            }
            const char ping_msg[] = "{\"type\":\"ping\"}";
            for (int i = 0; i < np; i++) {
                bridge::SendOutcome po;
                const bool ok = send_to_slot(ping_idx[i], ping_gen[i], ping_msg,
                                             sizeof(ping_msg) - 1, &po);
                bridge::record_ping(ping_idx[i], ok, po);
            }
            continue;
        }

        // Data ready on one or more slots — service each.
        for (int p = 0; p < npf; p++) {
            int idx = slot_of[p];
            short re = pfds[p].revents;
            if (re == 0) continue;

            // Re-validate the slot still holds the fd/gen we polled.
            int cur_fd; uint32_t cur_gen;
            {
                std::lock_guard<std::mutex> lock(g_client_mutex);
                cur_fd = g_clients[idx].fd;
                cur_gen = g_clients[idx].gen;
            }
            if (cur_fd < 0 || cur_gen != gen_of[p]) continue;  // released/reused
            reset_if_reused(idx, cur_gen);

            if (re & (POLLERR | POLLHUP | POLLNVAL)) {
                std::lock_guard<std::mutex> lock(g_client_mutex);
                if (g_clients[idx].fd == cur_fd && g_clients[idx].gen == cur_gen)
                    release_slot_locked(idx, "poll POLLERR/HUP/NVAL");
                line_bufs[idx].clear();
                continue;
            }
            if (!(re & POLLIN)) continue;

            ssize_t n = ::read(cur_fd, read_buf, sizeof(read_buf));
            if (n <= 0) {
                std::lock_guard<std::mutex> lock(g_client_mutex);
                if (g_clients[idx].fd == cur_fd && g_clients[idx].gen == cur_gen)
                    release_slot_locked(idx, "reader EOF / error");
                line_bufs[idx].clear();
                continue;
            }
            idle_cycles[idx] = 0;

            // Route every response produced while processing this slot's lines
            // back to this slot (handshake_ack, inline query results, errors).
            g_send_target_slot = idx;
            g_send_target_gen = cur_gen;
            std::string& line_buf = line_bufs[idx];

            for (ssize_t i = 0; i < n; i++) {
                if (read_buf[i] != '\n') { line_buf += read_buf[i]; continue; }
                if (line_buf.empty()) continue;

                // N2 (surface audit 2026-08-01): an ACTIONS envelope is never a
                // handshake. Without the "actions" exclusion, a misframed
                // {"actions":[{"action":"handshake",...}]} whose text also
                // contained "type" matched this substring test, got a plain
                // handshake_ack (no request_id), and the caller stalled forever
                // waiting for a request_id-matched reply — in ANY pause state.
                // Such envelopes now flow down the normal action path, where the
                // unregistered-name guard answers them with a typed redirect.
                bool is_handshake = (line_buf.find("\"type\"") != std::string::npos &&
                                     line_buf.find("\"handshake\"") != std::string::npos &&
                                     line_buf.find("\"handshake_ack\"") == std::string::npos &&
                                     line_buf.find("\"actions\"") == std::string::npos);

                if (is_handshake) {
                    if (bridge::g_logfile) fprintf(bridge::g_logfile, "[CK3-Bridge] Handshake received (slot=%d): %s\n", idx, line_buf.c_str());

                    // M1: parse handshake params and create/attach a session to this slot.
                    HandshakeResult hs_result = handle_handshake_for_slot(idx, cur_gen, line_buf);
                    const char* session_id = hs_result.session ? hs_result.session->session_id.c_str() : "";
                    // ck-6kvq.6: echo the mode the bridge ACTUALLY minted. A v1
                    // (legacy) handshake always yields "control" even if it named a
                    // mode, so a client cannot otherwise tell whether its observe
                    // request was honoured. Empty when no session was minted.
                    const char* session_mode = hs_result.session
                        ? bridge::session_mode_to_string(hs_result.session->mode) : "";
                    // ck-6kvq.5: hand back the SERVER-MINTED resume token.  A
                    // client cannot resume a session whose token it was never
                    // given, which is the point — the token is an authorisation
                    // over a claim set, not a label.
                    const char* resume_token = hs_result.session
                        ? hs_result.session->resume_token.c_str() : "";

                    // ck-nkyc leg (e): WHAT WE ACTUALLY SAW, beside what we
                    // expected. `binary_build_id` keeps its name and its
                    // meaning — ck3_version::BUILD_ID_HEX, the EXPECTED
                    // constant the offsets were cut against — because renaming
                    // a wire field is its own hazard. `observed_build_id` is
                    // the new one, and it is the only value in this ack that is
                    // unique to WHAT RAN rather than to a tree.
                    const bridge::BuildIdWitness& bidw = bridge::build_id_witness();

                    // Grown from 2048 with the six ck-nkyc fields: a truncated
                    // ack is a SILENT provenance failure, which is the exact
                    // class of defect this bead exists to end.
                    char ack_buf[4096];
                    const int ack_len = snprintf(ack_buf, sizeof(ack_buf),
                        "{\"type\":\"handshake_ack\",\"bridge_version\":\"v8-2026-03-20\","
                        "\"binary_build_id\":\"%s\","
                        "\"observed_build_id\":\"%s\","
                        "\"build_id_status\":\"%s\","
                        "\"build_id_strict_effective\":%s,"
                        "\"build_id_strict_compiled\":%s,"
                        "\"build_id_strict_env\":\"%s\","
                        "\"build_id_loosen_attempted\":%s,"
                        "\"offset_revision\":%d,"
                        "\"offset_date\":\"%s\","
                        "\"build_git_sha\":\"%s\","
                        "\"build_git_branch\":\"%s\","
                        "\"build_git_dirty\":\"%s\","
                        "\"build_offsets_md5\":\"%s\","
                        "\"build_time\":\"%s\","
                        "\"status\":\"%s\","
                        "\"reason\":\"%s\","
                        "\"session_id\":\"%s\","
                        "\"session_mode\":\"%s\","
                        // ck-1ek9: WHICH MODE MINTED THAT session_mode.
                        // `session_mode` alone cannot answer the question a
                        // client actually has, which is "can I act?" — an
                        // observe session in `null` MAY set the clock
                        // (mode_admits_observer_clock_verbs) and one in any
                        // other mode may not. Without this field the only way
                        // to tell those apart is to ISSUE A VERB AND SEE, and
                        // a client that must act to discover it cannot refuse
                        // before acting. The mode is not a secret — it is
                        // already on every get_tick_info — this just puts it
                        // where the decision is made.
                        "\"pipeline_mode\":\"%s\","
                        "\"session_state\":\"%s\","
                        "\"resume_token\":\"%s\","
                        "\"resumed\":%s,"
                        "\"resume_status\":\"%s\","
                        "\"resume_identity_reason\":\"%s\"}",
                        ck3_version::BUILD_ID_HEX,
                        bidw.observed_hex.c_str(),
                        bridge::build_id_check_string(bidw.status),
                        bidw.strict_effective ? "true" : "false",
                        bidw.strict_compiled ? "true" : "false",
                        bidw.strict_env.c_str(),
                        bidw.loosen_attempted ? "true" : "false",
                        ck3_version::OFFSET_REVISION,
                        ck3_version::OFFSET_DATE,
                        bridge_provenance::GIT_SHA,
                        bridge_provenance::GIT_BRANCH,
                        bridge_provenance::GIT_DIRTY,
                        bridge_provenance::OFFSETS_MD5,
                        bridge_provenance::BUILD_TIME,
                        hs_result.status,
                        hs_result.reason,
                        session_id,
                        session_mode,
                        bridge::pipeline_mode_name(bridge::pipeline_mode()),
                        session_state_string(),
                        resume_token,
                        hs_result.resumed ? "true" : "false",
                        hs_result.resume_status,
                        hs_result.identity_reason);
                    // ck-nkyc: a TRUNCATED ack is a silent provenance failure —
                    // the client would parse a partial JSON object, or none, and
                    // the gate that is supposed to refuse the episode would
                    // instead see "the bridge reported nothing". snprintf tells
                    // us what it WOULD have written, so say so out loud.
                    if (ack_len < 0 || (size_t)ack_len >= sizeof(ack_buf)) {
                        if (bridge::g_logfile)
                            fprintf(bridge::g_logfile,
                                    "[CK3-Bridge] FATAL: handshake_ack TRUNCATED "
                                    "(needed %d bytes, buffer %zu) — the client "
                                    "will see no build provenance\n",
                                    ack_len, sizeof(ack_buf));
                    }
                    // Targeted reply to the connecting slot only (not broadcast).
                    send_to_slot(idx, cur_gen, ack_buf, strlen(ack_buf));
                    if (hs_result.session) {
                        std::lock_guard<std::mutex> lock(g_client_mutex);
                        if (g_clients[idx].gen == cur_gen) g_clients[idx].handshaked = true;
                    }
                    if (bridge::g_logfile) fprintf(bridge::g_logfile, "[CK3-Bridge] Handshake acknowledged (slot=%d, status=%s, session=%s)\n", idx, hs_result.status, session_id);

                    // Refused handshakes close the slot after the error ack is sent.
                    if (strcmp(hs_result.status, "ok") != 0) {
                        std::lock_guard<std::mutex> lock(g_client_mutex);
                        if (g_clients[idx].gen == cur_gen) release_slot_locked(idx, "handshake refused");
                    }
                } else {
                    // M1: install the slot's session as the current request context
                    // for any inline dispatch (read-only queries resolve to the
                    // session's primary; queued mutations carry session_id and are
                    // reinstalled on the game thread).
                    bridge::Session* slot_session = nullptr;
                    {
                        std::lock_guard<std::mutex> lock(g_client_mutex);
                        if (g_clients[idx].gen == cur_gen) slot_session = g_clients[idx].session;
                    }
                    bridge::ClientContext ctx;
                    ctx.session = slot_session;
                    ctx.slot = nullptr;  // IPC thread; no slot back-pointer needed here
                    bridge::ClientContextGuard ctx_guard(&ctx);

                    // §2.7: ANY submission OR read from a control session resets
                    // its silence. Before the legacy/pipeline fork on purpose —
                    // a read served by the synchronous fast path is still
                    // activity, and an experiment must not record a hole that
                    // exists only because the agent polled instead of acting.
                    bridge::pipeline_note_session_activity(slot_session);

                    // ck-6kvq.9: the pipeline conductor's first stop. In
                    // LEGACY mode (the default, G11) this is one relaxed atomic
                    // load and a `return false`, so the ladder below runs
                    // exactly as it did before — that indistinguishability is
                    // the acceptance gate for the bead, not an aspiration.
                    // In pipeline mode it buffers the call and returns a ticket,
                    // EXCEPT for ANY-polarity calls, which fall through to the
                    // synchronous inline path below because the whole ops ladder
                    // polls them (ck3_game_selfheal.sh, operator_page.sh,
                    // overlay_poller.py, run_monitor.py).
                    // ── ck-dzxr: REFUSE A MALFORMED ENVELOPE, DO NOT QUEUE IT ──
                    // Every handler in the ladder below requires a top-level
                    // "actions" ARRAY. An envelope without one — a bare
                    // {"action":"..."}, an {"action_type":...}, a typo in the key
                    // — failed all of them and fell through to g_msg_queue, where
                    // it sat until the game thread drained it and produced
                    // nothing the sender could match.
                    //
                    // WHY THAT IS WORSE THAN AN ERROR. The queue's replies are
                    // BROADCAST (the game thread has no send target; clients match
                    // by request_id). So a caller whose envelope was never
                    // understood does not get silence — it gets the NEXT
                    // broadcast, which is another request's answer, well-formed
                    // and complete and about something else entirely.
                    //
                    // ck-zgh4 hit exactly this probing a live world: two malformed
                    // envelopes each came back with a full query_character payload
                    // for Harold Godwinson. They read a field off it, wrote the
                    // OPPOSITE of the truth into a report, and caught it only
                    // because they recognised the character name. A protocol that
                    // answers a question you did not ask, in the right shape, is
                    // the plausible-wrong-answer family at the transport layer.
                    //
                    // The refusal names the key that was missing, so the caller
                    // learns what to fix rather than that "something" was wrong.
                    if (!bridge::envelope_has_actions_array(line_buf.c_str())) {
                        char err[320];
                        snprintf(err, sizeof err,
                            "{\"success\":false,\"error\":\"malformed_envelope\","
                            "\"reason\":\"no top-level \\\"actions\\\" array\","
                            "\"hint\":\"send {\\\"actions\\\":[{\\\"action\\\":...,"
                            "\\\"params\\\":...,\\\"request_id\\\":...}]} after the "
                            "handshake; replies are matched by request_id\"}");
                        send_observation(err, strlen(err));
                        if (bridge::g_logfile) fprintf(bridge::g_logfile,
                            "[CK3-Bridge] REFUSED a malformed envelope on slot=%d "
                            "(no \"actions\" array) — refusing rather than queueing, "
                            "because a queued envelope's caller reads the next "
                            "BROADCAST as its own answer (ck-dzxr)\n", idx);
                    } else if (bridge::pipeline_handle_envelope(line_buf, slot_session)) {
                        // Buffered + acknowledged (or answered, for `check`).
                    } else if (try_inline_safe_query(line_buf, slot_session)) {
                        // Processed inline — reply already routed to this slot.
                    } else if (try_game_thread_paused_rejection(line_buf)) {
                        // Tier-3 action while paused — rejected inline.
                    } else {
                        SessionState state = get_session_state();
                        if (state != SessionState::ACTIVE) {
                            char err_buf[256];
                            snprintf(err_buf, sizeof(err_buf),
                                "{\"error\":\"game_not_loaded\",\"state\":\"%s\"}",
                                session_state_string());
                            send_observation(err_buf, strlen(err_buf));
                            if (bridge::g_logfile) fprintf(bridge::g_logfile, "[CK3-Bridge] Rejected action (slot=%d state=%s)\n",
                                    idx, session_state_string());
                        } else if (try_queue_fallthrough_rejection(line_buf)) {
                            // D1-C/N1/N2: unregistered names answered inline
                            // with unknown_action (any pause state); registered
                            // queue-bound actions while paused answered with a
                            // typed game_paused instead of parking silently in
                            // g_msg_queue until unpause.
                        } else {
                            // Queue for on_tick. The drained response is broadcast
                            // (game thread has no send target); the originating
                            // client matches it by request_id.
                            std::string session_id;
                            {
                                std::lock_guard<std::mutex> lock(g_client_mutex);
                                if (g_clients[idx].gen == cur_gen && g_clients[idx].session)
                                    session_id = g_clients[idx].session->session_id;
                            }
                            std::string tagged = embed_session_id(line_buf, session_id.c_str());
                            std::lock_guard<std::mutex> lock(g_queue_mutex);
                            g_msg_queue.push_back(std::move(tagged));
                            g_queue_cv.notify_one();
                        }
                    }

                    // R1c (ck-6kvq.10, design §2.9): the READER-THREAD anchor of
                    // the pulse ledger. This envelope's inline dispatch is over,
                    // so any pulse THIS thread still has open is leaked — the
                    // same argument as the drain anchor in bridge_init.cpp, and
                    // the one that covers the ~21 ALLOW_RANDOM_IN_SCOPE pulses
                    // that happen on this thread (eight of which cannot restore
                    // on their own crash path at all, because they arm AFTER
                    // their sigsetjmp).
                    bridge::pulse_sweep_ipc();
                }
                line_buf.clear();
            }
            g_send_target_slot = -1;
        }
    }
}

// ck-6kvq.5 / §6.2: the max_detach_s bound.
//
// The BOUND is ck-6kvq.1's, and its authority is the episode manifest
// (`bounds.max_detach_s`, default 900s) read by scripts/episode_abort.sh.  The
// bridge cannot parse the manifest, so it reads the SAME env override and falls
// back to the SAME default — one number, one spelling, no third name.
//
// What happens on expiry is deliberately small: the session is marked LOST and
// published in the detach report.  It is NOT released and NOT destroyed.  The
// episode is terminated from outside, by `episode_abort.sh check`, with reason
// `session_lost`.  Releasing the claims here would hand a claimed character back
// to the native AI in the middle of a frozen arm — the mid-episode condition
// change R3 forbids — with nobody connected to see it happen.
static uint64_t ipc_max_detach_ms() {
    const char* e = getenv("CK3_EP_MAX_DETACH_S");
    if (e && e[0]) {
        char* end = nullptr;
        long v = strtol(e, &end, 10);
        if (end && *end == '\0' && v >= 0) return static_cast<uint64_t>(v) * 1000ull;
    }
    return 900ull * 1000ull;   // episode_abort.sh's `bound max_detach_s 900`
}

static void sweep_detached_sessions() {
    std::vector<std::string> lost = bridge::g_session_registry.sweep_detached(
        ipc_monotonic_ms(), ipc_max_detach_ms());
    for (const std::string& sid : lost) {
        if (bridge::g_logfile) {
            fprintf(bridge::g_logfile,
                "[CK3-Bridge] max_detach_s EXCEEDED for session %s — REPORTED as "
                "lost (claims retained, ai_ctrl+0x28 untouched). The episode is "
                "terminated by episode_abort.sh with reason=session_lost; the "
                "bridge releases nothing here.\n",
                sid.c_str());
            fflush(bridge::g_logfile);
        }
        // ck-y6fp: and if THAT session is the one that opened the episode, the
        // clock hold is re-armed here. This is the EXPERIMENT-mode half of the
        // trigger: a dropped socket detaches rather than destroying, so
        // pipeline_note_session_ended never fires for it and the bound that
        // decides "not coming back" is this sweep's own max_detach_s.
        bridge::pipeline_note_episode_owner_gone(sid.c_str(), "session_lost");
    }
    // Publish unconditionally: this is also what keeps the epoch stamp in
    // state/detached-since current while a detach is outstanding, and what
    // REMOVES the file once the last session has come back.
    bridge::session_publish_detach_state();
}

// Server accept loop — runs on a background thread
static void server_loop() {
    bridge::register_thread(bridge::ThreadRole::IpcServer);   // ck-duv5
    // Mask SIGALRM on this thread — same rationale as reader_loop()
    {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGALRM);
        pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    }

    // Heartbeat cadence: the poll() below has a 1s timeout, so emit a liveness
    // line once per HEARTBEAT_ITERS iterations even when no client shows up.
    // This exists specifically to diagnose ck-7tzu: if the log tail stops here
    // we know the accept-thread itself is wedged; if the log keeps emitting
    // heartbeats but connect() still returns EAGAIN, the hang is somewhere
    // else (kernel queue, fd leak, etc.).
    constexpr int HEARTBEAT_ITERS = 60;  // ~60s
    int iters = 0;

    while (g_server_running.load()) {
        struct pollfd pfd;
        pfd.fd = g_server_fd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 1000); // 1 second timeout
        g_accept_loop_last_poll_ts.store(ipc_monotonic_secs(), std::memory_order_relaxed);

        // ck-6kvq.5: the max_detach_s sweep.  Driven from HERE because this is
        // the one loop that runs at a fixed cadence in every mode and does not
        // depend on the game ticking — and a detached session is exactly the
        // case where the world may be doing anything at all, including nothing.
        // Driving it from the conductor or from on_tick would mean a bound that
        // exists to catch an absent agent could only be checked by a moving
        // world.
        sweep_detached_sessions();

        // ── ck-qnft: THE SLOW-REQUEST WATCHER ───────────────────────────────
        // Here, and not in reader_loop, because reader_loop is the thread that
        // gets stuck: a report written by the stuck thread is written after it
        // is unstuck, which is exactly too late. This loop polls on a 1s
        // timeout and is not blocked by it, so it can describe the stall WHILE
        // the stall is happening — and before the driver's 15s deadline.
        //
        // Two thresholds so the log carries a TREND, not a single line: one
        // request logged twice with a growing age distinguishes "slow" from
        // "wedged", and a request that clears between them says so by never
        // reaching the second. Both fire before 15s on purpose.
        {
            const uint64_t now = bridge::inflight_now_ms();
            const bridge::InflightView v = bridge::inflight_read(now);
            static uint64_t s_warned_at_ms = 0;
            static char     s_warned_rid[bridge::kInflightNameMax] = {0};
            if (v.stable && v.busy) {
                const bool new_request = std::strcmp(s_warned_rid, v.request_id) != 0;
                if (new_request) { s_warned_at_ms = 0; }
                const uint64_t tier = (v.age_ms >= 10000) ? 10000
                                    : (v.age_ms >=  5000) ?  5000 : 0;
                if (tier != 0 && tier > s_warned_at_ms) {
                    char ib[512];
                    if (bridge::g_logfile) {
                        fprintf(bridge::g_logfile,
                            "[CK3-Bridge] SLOW REQUEST (>=%llus on the serving "
                            "thread; the client deadline is 15s): %s\n",
                            (unsigned long long)(tier / 1000),
                            bridge::format_inflight(ib, sizeof ib, v,
                                                    bridge::drain_lock_state(), now));
                        fflush(bridge::g_logfile);
                    }
                    s_warned_at_ms = tier;
                    snprintf(s_warned_rid, sizeof s_warned_rid, "%s", v.request_id);
                }
            } else if (v.stable && !v.busy) {
                s_warned_at_ms = 0;
                s_warned_rid[0] = '\0';
            }
        }

        if (++iters >= HEARTBEAT_ITERS) {
            iters = 0;
            if (bridge::g_logfile) {
                // ck-jh76: the reply counters ride the heartbeat so that a
                // QUIET log is still readable. "No REPLY NOT DELIVERED line"
                // only means something beside a climbing attempt count; on its
                // own it is equally consistent with the instrument never having
                // run, which is the exact ambiguity this bead exists to remove.
                char rc[160];
                // ck-475j: refusal counts ride the heartbeat too, so a caller
                // bug that a fix made QUIET still shows as a climbing number
                // rather than needing someone to grep for an exact string.
                char rf[256];
                char sh[128];   // ck-7rk6: skip-held-heir firings, zero stated
                char dp[160];   // ck-dup0: which path fed the game-thread drain
                // ck-zgh4: 192 held the four ck-2gtk counters and nothing more.
                // The latch + three-state + tid fields are ~640 chars of name
                // and value, and snprintf TRUNCATES SILENTLY — a short buffer
                // here would drop the new counters off the end of the line and
                // leave the heartbeat looking healthy. scope_slot_probe.cpp
                // asserts the formatted length against this size.
                char ss[768];   // ck-2gtk/ck-zgh4: scope-slot outcomes + latch
                char la[192];   // ck-bwz1: rotated-window archiving
                fprintf(bridge::g_logfile,
                    "[CK3-Bridge] IPC accept-thread heartbeat: "
                    "last_accept_ts=%lu last_swap_ts=%lu total_accepts=%lu poll_ret=%d %s %s %s %s %s %s\n",
                    static_cast<unsigned long>(g_accept_loop_last_accept_ts.load(std::memory_order_relaxed)),
                    static_cast<unsigned long>(g_accept_loop_last_swap_ts.load(std::memory_order_relaxed)),
                    static_cast<unsigned long>(g_accept_loop_total_accepts.load(std::memory_order_relaxed)),
                    ret,
                    bridge::format_reply_counters(rc, sizeof rc),
                    bridge::format_refusals(rf, sizeof rf),
                    bridge::format_skip_held_heir(sh, sizeof sh),
                    bridge::format_drain_provenance(dp, sizeof dp),
                    // ck-2gtk: "no arm displaced a live engine scope this run"
                    // is a NEGATIVE claim, so it must be answerable from an
                    // UNTRACED run. It rides the heartbeat, not the trace.
                    bridge::format_scope_slot_counters(ss, sizeof ss),
                    // ck-bwz1: "every rotated window was archived" is a
                    // NEGATIVE-shaped claim about loss, so it rides the
                    // heartbeat rather than the trace — an untraced run must
                    // still be able to say whether a window went missing.
                    bridge::format_archive_counters(la, sizeof la,
                                                    bridge::g_log_archive_q));
                fflush(bridge::g_logfile);
            }
        }

        // ck-bwz1: one spawn and one reap per wake, bounded work, off

        // the game thread. Compressing a 64MB window where the rotation

        // happens would stall the simulation the episode is measuring.

        bridge::archive_pump(bridge::g_log_archive_q,

                             bridge::instance_log_archive_dir());


        if (ret < 0) {
            if (errno != EINTR && bridge::g_logfile) {
                fprintf(bridge::g_logfile,
                    "[CK3-Bridge] IPC accept-thread poll() error: %s (errno=%d)\n",
                    strerror(errno), errno);
            }
            continue;
        }
        if (ret == 0) continue;

        struct sockaddr_un client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client = accept(g_server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client < 0) {
            if (bridge::g_logfile) fprintf(bridge::g_logfile,
                "[CK3-Bridge] IPC accept() failed: %s (errno=%d)\n",
                strerror(errno), errno);
            continue;
        }

        g_accept_loop_last_accept_ts.store(ipc_monotonic_secs(), std::memory_order_relaxed);
        g_accept_loop_total_accepts.fetch_add(1, std::memory_order_relaxed);

        if (bridge::g_logfile) fprintf(bridge::g_logfile, "[CK3-Bridge] Agent connected (fd=%d)\n", client);

        // ck-multiclient: adopt the new connection into a free (or reclaimable
        // stale) slot. Up to MAX_CLIENTS clients coexist (e.g. the MCP server +
        // a per-turn UserPromptSubmit hook). A LIVE existing client is never
        // evicted — we only reclaim a slot whose peer has gone away (MSG_PEEK
        // EOF / other errno, or peer_pid ESRCH). If every slot is held by a live
        // client, reject the newcomer with bridge_busy (slots_full).
        uint64_t lock_begin = ipc_monotonic_secs();
        int chosen_slot = -1;
        int live_count = 0;
        int first_holder_fd = -1, first_holder_pid = -1, first_holder_uid = -1;
        uint64_t first_holder_age = 0;
        {
            std::lock_guard<std::mutex> lock(g_client_mutex);
            uint64_t lock_wait = ipc_monotonic_secs() - lock_begin;
            if (lock_wait >= 2 && bridge::g_logfile) {
                fprintf(bridge::g_logfile,
                    "[CK3-Bridge] WARNING: IPC accept-thread waited %lus for g_client_mutex "
                    "(a holder is slow/stuck — investigate send_to_slot or reader_loop)\n",
                    static_cast<unsigned long>(lock_wait));
            }

            int free_slot = -1, stale_slot = -1;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (!bridge::slot_is_open(g_clients[i])) { if (free_slot < 0) free_slot = i; continue; }
                // Existing client — probe liveness (MSG_PEEK + peer pid), same
                // logic as the old single-slot path, now applied per slot.
                bool alive = false;
                char peek_buf;
                ssize_t pr = recv(g_clients[i].fd, &peek_buf, 1, MSG_PEEK | MSG_DONTWAIT);
                if (pr > 0 || (pr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) alive = true;
                if (alive && g_clients[i].peer_pid > 0 &&
                    !ipc_peer_pid_alive(g_clients[i].peer_pid)) {
                    alive = false;
                    if (bridge::g_logfile) fprintf(bridge::g_logfile,
                        "[CK3-Bridge] Slot=%d fd=%d looked alive via MSG_PEEK but peer_pid=%d "
                        "is gone (ESRCH) — reclaimable\n", i, g_clients[i].fd, g_clients[i].peer_pid);
                }
                if (!alive) { if (stale_slot < 0) stale_slot = i; continue; }
                live_count++;
                if (first_holder_fd < 0) {
                    first_holder_fd  = g_clients[i].fd;
                    first_holder_pid = g_clients[i].peer_pid;
                    first_holder_uid = g_clients[i].peer_uid;
                    first_holder_age = (g_clients[i].connect_ts != 0)
                        ? (ipc_monotonic_secs() - g_clients[i].connect_ts) : 0;
                }
            }

            chosen_slot = (free_slot >= 0) ? free_slot : stale_slot;
            if (chosen_slot >= 0) {
                if (bridge::slot_is_open(g_clients[chosen_slot]))
                    release_slot_locked(chosen_slot, "stale slot displaced by new accept");
                ClientSlot& s = g_clients[chosen_slot];
                // ck-xqol: in_use + fd + gen are written together under BOTH
                // g_client_mutex (held here) and send_mtx (taken inside); bumping
                // gen makes the reader reset framing and invalidates any stale
                // send snapshot.
                bridge::client_slot_open(s, client, ipc_monotonic_secs());
                // SO_PEERCRED returns the peer's credentials at connect() time.
                struct ucred uc;
                socklen_t uc_len = sizeof(uc);
                if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &uc, &uc_len) == 0) {
                    s.peer_pid = static_cast<int>(uc.pid);
                    s.peer_uid = static_cast<int>(uc.uid);
                    s.peer_gid = static_cast<int>(uc.gid);
                } else if (bridge::g_logfile) {
                    fprintf(bridge::g_logfile,
                        "[CK3-Bridge] SO_PEERCRED failed on fd=%d: %s (errno=%d)\n",
                        client, strerror(errno), errno);
                }
                if (bridge::g_logfile) fprintf(bridge::g_logfile,
                    "[CK3-Bridge] Agent connected (fd=%d) adopted into slot=%d "
                    "(live clients now=%d/%d)\n",
                    client, chosen_slot, live_count + 1, MAX_CLIENTS);
            }
        }

        if (chosen_slot < 0) {
            // Every slot held by a live client — reject with a structured
            // bridge_busy that quotes the first live holder + the client count.
            char busy_msg[416];
            int blen = snprintf(busy_msg, sizeof(busy_msg),
                "{\"error\":\"bridge_busy\","
                "\"hint\":\"All %d client slots in use; retry later.\","
                "\"who_holds_the_lock\":{"
                    "\"connected\":true,"
                    "\"client_count\":%d,"
                    "\"fd\":%d,"
                    "\"peer_pid\":%d,"
                    "\"peer_uid\":%d,"
                    "\"connection_age_seconds\":%lu"
                "}}\n",
                MAX_CLIENTS, live_count, first_holder_fd, first_holder_pid,
                first_holder_uid, static_cast<unsigned long>(first_holder_age));
            int flags = fcntl(client, F_GETFL, 0);
            if (flags >= 0) (void)fcntl(client, F_SETFL, flags | O_NONBLOCK);
            if (blen > 0) (void)send(client, busy_msg, static_cast<size_t>(blen), MSG_NOSIGNAL);
            close(client);
            continue;
        }

        g_accept_loop_last_swap_ts.store(ipc_monotonic_secs(), std::memory_order_relaxed);

        // ck-multiclient: do NOT clear g_msg_queue on connect (it would drop
        // another live client's in-flight actions), and do NOT reset
        // g_agent_mode (steering must persist across the per-turn hook's
        // connect/disconnect churn; control is managed via take_control /
        // release_control, not by connection lifecycle).
    }
}

bool start_ipc_server(const char* socket_path) {
    // ck-qn03: a null path means "this instance's socket". Resolving here (not
    // only at the call sites) keeps any future caller on the namespaced path.
    if (!socket_path || !*socket_path) socket_path = bridge::instance_socket_path();
    bridge::instance_make_dirs();   // the parent dir must exist before bind()
    strncpy(g_socket_path, socket_path, sizeof(g_socket_path) - 1);

    // Allow send timeout override via environment variable
    const char* timeout_env = getenv("CK3_BRIDGE_SEND_TIMEOUT_MS");
    if (timeout_env) {
        int val = atoi(timeout_env);
        if (val > 0) {
            g_send_timeout_ms = val;
            if (bridge::g_logfile) fprintf(bridge::g_logfile, "[CK3-Bridge] Send timeout set to %dms (from env)\n", g_send_timeout_ms);
        }
    }

    // Remove stale socket file
    unlink(socket_path);

    // Create socket
    g_server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_server_fd < 0) {
        if (bridge::g_logfile) fprintf(bridge::g_logfile, "[CK3-Bridge] Failed to create socket: %s\n", strerror(errno));
        return false;
    }

    // Bind
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(g_server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        if (bridge::g_logfile) fprintf(bridge::g_logfile, "[CK3-Bridge] Failed to bind socket: %s\n", strerror(errno));
        close(g_server_fd);
        g_server_fd = -1;
        return false;
    }

    // Allow cross-user connections (admin MCP connecting to gaming's socket).
    // Users 'gaming' and 'admin' are in different groups, so group-only (0770)
    // is insufficient. Use 0666 (rw for all) — minimum needed for Unix sockets.
    // Execute permission is not required for socket files.
    chmod(socket_path, 0666);

    // Listen. Only one agent is serviced at a time (single g_client_fd slot),
    // but the listen backlog is intentionally larger: if the accept-thread
    // stalls briefly (mutex wait, log pressure, etc.), a backlog of 1 makes
    // the kernel drop follow-up connects with EAGAIN, which is exactly the
    // ck-7tzu failure mode (Recv-Q=2 vs backlog=1 → every admin connect()
    // returns EAGAIN until someone drains the queue). Backlog=16 lets new
    // clients ride out short stalls without the socket looking "full".
    if (listen(g_server_fd, 16) < 0) {
        if (bridge::g_logfile) fprintf(bridge::g_logfile, "[CK3-Bridge] Failed to listen: %s\n", strerror(errno));
        close(g_server_fd);
        g_server_fd = -1;
        return false;
    }

    // Start accept thread and reader thread
    g_server_running = true;
    g_server_thread = std::thread(server_loop);
    g_reader_thread = std::thread(reader_loop);

    return true;
}

void stop_ipc_server() {
    g_server_running = false;

    if (g_server_thread.joinable()) {
        g_server_thread.join();
    }
    if (g_reader_thread.joinable()) {
        g_reader_thread.join();
    }

    {
        std::lock_guard<std::mutex> lock(g_client_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (bridge::slot_is_open(g_clients[i])) release_slot_locked(i, "stop_ipc_server");
    }
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }

    unlink(g_socket_path);
}

bool agent_connected() {
    std::lock_guard<std::mutex> lock(g_client_mutex);
    return connected_client_count_locked() > 0;
}

bool get_client_lock_info(ClientLockInfo* out) {
    if (!out) return false;
    std::lock_guard<std::mutex> lock(g_client_mutex);
    // Multi-client: report the FIRST live slot as the representative holder
    // (the ClientLockInfo shape is single-holder for back-compat). client_count
    // carries the true number of connected clients.
    int holder = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) { if (bridge::slot_is_open(g_clients[i])) { holder = i; break; } }
    out->client_count = connected_client_count_locked();
    out->connected = (holder >= 0);
    if (holder >= 0) {
        out->fd       = g_clients[holder].fd;
        out->peer_pid = g_clients[holder].peer_pid;
        out->peer_uid = g_clients[holder].peer_uid;
        out->peer_gid = g_clients[holder].peer_gid;
        out->connection_age_seconds = (g_clients[holder].connect_ts != 0)
            ? (ipc_monotonic_secs() - g_clients[holder].connect_ts) : 0;
    } else {
        out->fd = -1; out->peer_pid = -1; out->peer_uid = -1; out->peer_gid = -1;
        out->connection_age_seconds = 0;
    }
    // Probe under the mutex so the pid we report can't be reset by another
    // thread between the read and the kill().
    out->peer_alive = out->connected && ipc_peer_pid_alive(out->peer_pid);
    return true;
}

// ── ck-n7n0: SEND A DRAINED REPLY TO THE CLIENT THAT ASKED ─────────────────
//
// The game thread has no send target, so its replies BROADCAST to every live
// slot and clients are expected to filter by request_id. Measured: 26 of the
// bridge's 180 registered actions are queue-bound and therefore broadcast, and
// they are the WRITES — create_title, raise_armies, raid, set_character_faith,
// change_realm_law, the campaign's whole lever set. Measured also:
// ck3agent's bridge_client.recv() does NOT filter. It returns the first
// non-ping line, and only one call site (the declare path) filters. So with six
// agents on one socket, an agent's create_title can be answered, as far as it
// can tell, by another agent's raise_armies confirmation.
//
// That does not corrupt game state — the engine did what it did — but it
// corrupts the RECORD of which steering action succeeded, which is the arms'
// primary measurement.
//
// THE BRIDGE ALREADY KNOWS WHO ASKED. The reader tags every queued envelope
// with embed_session_id(), and the drain re-resolves that Session before
// dispatch (bridge_init.cpp). The identity was carried the whole way and simply
// never used for routing. So this is not new bookkeeping; it is using what is
// already there.
//
// MATCHED BY ID, NOT BY POINTER, per client_slot.h's own warning: a session can
// be ERASED out from under a live slot, so the by-value session_id copy is the
// only key that does not dereference a pointer that may already be freed
// (ck-6kvq.5, generalising ck-xqol).
//
// FALLS BACK TO BROADCAST, DELIBERATELY. An unsolicited engine event has no
// originating session, and a client that disconnected mid-flight has no slot.
// Both must still broadcast — today's behaviour — or this fix would SILENCE
// replies rather than misroute them, which is a worse trade. The narrowing is
// only for replies whose asker is identifiable AND still connected.
bool send_to_session_id(const char* session_id, const char* json_data, size_t len) {
    if (!session_id || !session_id[0]) return false;
    int idx = -1; uint32_t gen = 0;
    {
        std::lock_guard<std::mutex> lock(g_client_mutex);
        idx = bridge::find_slot_for_session(g_clients, MAX_CLIENTS, session_id);
        if (idx >= 0) gen = g_clients[idx].gen;
    }
    if (idx < 0) return false;             // caller falls back to broadcast
    return send_to_slot(idx, gen, json_data, len);
}

// ck-h4pn: ONE BODY, TWO ENTRY POINTS, and the route is a named decision rather
// than an implicit consequence of which thread is running. See
// observation_route.h for why the predicate exists and what it is the fix for.
static bool send_observation_impl(const char* json_data, size_t len,
                                  bool must_broadcast) {
    // Reader thread sets a thread-local target slot, so a reply routes back to
    // the requesting client. The game thread (events, on_tick drains) leaves it
    // unset → BROADCAST to all live clients, which then filter by request_id.
    //
    // must_broadcast OVERRIDES that: a record is not a reply, whoever emits it.
    if (bridge::observation_route(g_send_target_slot, must_broadcast)
            == bridge::ObsRoute::Unicast) {
        return send_to_slot(g_send_target_slot, g_send_target_gen, json_data, len,
                            g_last_send_outcome);
    }
    int idxs[MAX_CLIENTS]; uint32_t gens[MAX_CLIENTS]; int n = 0;
    {
        std::lock_guard<std::mutex> lock(g_client_mutex);
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (bridge::slot_is_open(g_clients[i])) { idxs[n] = i; gens[n] = g_clients[i].gen; n++; }
    }
    bool any = false;
    // ck-0zfc: the broadcast leg captured NO outcome, so a drop here was
    // invisible to the ck-jh76 record even though it went through the same five
    // exits. NOTE the braces: this loop body was a single brace-less statement,
    // and replacing it with a block without them put everything after the first
    // declaration OUTSIDE the loop. The compiler caught it; a block that still
    // compiled would not have been caught.
    //
    // The caller's outcome (if it opted in) keeps the FIRST failure rather than
    // the last — a later slot succeeding must not overwrite an earlier slot's
    // drop, or a broadcast that reached one of six clients would report clean.
    for (int i = 0; i < n; i++) {
        bridge::SendOutcome bo;
        if (send_to_slot(idxs[i], gens[i], json_data, len, &bo)) {
            any = true;
        } else {
            bridge::merge_broadcast_outcome(g_last_send_outcome, bo);
        }
    }
    return any;
}

bool send_observation(const char* json_data, size_t len) {
    return send_observation_impl(json_data, len, /*must_broadcast=*/false);
}

//: ck-h4pn: for frames whose audience is whoever is listening. Named so the intent
//: is visible where it is used, which is exactly where the unicast was invisible.
bool broadcast_observation(const char* json_data, size_t len) {
    return send_observation_impl(json_data, len, /*must_broadcast=*/true);
}

bool send_ipc_message(const std::string& json_msg) {
    return send_observation(json_msg.c_str(), json_msg.size());
}

// ck-6kvq.9: the pipeline conductor's DrainTick step pushes here. Same queue,
// same drain, same MAX_ACTIONS_PER_TICK bound as the reader thread's own
// fall-through — the flush loop does NOT open a second execution path into the
// game thread, it just decides WHEN the existing one is fed.
bool enqueue_action_envelope(const std::string& envelope) {
    if (envelope.empty()) return false;
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        g_msg_queue.push_back(envelope);
    }
    g_queue_cv.notify_one();
    return true;
}

// Dequeue a message from the background reader's queue.
// timeout_ms: 0 = non-blocking, >0 = wait up to timeout_ms milliseconds
// Returns allocated buffer (caller frees) or nullptr if queue is empty/timeout.
char* dequeue_action(int timeout_ms) {
    std::unique_lock<std::mutex> lock(g_queue_mutex);

    if (timeout_ms > 0 && g_msg_queue.empty()) {
        g_queue_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                            []{ return !g_msg_queue.empty(); });
    }

    if (g_msg_queue.empty()) return nullptr;

    std::string msg = std::move(g_msg_queue.front());
    g_msg_queue.pop_front();
    lock.unlock();

    // Return as malloc'd C string for API compatibility
    char* buf = static_cast<char*>(malloc(msg.size() + 1));
    if (!buf) return nullptr;
    memcpy(buf, msg.c_str(), msg.size() + 1);
    return buf;
}

} // namespace bridge
