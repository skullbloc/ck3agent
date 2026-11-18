// test_reader_fairness.cpp — wave 13: the REAL reader thread does not
// hold one client's get_tick_info behind another client's burst of game-thread
// reads.
//
// THE DEFECT. The bridge has
// ONE reader thread for every client. It used to serve every complete line of a
// slot's read() chunk before it looked at any other slot, and a Tier-2 read
// blocks that thread until the game-thread drain has run it. A seated episode's
// observe ends with one query_realm per direct vassal, issued together (the
// pushed-warnings read, mcp_server._attach_pushed): 32 of them for the king of
// France. The meter's get_tick_info, a Tier-1 read that needs no game thread,
// waited behind the whole batch and timed out at the meter's 15 s on 47 of 51
// observes in pilot 2's one-seat K1 episode.
//
// THE RULE NOW: one line per slot per round, round robin; a slot's queued lines
// keep their order; a slot with queued lines is not read again until they are
// served (so the queue is bounded by one read() and the socket still pushes
// back); a slot that closes has its queued lines served before it is released,
// as before (every line the reader took off the socket runs).
//
// Rows:
//   1  the meter's get_tick_info, sent while another client's 20-read burst is
//      being served, answers within one read's service time (meter dialled
//      AFTER the seat: the higher slot), five polls in a row;
//   2  the same with the meter dialled FIRST (the lower slot);
//   3  the seat's 20 replies all arrive, in the order it sent them;
//   4  CONTROL (does not discriminate the fix: green on the old reader too): a
//      client that writes 5 reads and closes while they are being served
//      still has all 5 run (the new reader serves a closing slot's queue
//      before it releases the slot).
//
// Red on the pre-fix reader: rows 1 and 2 (the meter waits ~N x the read time).
// Exit 0 pass, 1 fail, 3 not evaluable.
#include "bridge.h"
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

//: One game-thread read's service time here, and the burst size. 20 x 200 ms is
//: the 4 s the old reader held a Tier-1 read for; the bound below is 5 reads.
static constexpr int kReadMs = 200;
static constexpr int kBurst = 20;
static constexpr int kMeterBoundMs = 5 * kReadMs;

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

std::mutex               g_rid_mutex;
std::vector<std::string> g_rids_run;

bool dispatch_action_with_response(const char* action, const char* inner, std::string* out) {
    const bool game_read = action && std::strcmp(action, "query_realm") == 0;
    if (game_read) std::this_thread::sleep_for(std::chrono::milliseconds(kReadMs));
    {
        std::lock_guard<std::mutex> lk(g_rid_mutex);
        const char* r = inner ? std::strstr(inner, "\"request_id\":\"") : nullptr;
        g_rids_run.push_back(r ? std::string(r + 14, std::strcspn(r + 14, "\"")) : "");
    }
    if (out) *out = game_read ? "{\"success\":true,\"realm_total_counties\":9}"
                              : "{\"success\":true,\"paused\":true}";
    return true;
}
SessionState get_session_state() { return SessionState::ACTIVE; }
const char* session_state_string() { return "test"; }
bool is_bridge_internal_query(const char* a) { return a && std::strcmp(a, "get_tick_info") == 0; }
bool is_deferred_drain_action(const char*) { return false; }
bool is_game_state_query(const char* a) { return a && std::strcmp(a, "query_realm") == 0; }
bool is_game_thread_action(const char*) { return false; }
bool is_paused() { return true; }
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
    std::printf("%-60s %s  %s\n", name, ok ? "PASS" : "FAIL", detail.c_str());
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

// One newline-framed line, or "" on timeout.
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

std::string line(const char* action, const std::string& rid) {
    return std::string("{\"actions\":[{\"action\":\"") + action +
           "\",\"params\":{},\"request_id\":\"" + rid + "\"}]}\n";
}

// The stand-in session thread: one drain pass, then an idle pass's 5 ms.
struct FakeSessionThread {
    std::atomic<bool> stop{false};
    std::thread th;
    void start() {
        th = std::thread([this] {
            while (!stop.load()) {
                bridge::run_game_thread_jobs(8, [] { return true; });
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    }
    void halt() { stop = true; if (th.joinable()) th.join(); }
};

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

// A burst of kBurst game-thread reads from `seat` (one write, as an asyncio
// gather of concurrent reads arrives), then five get_tick_info polls from
// `meter` while it is being served. Returns the meter's worst latency (ms) or
// -1 when a poll got no reply; `order_ok` reports the seat's replies.
double burst_and_poll(int seat, int meter, const std::string& tag, bool* order_ok, std::string* why) {
    std::string burst;
    for (int i = 0; i < kBurst; ++i) burst += line("query_realm", tag + "_r" + std::to_string(i));
    if (!send_all(seat, burst)) { *why = "seat send failed"; return -1; }
    std::this_thread::sleep_for(std::chrono::milliseconds(kReadMs / 2));
    double worst = 0;
    for (int k = 0; k < 5; ++k) {
        const std::string rid = tag + "_tick" + std::to_string(k);
        const auto t0 = std::chrono::steady_clock::now();
        send_all(meter, line("get_tick_info", rid));
        const std::string r = read_line(meter, 15000);
        const double ms = ms_since(t0);
        if (r.find(rid) == std::string::npos) { *why = "no reply for " + rid; return -1; }
        worst = std::max(worst, ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    bool ok = true;
    for (int i = 0; i < kBurst; ++i) {
        const std::string r = read_line(seat, 15000);
        if (r.find("\"request_id\":\"" + tag + "_r" + std::to_string(i) + "\"") == std::string::npos) {
            ok = false;
            *why = "seat reply " + std::to_string(i) + " was: " + r.substr(0, 80);
            break;
        }
    }
    *order_ok = ok;
    return worst;
}

}  // namespace

int main() {
    const char* t = getenv("TMPDIR");
    const bool use_t = t && *t && strlen(t) < 60;
    std::string tmpl = std::string(use_t ? t : "/tmp") + "/ck3-rf-XXXXXX";
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
    FakeSessionThread drain;
    drain.start();

    // Rows 1 and 3: the seat dials first, the meter second (the higher slot).
    int seat = dial(sock);
    int meter = dial(sock);
    if (seat < 0 || meter < 0) { std::printf("NOT EVALUABLE: dial\n"); return 3; }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));   // both slots adopted
    bool order_ok = false;
    std::string why;
    double worst = burst_and_poll(seat, meter, "a", &order_ok, &why);
    char d[200];
    std::snprintf(d, sizeof d, "worst of 5 polls %.0f ms during a %d x %d ms burst (bound %d ms) %s",
                  worst, kBurst, kReadMs, kMeterBoundMs, why.c_str());
    row("meter_tick_info_not_held_behind_a_burst_meter_dialled_second",
        worst >= 0 && worst <= kMeterBoundMs, d);
    row("the_seats_replies_all_arrive_in_send_order", order_ok, why.empty() ? "20 of 20" : why);
    close(seat);
    close(meter);

    // Row 2: the meter dials first (the lower slot).
    meter = dial(sock);
    seat = dial(sock);
    if (seat < 0 || meter < 0) { std::printf("NOT EVALUABLE: dial\n"); return 3; }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    why.clear();
    worst = burst_and_poll(seat, meter, "b", &order_ok, &why);
    std::snprintf(d, sizeof d, "worst of 5 polls %.0f ms (bound %d ms) %s",
                  worst, kMeterBoundMs, why.c_str());
    row("meter_tick_info_not_held_behind_a_burst_meter_dialled_first",
        worst >= 0 && worst <= kMeterBoundMs, d);
    close(seat);
    close(meter);

    // Row 4 (CONTROL, green on the old reader too): write 5 reads and close.
    int quitter = dial(sock);
    if (quitter < 0) { std::printf("NOT EVALUABLE: dial\n"); return 3; }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::string five;
    for (int i = 0; i < 5; ++i) five += line("query_realm", "q_r" + std::to_string(i));
    send_all(quitter, five);
    // Closed once the reader has taken the lines off the socket (it has: the
    // first read is being served) and before the old reader had run them all.
    // A peer that closes BEFORE the reader read its bytes loses them on both
    // readers (poll reports POLLHUP and the slot is released unread), so that
    // case is not a row: this one is the invariant, "a line taken off the socket runs".
    std::this_thread::sleep_for(std::chrono::milliseconds(kReadMs + kReadMs / 2));
    close(quitter);
    std::this_thread::sleep_for(std::chrono::milliseconds(5 * kReadMs + 1500));
    int ran = 0;
    {
        std::lock_guard<std::mutex> lk(bridge::g_rid_mutex);
        for (int i = 0; i < 5; ++i)
            ran += std::count(bridge::g_rids_run.begin(), bridge::g_rids_run.end(),
                              "q_r" + std::to_string(i)) == 1;
    }
    row("control_lines_written_before_a_close_all_run_once", ran == 5,
        std::to_string(ran) + " of 5 ran (non-discriminating control)");

    drain.halt();
    bridge::stop_ipc_server();
    rmdir((dir + "/ck3agent/telemetry").c_str());
    rmdir((dir + "/ck3agent").c_str());
    rmdir(dir.c_str());
    std::printf("\n%s (%d failing row%s)\n", g_failures ? "FAIL" : "PASS", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
