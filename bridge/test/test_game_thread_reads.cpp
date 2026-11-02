// test_game_thread_reads.cpp — L13 D1/D2: the REAL ipc_server.cpp serves a
// game-state read through the game-thread drain, in either clock state, with
// the reply bytes the inline path always built.
//
// Links the real reader (try_inline_safe_query, run_on_game_thread,
// run_game_thread_jobs) on a real socket. A stand-in "session thread" calls
// run_game_thread_jobs the way drain_game_thread_work does, once per pass,
// sleeping like the engine's idle pass (5 ms) or like a pass that executed a
// daily tick (50 ms here). Rows:
//
//   1  a read with the pause flag SET and with it CLEAR answers byte-identical
//      replies, and the reader never consults the flag (is_paused() calls: 0);
//   2  the handler ran on the drain thread, never on the reader;
//   3  the reply is exactly what the inline path composes: {request_id, the
//      envelope clock fragment, then the handler's body} (byte-identical
//      success path for a read that succeeded before L13);
//   4  with no drain running, the read answers a typed game_thread_timeout
//      after kGameThreadReadWaitMs, carrying its request_id, and the abandoned
//      job never runs once a drain starts;
//   5  DORMANT -> ACTIVE on the first drain with no tick (g_tick_id == 0), once;
//   and it prints the measured read latency (idle passes vs tick-length passes).
//
// Mutants that must red: route Tier 2 back to the IPC thread (row 2), drop the
// request_id/clock injection (row 3), make the wait unbounded (row 4 hangs to
// ctest's timeout), make activate_on_first_drain read g_tick_id (row 5).
//
// Exit 0 pass, 1 fail, 3 not evaluable.
#include "bridge.h"
#include "drain_activation.h"
#include "instance_paths.h"
#include "pipeline.h"
#include "session.h"
#include "snapshot.h"
#include "action_dispatch_internal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <poll.h>
#include <shared_mutex>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

// ── host stand-ins for the engine-bound symbols ipc_server.cpp reaches ───────
namespace bridge {
uintptr_t                g_base_addr = 0;
FILE*                    g_logfile = nullptr;
std::vector<MemRange>    g_valid_ranges;
std::shared_mutex        g_ranges_mutex;
std::mutex               g_controlled_mutex;
std::vector<uint32_t>    g_controlled_chars;
std::atomic<uint32_t>    g_controlled_primary{0};
std::atomic<uint32_t>    g_controlled_heir{0};
std::atomic<bool>        g_agent_mode{false};
std::atomic<uint64_t>    g_tick_id{0};
bool                     g_trace = false;
thread_local bool        g_in_protected_region = false;
thread_local sigjmp_buf  g_recovery_point;

// The handler: records the thread it ran on and how often each request ran.
std::atomic<std::thread::id> g_handler_thread{};
std::atomic<int>             g_handler_runs{0};
std::mutex                   g_rid_mutex;
std::vector<std::string>     g_rids_run;
static const char* kBody = "{\"success\":true,\"character_id\":7,\"name\":\"Probe\"}";

bool dispatch_action_with_response(const char*, const char* inner, std::string* out) {
    g_handler_thread.store(std::this_thread::get_id());
    g_handler_runs.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(g_rid_mutex);
        const char* r = inner ? std::strstr(inner, "\"request_id\":\"") : nullptr;
        g_rids_run.push_back(r ? std::string(r + 14, std::strcspn(r + 14, "\"")) : "");
    }
    if (out) *out = kBody;
    return true;
}
std::atomic<int> g_test_session_state{static_cast<int>(SessionState::ACTIVE)};
SessionState get_session_state() { return static_cast<SessionState>(g_test_session_state.load()); }
const char* session_state_string() { return "test"; }
bool is_bridge_internal_query(const char*) { return false; }
bool is_deferred_drain_action(const char*) { return false; }
bool is_game_state_query(const char* a) { return a && std::strcmp(a, "query_character") == 0; }
bool is_game_thread_action(const char*) { return false; }
// The pause flag the reader must NOT consult: toggled by the test, calls counted.
std::atomic<bool> g_test_paused{false};
std::atomic<int>  g_is_paused_calls{0};
bool is_paused() { g_is_paused_calls.fetch_add(1); return g_test_paused.load(); }
void refresh_memory_map() {}
bool is_registered_action(const char*) { return true; }
bool debug_verb_compiled_out(const char*) { return false; }
PipelineMode pipeline_mode() { return PipelineMode::Legacy; }
bool pipeline_handle_envelope(const std::string&, Session*) { return false; }
void pipeline_declare_episode_observer(const char*, const char*, Session::Mode) {}
void pipeline_forget_session(const char*) {}
void pipeline_note_episode_owner_gone(const char*, const char*) {}
void pipeline_note_session_activity(const Session*) {}
void pipeline_note_session_detached(const char*) {}
void pipeline_note_session_ended(const char*) {}
void pipeline_note_session_open(const Session*) {}
void pipeline_register_session(const char*) {}
void pipeline_unregister_session(const char*) {}
size_t pulse_sweep_ipc() { return 0; }
DeferredDrainStatus run_deferred_single_flight(
        const std::function<void(std::string*)>&, std::string*) {
    return DeferredDrainStatus::BusyDeferred;
}
}  // namespace bridge

namespace {

int g_failures = 0;
void row(const char* name, bool ok, const std::string& detail) {
    std::printf("%-56s %s  %s\n", name, ok ? "PASS" : "FAIL", detail.c_str());
    if (!ok) ++g_failures;
}

int dial(const std::string& path) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::snprintf(a.sun_path, sizeof a.sun_path, "%s", path.c_str());
    if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) { close(fd); return -1; }
    return fd;
}

bool send_all(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

// One newline-framed reply line, or "" on timeout.
std::string read_line(int fd, int ms) {
    std::string buf;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        const size_t nl = buf.find('\n');
        if (nl != std::string::npos) return buf.substr(0, nl);
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - std::chrono::steady_clock::now()).count();
        if (left <= 0) return "";
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, static_cast<int>(left)) <= 0) continue;
        char c;
        if (recv(fd, &c, 1, 0) <= 0) return "";
        buf.push_back(c);
    }
}

// The stand-in session thread: one drain pass, then the pass's sleep.
struct FakeSessionThread {
    std::atomic<bool> stop{false};
    std::atomic<int>  pass_ms{5};
    std::thread th;
    std::thread::id id;
    void start() {
        stop = false;
        th = std::thread([this] {
            for (;;) {
                if (stop.load()) return;
                bridge::run_game_thread_jobs(8, [] { return true; });
                std::this_thread::sleep_for(std::chrono::milliseconds(pass_ms.load()));
            }
        });
        id = th.get_id();
    }
    void halt() { stop = true; if (th.joinable()) th.join(); }
};

std::string read_request(const char* rid) {
    return std::string("{\"actions\":[{\"action\":\"query_character\",\"character_id\":7,"
                       "\"request_id\":\"") + rid + "\"}]}\n";
}

}  // namespace

int main() {
    const char* t = getenv("TMPDIR");
    const bool use_t = t && *t && strlen(t) < 60;
    std::string tmpl = std::string(use_t ? t : "/tmp") + "/ck3-gtr-XXXXXX";
    std::vector<char> b(tmpl.begin(), tmpl.end());
    b.push_back('\0');
    if (!mkdtemp(b.data())) { std::printf("NOT EVALUABLE: mkdtemp\n"); return 3; }
    const std::string dir(b.data());
    setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
    for (const char* k : {"CK3_INSTANCE_LAYOUT", "CK3_INSTANCE_ID", "CK3_INSTANCE_ROOT",
                          "CK3_BRIDGE_SOCKET", "CK3_TELE", "CK3_BRIDGE_LOG"})
        unsetenv(k);
    if (!bridge::start_ipc_server(nullptr)) {
        std::printf("NOT EVALUABLE: start_ipc_server refused: %s\n",
                    bridge::instance_private_dir_error());
        return 3;
    }
    const std::string sock = bridge::instance_socket_path();
    int fd = dial(sock);
    if (fd < 0) { std::printf("NOT EVALUABLE: dial\n"); return 3; }

    FakeSessionThread drain;
    drain.start();

    // Rows 1-3: the same read, pause flag set, then clear.
    bridge::g_is_paused_calls = 0;
    bridge::g_test_paused = true;
    send_all(fd, read_request("gtr1"));
    const std::string paused_reply = read_line(fd, 8000);
    const bool ran_on_drain_1 = bridge::g_handler_thread.load() == drain.id;
    bridge::g_test_paused = false;
    send_all(fd, read_request("gtr1"));
    const std::string running_reply = read_line(fd, 8000);
    const bool ran_on_drain_2 = bridge::g_handler_thread.load() == drain.id;
    row("same_bytes_with_the_pause_flag_set_and_clear",
        !paused_reply.empty() && paused_reply == running_reply &&
            bridge::g_is_paused_calls.load() == 0,
        "paused=" + paused_reply + " running=" + running_reply +
            " is_paused() calls=" + std::to_string(bridge::g_is_paused_calls.load()));
    row("the_read_ran_on_the_drain_thread_not_the_reader",
        ran_on_drain_1 && ran_on_drain_2, "both reads dispatched by the stand-in session thread");
    const std::string expected = std::string("{\"request_id\":\"gtr1\",") +
        bridge::build_envelope_clock_fragment() + "," + std::string(bridge::kBody + 1);
    row("the_reply_is_the_inline_paths_composition", paused_reply == expected,
        "expected " + expected);

    // Latency: 20 reads with idle passes (5 ms), 20 with tick-length passes (50 ms).
    auto measure = [&](int pass_ms, const char* tag) {
        drain.pass_ms = pass_ms;
        std::vector<double> ms;
        for (int i = 0; i < 20; ++i) {
            const std::string rid = std::string(tag) + std::to_string(i);
            const auto t0 = std::chrono::steady_clock::now();
            send_all(fd, read_request(rid.c_str()));
            const std::string r = read_line(fd, 8000);
            const auto t1 = std::chrono::steady_clock::now();
            if (r.find(rid) == std::string::npos) return std::string("no reply for ") + rid;
            ms.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        }
        std::sort(ms.begin(), ms.end());
        char buf[160];
        std::snprintf(buf, sizeof buf, "pass %d ms: median %.1f ms, max %.1f ms (n=20)",
                      pass_ms, ms[ms.size() / 2], ms.back());
        return std::string(buf);
    };
    const std::string idle = measure(5, "lat_idle_");
    const std::string busy = measure(50, "lat_tick_");
    row("latency_measured", idle.rfind("pass", 0) == 0 && busy.rfind("pass", 0) == 0,
        idle + "; " + busy);
    drain.pass_ms = 5;

    // Row 4: no drain -> typed timeout with its request_id; the job never runs.
    drain.halt();
    const auto t0 = std::chrono::steady_clock::now();
    send_all(fd, read_request("gtr_timeout"));
    const std::string timeout_reply = read_line(fd, bridge::kGameThreadReadWaitMs + 4000);
    const double waited = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    row("no_drain_answers_game_thread_timeout_with_its_request_id",
        timeout_reply.find("\"request_id\":\"gtr_timeout\"") != std::string::npos &&
            timeout_reply.find("game_thread_timeout") != std::string::npos &&
            waited >= bridge::kGameThreadReadWaitMs - 50,
        timeout_reply.substr(0, 160) + " after " + std::to_string(static_cast<int>(waited)) + " ms");
    drain.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool abandoned_ran = false;
    {
        std::lock_guard<std::mutex> lk(bridge::g_rid_mutex);
        abandoned_ran = std::find(bridge::g_rids_run.begin(), bridge::g_rids_run.end(),
                                  "gtr_timeout") != bridge::g_rids_run.end();
    }
    row("the_abandoned_read_never_runs", !abandoned_ran && bridge::game_thread_jobs_pending() == 0,
        "pending=" + std::to_string(bridge::game_thread_jobs_pending()));
    drain.halt();

    // Row 5: DORMANT -> ACTIVE on the first drain, with no tick.
    {
        bridge::g_tick_id = 0;
        std::atomic<bridge::SessionState> st{bridge::SessionState::DORMANT};
        const bool first = bridge::activate_on_first_drain(st);
        const bool second = bridge::activate_on_first_drain(st);
        row("dormant_to_active_on_the_first_drain_without_a_tick",
            first && !second && st.load() == bridge::SessionState::ACTIVE &&
                bridge::g_tick_id.load() == 0,
            "g_tick_id stayed 0; the second drain is not a transition");
    }

    close(fd);
    bridge::stop_ipc_server();
    rmdir((dir + "/ck3agent/telemetry").c_str());
    rmdir((dir + "/ck3agent").c_str());
    rmdir(dir.c_str());
    std::printf("\n%s (%d failing row%s)\n", g_failures ? "FAIL" : "PASS", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
