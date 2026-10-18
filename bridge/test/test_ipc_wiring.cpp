// test_ipc_wiring.cpp — rc2 B1 + B9: the REAL ipc_server.cpp, on a real socket.
//
// test_instance_paths checks instance_peer_admitted() as a pure predicate, so
// deleting its call at accept() passed every test. This links ipc_server.cpp,
// starts the server in a private temp dir and drives it as a client:
//
//   B1  a peer whose uid is not the bridge's is refused at accept(). A second
//       uid needs root, so the test interposes geteuid(): once the server is
//       listening, the bridge believes it runs as uid+1 and our own connection
//       is the foreign peer. Before that, the same connection is admitted
//       (the control row), so the refusal is the call, not a broken socket.
//   B9  the reader drops a client whose line passes 1 MiB, refuses nesting
//       deeper than 128 before any parse, does not count brackets inside
//       strings, and stays up for the next client. The library's RapidJSON
//       default flags (inherited from ck3_bridge's compile definitions) are
//       iterative: a 50,000-deep document parses on a 256 KiB stack.
//
// Mutants that must red (see the commit): delete the instance_peer_admitted()
// call, the kMaxLineBytes check, the json_depth_exceeds() check, or the
// RAPIDJSON_PARSE_DEFAULT_FLAGS definition.
//
// Exit 0 pass, 1 fail, 3 not evaluable.
#include "bridge.h"
#include "instance_paths.h"
#include "pipeline.h"
#include "session.h"
#include "snapshot.h"
#include "action_dispatch_internal.h"
#include "envelope_shape.h"   // kMaxLineBytes

#include <rapidjson/reader.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <shared_mutex>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

// ── geteuid interposition: the executable's definition wins at link time ──────
static std::atomic<int> g_euid_delta{0};
extern "C" uid_t geteuid(void) noexcept {
    return static_cast<uid_t>(syscall(SYS_geteuid)) + static_cast<uid_t>(g_euid_delta.load());
}

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

bool dispatch_action_with_response(const char*, const char*, std::string* out) {
    if (out) *out = "{\"success\":true,\"stub\":true}";
    return true;
}
std::atomic<int> g_test_session_state{static_cast<int>(SessionState::DORMANT)};
SessionState get_session_state() { return static_cast<SessionState>(g_test_session_state.load()); }
const char* session_state_string() { return "test"; }
bool is_bridge_internal_query(const char*) { return false; }
bool is_deferred_drain_action(const char*) { return false; }
bool is_game_state_query(const char*) { return false; }
bool is_game_thread_action(const char*) { return false; }
bool is_paused() { return true; }
bool is_registered_action(const char*) { return false; }
// rc2 B2: one stand-in debug verb, so the reader's unavailable_in_release
// branch (ipc_server.cpp try_queue_fallthrough_rejection) is exercised. The
// real list is test_debug_verbs's business.
bool debug_verb_compiled_out(const char* v) { return v && std::strcmp(v, "memory_probe") == 0; }
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
DeferredDrainStatus run_deferred_pause_transparent(
        const std::function<void(std::string*)>&, std::string*) {
    return DeferredDrainStatus::DrainUnavailable;
}
}  // namespace bridge

// ── client helpers ───────────────────────────────────────────────────────────
namespace {

int g_failures = 0;
void row(const char* name, bool ok, const std::string& detail) {
    std::printf("%-44s %s  %s\n", name, ok ? "PASS" : "FAIL", detail.c_str());
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

// Read until `want` appears (returns "match"), EOF ("eof"), or timeout ("timeout").
std::string await(int fd, const char* want, int ms, std::string* seen = nullptr) {
    std::string buf;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        if (want && buf.find(want) != std::string::npos) { if (seen) *seen = buf; return "match"; }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            end - std::chrono::steady_clock::now()).count();
        if (left <= 0) { if (seen) *seen = buf; return "timeout"; }
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, static_cast<int>(left)) <= 0) continue;
        char tmp[4096];
        ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) { if (seen) *seen = buf; return "eof"; }
        buf.append(tmp, static_cast<size_t>(n));
    }
}

std::string nested(int depth) {
    return std::string(static_cast<size_t>(depth), '[') + std::string(static_cast<size_t>(depth), ']');
}

// ── the iterative-parse row runs in a child on a small stack ─────────────────
struct NullHandler : rapidjson::BaseReaderHandler<rapidjson::UTF8<>, NullHandler> {};
void* parse_deep(void* arg) {
    const std::string* doc = static_cast<const std::string*>(arg);
    rapidjson::Reader reader;
    rapidjson::StringStream ss(doc->c_str());
    NullHandler h;
    // DEFAULT flags: whatever RAPIDJSON_PARSE_DEFAULT_FLAGS the library uses.
    reader.Parse(ss, h);
    return reinterpret_cast<void*>(reader.HasParseError() ? 1 : 0);
}

int iterative_parse_child() {
    pid_t pid = fork();
    if (pid == 0) {
        static const std::string doc = nested(50000);
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 256 * 1024);
        pthread_t th;
        if (pthread_create(&th, &at, parse_deep, const_cast<std::string*>(&doc)) != 0) _exit(3);
        void* rv = nullptr;
        pthread_join(th, &rv);
        _exit(rv ? 2 : 0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 99;
}

}  // namespace

int main() {
    // A private temp dir as the user-layout base; nothing inherited. TMPDIR
    // only when short enough for sun_path (108 bytes) with /ck3agent/agent.sock.
    const char* t = getenv("TMPDIR");
    const bool use_t = t && *t && strlen(t) < 60;
    std::string tmpl = std::string(use_t ? t : "/tmp") + "/ck3-ipcw-XXXXXX";
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
    struct stat st;
    row("listens_in_private_dir_0600",
        lstat(sock.c_str(), &st) == 0 && S_ISSOCK(st.st_mode) && (st.st_mode & 0777) == 0600,
        sock);

    // Control: our own uid is admitted, and the reader answers.
    {
        int fd = dial(sock);
        std::string seen;
        const bool ok = fd >= 0 && send_all(fd, "{\"x\":1}\n") &&
                        await(fd, "malformed_envelope", 3000, &seen) == "match";
        row("same_uid_admitted_and_answered", ok, "control: the connection itself works");
        if (fd >= 0) close(fd);
    }

    // B1: the bridge now believes it is uid+1; our connection is a foreign peer.
    {
        g_euid_delta = 1;
        int fd = dial(sock);
        std::string seen;
        const std::string r1 = fd >= 0 ? await(fd, "peer_uid_refused", 3000, &seen) : "dial";
        const std::string r2 = r1 == "match" ? await(fd, nullptr, 3000) : r1;
        g_euid_delta = 0;
        row("foreign_uid_refused_at_accept", r1 == "match" && r2 == "eof",
            "ipc_server calls instance_peer_admitted (got " + r1 + "/" + r2 + ")");
        if (fd >= 0) close(fd);
    }

    // B9: nesting.
    {
        int fd = dial(sock);
        std::string seen;
        const bool deep = fd >= 0 && send_all(fd, nested(129) + "\n") &&
                          await(fd, "json_too_deep", 3000, &seen) == "match";
        row("depth_129_refused_before_parse", deep, "json_too_deep");
        const bool edge = fd >= 0 && send_all(fd, nested(128) + "\n") &&
                          await(fd, "malformed_envelope", 3000, &seen) == "match";
        row("depth_128_reaches_the_ladder", edge, "the cap is exact, not lower");
        const std::string in_str = "{\"x\":\"" + std::string(500, '[') + "\"}\n";
        const bool strs = fd >= 0 && send_all(fd, in_str) &&
                          await(fd, "malformed_envelope", 3000, &seen) == "match";
        row("brackets_inside_strings_not_counted", strs, "500 '[' in a string value");
        if (fd >= 0) close(fd);
    }

    // B9: an unterminated line past 1 MiB drops the client.
    {
        int fd = dial(sock);
        bool sent = fd >= 0;
        std::string chunk(64 * 1024, 'a');
        size_t total = 0;
        while (sent && total <= bridge::kMaxLineBytes + chunk.size()) {
            if (!send_all(fd, chunk)) break;   // EPIPE once dropped is fine
            total += chunk.size();
        }
        const std::string r = fd >= 0 ? await(fd, nullptr, 3000) : "dial";
        row("line_over_1MiB_drops_the_client", r == "eof",
            "got " + r + " after " + std::to_string(total) + " bytes");
        if (fd >= 0) close(fd);
    }
    {
        int fd = dial(sock);
        std::string seen;
        const bool ok = fd >= 0 && send_all(fd, "{\"x\":2}\n") &&
                        await(fd, "malformed_envelope", 3000, &seen) == "match";
        row("bridge_still_serves_after_a_drop", ok, "next client answered");
        if (fd >= 0) close(fd);
    }

    // B2: a compiled-out verb reaching the reader's fast-fail is answered
    // unavailable_in_release, not unknown_action (which it degrades to if the
    // branch is deleted). Control: a plain unknown verb is unknown_action.
    {
        bridge::g_test_session_state = static_cast<int>(bridge::SessionState::ACTIVE);
        int fd = dial(sock);
        std::string seen;
        const bool ok = fd >= 0 &&
            send_all(fd, "{\"actions\":[{\"action\":\"memory_probe\",\"request_id\":\"dv1\"}]}\n") &&
            await(fd, "\"request_id\":\"dv1\"", 3000, &seen) == "match";
        row("compiled_out_verb_is_unavailable_in_release",
            ok && seen.find("unavailable_in_release") != std::string::npos &&
                seen.find("unknown_action") == std::string::npos,
            seen.substr(0, 160));
        std::string seen2;
        const bool ok2 = fd >= 0 &&
            send_all(fd, "{\"actions\":[{\"action\":\"no_such_verb\",\"request_id\":\"dv2\"}]}\n") &&
            await(fd, "\"request_id\":\"dv2\"", 3000, &seen2) == "match";
        const size_t at = seen2.find("dv2");
        const std::string tail = at == std::string::npos ? seen2 : seen2.substr(at);
        row("control_unknown_verb_is_unknown_action",
            ok2 && tail.find("unknown_action") != std::string::npos &&
                tail.find("unavailable_in_release") == std::string::npos,
            tail.substr(0, 160));
        if (fd >= 0) close(fd);
        bridge::g_test_session_state = static_cast<int>(bridge::SessionState::DORMANT);
    }

    // B9: the library's default RapidJSON flags are iterative.
    {
        const int rc = iterative_parse_child();
        row("default_parse_is_iterative_on_small_stack", rc == 0,
            rc == 0 ? "50k-deep document on a 256 KiB stack"
                    : "child status " + std::to_string(rc) + " (recursive parse overflows)");
    }

    bridge::stop_ipc_server();
    rmdir((dir + "/ck3agent/telemetry").c_str());
    rmdir((dir + "/ck3agent").c_str());
    rmdir(dir.c_str());
    std::printf("\n%s (%d failing row%s)\n", g_failures ? "FAIL" : "PASS", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
