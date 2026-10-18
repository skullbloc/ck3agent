// test_instance_paths.cpp — the bridge's instance naming, host-compiled.
//
// Bead ck-qn03 (R12). Two jobs:
//
//   1. SELF-CHECK (default invocation): assert the rules directly — default is
//      legacy byte for byte, a named instance moves every path under the
//      instance dir, an explicit override beats the id, a non-slug id is
//      REFUSED rather than sanitised.
//
//   2. CROSS-CHECK (`--dump`): print the four resolved paths as JSON so
//      `scripts/test_multi_instance.py` can compare the C++ answers against
//      the python authority and the shell helper, key for key. The resolver
//      caches on first use (std::call_once), so one process answers for one
//      environment — the dump mode exists precisely because a single process
//      cannot be re-asked.
//
// Run:  ./test_instance_paths            -> checks, exit 0 on pass
//       ./test_instance_paths --dump     -> {"instance_id":...} for the env
#include "instance_paths.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/wait.h>

static int g_checks = 0;
static int g_failures = 0;

static void check_eq(const char* what, const std::string& got, const std::string& want) {
    ++g_checks;
    if (got != want) {
        ++g_failures;
        std::printf("FAIL %s\n  got  %s\n  want %s\n", what, got.c_str(), want.c_str());
    }
}

static void check_true(const char* what, bool cond) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("FAIL %s\n", what); }
}

// Re-exec ourselves with a controlled environment and capture the --dump line.
// Forking is the only honest way to test a once-resolved global.
static std::string dump_with(const char* const* envp, const char* self) {
    int fds[2];
    if (pipe(fds) != 0) return "<pipe-failed>";
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]); close(fds[1]);
        const char* argv[] = {self, "--dump", nullptr};
        execve(self, const_cast<char* const*>(argv), const_cast<char* const*>(envp));
        _exit(127);
    }
    close(fds[1]);
    std::string out;
    char buf[512];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) out.append(buf, static_cast<size_t>(n));
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return out;
}

static std::string field(const std::string& json, const char* key) {
    std::string needle = std::string("\"") + key + "\":\"";
    size_t i = json.find(needle);
    if (i == std::string::npos) return "<missing:" + std::string(key) + ">";
    i += needle.size();
    size_t j = json.find('"', i);
    return json.substr(i, j - i);
}

int main(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "--dump") == 0) {
        std::printf("{\"instance_id\":\"%s\",\"socket\":\"%s\","
                    "\"telemetry_dir\":\"%s\",\"bridge_log\":\"%s\",\"refused\":%s}\n",
                    bridge::instance_id(), bridge::instance_socket_path(),
                    bridge::instance_telemetry_dir(), bridge::instance_bridge_log(),
                    bridge::instance_id_was_refused() ? "true" : "false");
        return 0;
    }

    const char* self = argv[0];

    // 1. No instance in the environment == the legacy host, byte for byte.
    {
        const char* env[] = {"PATH=/usr/bin:/bin", nullptr};
        std::string d = dump_with(env, self);
        check_eq("default id", field(d, "instance_id"), "default");
        check_eq("default socket", field(d, "socket"), "/tmp/ck3-agent.sock");
        check_eq("default telemetry", field(d, "telemetry_dir"), "/tmp/ck3-telemetry");
        check_eq("default log", field(d, "bridge_log"), "/tmp/ck3_bridge.log");
    }

    // 2. An explicit `default` is the same thing — not a directory called
    //    "default". This is the case an operator hits by exporting the id
    //    everywhere and expecting the campaign lane to be unchanged.
    {
        const char* env[] = {"PATH=/usr/bin:/bin", "CK3_INSTANCE_ID=default", nullptr};
        std::string d = dump_with(env, self);
        check_eq("explicit default socket", field(d, "socket"), "/tmp/ck3-agent.sock");
    }

    // 3. A named instance moves EVERY path under the instance dir.
    {
        const char* env[] = {"PATH=/usr/bin:/bin", "CK3_INSTANCE_ID=ep-a",
                             "CK3_INSTANCE_ROOT=/tmp/xx", nullptr};
        std::string d = dump_with(env, self);
        check_eq("named id", field(d, "instance_id"), "ep-a");
        check_eq("named socket", field(d, "socket"), "/tmp/xx/ep-a/agent.sock");
        check_eq("named telemetry", field(d, "telemetry_dir"), "/tmp/xx/ep-a/telemetry");
        check_eq("named log", field(d, "bridge_log"), "/tmp/xx/ep-a/bridge.log");
    }

    // 4. Two named instances never share anything. This is the whole bead in
    //    one assertion.
    {
        const char* ea[] = {"PATH=/usr/bin:/bin", "CK3_INSTANCE_ID=ep-a", nullptr};
        const char* eb[] = {"PATH=/usr/bin:/bin", "CK3_INSTANCE_ID=ep-b", nullptr};
        std::string a = dump_with(ea, self), b = dump_with(eb, self);
        check_true("sockets differ", field(a, "socket") != field(b, "socket"));
        check_true("telemetry differs", field(a, "telemetry_dir") != field(b, "telemetry_dir"));
        check_true("logs differ", field(a, "bridge_log") != field(b, "bridge_log"));
    }

    // 5. An explicit per-path override beats the instance id.
    {
        const char* env[] = {"PATH=/usr/bin:/bin", "CK3_INSTANCE_ID=ep-a",
                             "CK3_BRIDGE_SOCKET=/tmp/odd.sock", nullptr};
        std::string d = dump_with(env, self);
        check_eq("override wins", field(d, "socket"), "/tmp/odd.sock");
        check_eq("override is per-path", field(d, "telemetry_dir"),
                 "/tmp/ck3-inst/ep-a/telemetry");
    }

    // 6. A non-slug id is REFUSED, and says so. Sanitising `../../etc` into
    //    something plausible is how two "different" instances end up on one
    //    socket; falling back to the default is loud and recoverable.
    {
        const char* env[] = {"PATH=/usr/bin:/bin", "CK3_INSTANCE_ID=../evil", nullptr};
        std::string d = dump_with(env, self);
        check_eq("refused id falls back", field(d, "instance_id"), "default");
        check_true("refusal is flagged", d.find("\"refused\":true") != std::string::npos);
    }
    {
        const char* env[] = {"PATH=/usr/bin:/bin",
                             "CK3_INSTANCE_ID=way-too-long-way-too-long-way-too-long-"
                             "way-too-long-way-too-long", nullptr};
        std::string d = dump_with(env, self);
        check_eq("over-long id falls back", field(d, "instance_id"), "default");
    }

    std::printf("%s: %d checks, %d failures\n",
                g_failures ? "FAILED" : "ok", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
