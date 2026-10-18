// test_instance_paths.cpp — the bridge's instance naming, host-compiled.
//
// Bead ck-qn03 (R12). Two jobs:
//
//   1. SELF-CHECK (default invocation): assert the rules directly — legacy
//      layout is the old host byte for byte, the default layout is the private
//      per-user dir, a named instance moves every path under the instance dir,
//      an explicit override beats the id, a non-slug id is REFUSED rather than
//      sanitised, peers and private dirs are checked (rc2 B1), a malformed
//      layout or id binds nothing, ancestors of the private dir are checked,
//      and state files go through the verified dir fd (section 10).
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
//       ./test_instance_paths --mkdirs   -> make dirs, print {"ok":..,"error":..}
//       ./test_instance_paths --statefile -> write/read via the state dir fd
#include "instance_paths.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
static std::string run_with(const char* mode, const char* const* envp, const char* self) {
    int fds[2];
    if (pipe(fds) != 0) return "<pipe-failed>";
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]); close(fds[1]);
        const char* argv[] = {self, mode, nullptr};
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

static std::string dump_with(const char* const* envp, const char* self) {
    return run_with("--dump", envp, self);
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
        char boot[512];
        if (!bridge::instance_boot_log_base(boot, sizeof boot)) boot[0] = '\0';
        std::printf("{\"instance_id\":\"%s\",\"socket\":\"%s\","
                    "\"telemetry_dir\":\"%s\",\"bridge_log\":\"%s\",\"refused\":%s,"
                    "\"layout\":\"%s\",\"layout_refused\":%s,\"state_dir\":\"%s\","
                    "\"config_refused\":%s,\"boot_log_base\":\"%s\"}\n",
                    bridge::instance_id(), bridge::instance_socket_path(),
                    bridge::instance_telemetry_dir(), bridge::instance_bridge_log(),
                    bridge::instance_id_was_refused() ? "true" : "false",
                    bridge::instance_layout(),
                    bridge::instance_layout_was_refused() ? "true" : "false",
                    bridge::instance_state_dir(),
                    bridge::instance_config_was_refused() ? "true" : "false", boot);
        return 0;
    }
    // rc2 B1: does the boot log open in this env? Writes one line if so.
    if (argc > 1 && std::strcmp(argv[1], "--bootlog") == 0) {
        const int fd = bridge::instance_open_boot_log();
        if (fd >= 0) { (void)!write(fd, "boot\n", 5); close(fd); }
        std::printf("{\"opened\":\"%s\"}\n", fd >= 0 ? "1" : "0");
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "--mkdirs") == 0) {
        bridge::instance_make_dirs();
        std::printf("{\"ok\":\"%s\",\"error\":\"%s\"}\n",
                    bridge::instance_private_dir_ok() ? "1" : "0",
                    bridge::instance_private_dir_error());
        return 0;
    }
    // rc2 B1: state files go through the verified dir fd. Writes "probe",
    // then tries to read "link" (the caller planted a symlink there).
    if (argc > 1 && std::strcmp(argv[1], "--statefile") == 0) {
        bridge::instance_make_dirs();
        FILE* w = bridge::instance_state_fopen("probe", "w");
        if (w) { std::fputs("x\n", w); std::fclose(w); }
        FILE* l = bridge::instance_state_fopen("link", "r");
        if (l) std::fclose(l);
        std::printf("{\"write\":\"%s\",\"symlink\":\"%s\"}\n", w ? "1" : "0", l ? "1" : "0");
        return 0;
    }

    const char* self = argv[0];

    // 1. Legacy layout, no instance == the historical host, byte for byte.
    {
        const char* env[] = {"PATH=/usr/bin:/bin", "CK3_INSTANCE_LAYOUT=legacy", nullptr};
        std::string d = dump_with(env, self);
        check_eq("legacy id", field(d, "instance_id"), "default");
        check_eq("legacy socket", field(d, "socket"), "/tmp/ck3-agent.sock");
        check_eq("legacy telemetry", field(d, "telemetry_dir"), "/tmp/ck3-telemetry");
        check_eq("legacy log", field(d, "bridge_log"), "/tmp/ck3_bridge.log");
        check_eq("legacy state dir", field(d, "state_dir"), "/tmp");
        check_eq("legacy layout", field(d, "layout"), "legacy");
    }

    // 1b. Default layout is the per-user dir under $XDG_RUNTIME_DIR.
    {
        const char* env[] = {"PATH=/usr/bin:/bin", "XDG_RUNTIME_DIR=/xr", nullptr};
        std::string d = dump_with(env, self);
        check_eq("user layout", field(d, "layout"), "user");
        check_eq("user socket", field(d, "socket"), "/xr/ck3agent/agent.sock");
        check_eq("user telemetry", field(d, "telemetry_dir"), "/xr/ck3agent/telemetry");
        check_eq("user log", field(d, "bridge_log"), "/xr/ck3agent/bridge.log");
        check_eq("user state dir", field(d, "state_dir"), "/xr/ck3agent");
    }
    {
        const char* env[] = {"PATH=/usr/bin:/bin", "XDG_RUNTIME_DIR=/xr",
                             "CK3_INSTANCE_ID=ep-a", nullptr};
        std::string d = dump_with(env, self);
        check_eq("user named socket", field(d, "socket"), "/xr/ck3agent/inst/ep-a/agent.sock");
        check_eq("user named state dir", field(d, "state_dir"), "/xr/ck3agent/inst/ep-a");
    }
    // 1c. A relative XDG_RUNTIME_DIR is ignored: /run/user/<euid>, else HOME.
    {
        const char* env[] = {"PATH=/usr/bin:/bin", "XDG_RUNTIME_DIR=rel", "HOME=/hh", nullptr};
        std::string d = dump_with(env, self);
        char run[64];
        std::snprintf(run, sizeof run, "/run/user/%u", (unsigned)geteuid());
        struct stat st;
        const bool have_run = ::stat(run, &st) == 0 && S_ISDIR(st.st_mode);
        check_eq("fallback socket", field(d, "socket"),
                 have_run ? std::string(run) + "/ck3agent/agent.sock"
                          : std::string("/hh/.local/state/ck3agent/agent.sock"));
    }
    // 1d. An unknown layout is refused and falls back to the private one.
    {
        const char* env[] = {"PATH=/usr/bin:/bin", "XDG_RUNTIME_DIR=/xr",
                             "CK3_INSTANCE_LAYOUT=tmp", nullptr};
        std::string d = dump_with(env, self);
        check_eq("unknown layout -> user", field(d, "layout"), "user");
        check_true("unknown layout flagged",
                   d.find("\"layout_refused\":true") != std::string::npos);
    }

    // 2. An explicit `default` is the same thing — not a directory called
    //    "default". This is the case an operator hits by exporting the id
    //    everywhere and expecting the campaign lane to be unchanged.
    {
        const char* env[] = {"PATH=/usr/bin:/bin", "CK3_INSTANCE_LAYOUT=legacy",
                             "CK3_INSTANCE_ID=default", nullptr};
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
        const char* env[] = {"PATH=/usr/bin:/bin", "CK3_INSTANCE_LAYOUT=legacy",
                             "CK3_INSTANCE_ID=ep-a",
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

    // 7. Peer admission (rc2 B1). A foreign uid needs root to produce, so the
    //    rule is checked directly and SO_PEERCRED is checked on a socketpair.
    {
        check_true("same uid admitted", bridge::instance_peer_admitted(true, 1000, 1000, false));
        check_true("other uid refused", !bridge::instance_peer_admitted(true, 1001, 1000, false));
        check_true("no cred refused", !bridge::instance_peer_admitted(false, 1000, 1000, false));
        check_true("legacy admits any", bridge::instance_peer_admitted(true, 1001, 1000, true));
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            struct ucred uc;
            socklen_t len = sizeof uc;
            const bool have = getsockopt(sv[0], SOL_SOCKET, SO_PEERCRED, &uc, &len) == 0;
            check_true("socketpair peer admitted",
                       bridge::instance_peer_admitted(have, have ? uc.uid : 0, geteuid(), false));
            close(sv[0]); close(sv[1]);
        } else {
            check_true("socketpair", false);
        }
    }

    // 8. The private dir: created 0700, refused when group/other-writable or
    //    a symlink. Run in children: the resolver caches per process.
    {
        char tmpl[] = "/tmp/ck3-ipt-XXXXXX";
        const char* t = mkdtemp(tmpl);
        check_true("mkdtemp", t != nullptr);
        if (t) {
            std::string xr = std::string("XDG_RUNTIME_DIR=") + t;
            const char* env[] = {"PATH=/usr/bin:/bin", xr.c_str(), nullptr};
            std::string base = std::string(t) + "/ck3agent";

            std::string d = run_with("--mkdirs", env, self);
            check_eq("fresh private dir ok", field(d, "ok"), "1");
            struct stat st;
            check_true("private dir is 0700",
                       ::lstat(base.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);

            ::chmod(base.c_str(), 0777);
            d = run_with("--mkdirs", env, self);
            check_eq("world-writable dir refused", field(d, "ok"), "0");

            ::rmdir((base + "/telemetry").c_str());
            ::rmdir(base.c_str());
            std::string real = std::string(t) + "/real";
            ::mkdir(real.c_str(), 0700);
            check_true("symlink made", ::symlink(real.c_str(), base.c_str()) == 0);
            d = run_with("--mkdirs", env, self);
            check_eq("symlinked dir refused", field(d, "ok"), "0");

            // 9. A private log is 0600 and never follows a symlink.
            std::string log = real + "/x.log";
            FILE* f = bridge::instance_open_private_log(log.c_str(), true);
            check_true("private log opened", f != nullptr);
            if (f) std::fclose(f);
            const mode_t want = bridge::instance_layout_is_legacy() ? 0644 : 0600;
            check_true("private log is 0600 (0644 legacy)",
                       ::lstat(log.c_str(), &st) == 0 && (st.st_mode & 0777) == want);
            std::string link = real + "/link.log";
            ::symlink(log.c_str(), link.c_str());
            FILE* g = bridge::instance_open_private_log(link.c_str(), false);
            check_true("append refuses a symlink", g == nullptr);
            if (g) std::fclose(g);
            ::chmod(log.c_str(), 0666);
            g = bridge::instance_open_private_log(log.c_str(), false);
            if (g) std::fclose(g);
            check_true("append tightens an existing loose log",
                       ::lstat(log.c_str(), &st) == 0 && (st.st_mode & 0777) == want);

            ::unlink(link.c_str()); ::unlink(log.c_str()); ::unlink(base.c_str());
            ::rmdir(real.c_str()); ::rmdir(t);
        }
    }

    // 10. rc2 B1: a malformed layout or id binds NOTHING (instance.py raises on
    //     both, so neither side settles on a path), and the chain above a
    //     named instance's root is checked, not just the leaf.
    {
        char tmpl[] = "/tmp/ck3-ipt-XXXXXX";
        const char* t = mkdtemp(tmpl);
        check_true("mkdtemp (10)", t != nullptr);
        if (t) {
            const std::string xr = std::string("XDG_RUNTIME_DIR=") + t;
            {
                const char* env[] = {"PATH=/usr/bin:/bin", xr.c_str(),
                                     "CK3_INSTANCE_LAYOUT=tmp", nullptr};
                std::string d = run_with("--mkdirs", env, self);
                check_eq("bad layout: nothing bound", field(d, "ok"), "0");
                check_true("bad layout: says why",
                           field(d, "error").find("CK3_INSTANCE_LAYOUT") != std::string::npos);
            }
            {
                const char* env[] = {"PATH=/usr/bin:/bin", xr.c_str(),
                                     "CK3_INSTANCE_ID=../evil", nullptr};
                std::string d = run_with("--mkdirs", env, self);
                check_eq("bad id: nothing bound", field(d, "ok"), "0");
            }
            {
                const char* env[] = {"PATH=/usr/bin:/bin", xr.c_str(),
                                     "CK3_INSTANCE_LAYOUT=legacy", "CK3_INSTANCE_ID=a/b", nullptr};
                std::string d = run_with("--mkdirs", env, self);
                check_eq("bad id, legacy layout: nothing bound", field(d, "ok"), "0");
            }
            // A root under a group/other-writable, NON-sticky ancestor: anyone
            // could rename the verified leaf away. Refused.
            const std::string ww = std::string(t) + "/ww";
            ::mkdir(ww.c_str(), 0700);
            ::chmod(ww.c_str(), 0777);
            const std::string root = std::string("CK3_INSTANCE_ROOT=") + ww + "/root";
            {
                const char* env[] = {"PATH=/usr/bin:/bin", xr.c_str(), root.c_str(),
                                     "CK3_INSTANCE_ID=ep-a", nullptr};
                std::string d = run_with("--mkdirs", env, self);
                check_eq("non-sticky writable ancestor refused", field(d, "ok"), "0");
                check_true("chain refusal names the ancestor",
                           field(d, "error").find("not sticky") != std::string::npos);
            }
            // The same with the sticky bit (how /tmp is): allowed.
            ::chmod(ww.c_str(), 01777);
            {
                const char* env[] = {"PATH=/usr/bin:/bin", xr.c_str(), root.c_str(),
                                     "CK3_INSTANCE_ID=ep-a", nullptr};
                std::string d = run_with("--mkdirs", env, self);
                check_eq("sticky writable ancestor allowed", field(d, "ok"), "1");
            }
            // State files: written through the dir fd, a planted symlink is
            // not followed.
            {
                const std::string sdir = ww + "/root/ep-a";
                const std::string target = std::string(t) + "/secret";
                if (FILE* s = std::fopen(target.c_str(), "w")) { std::fputs("s\n", s); std::fclose(s); }
                ::symlink(target.c_str(), (sdir + "/link").c_str());
                const char* env[] = {"PATH=/usr/bin:/bin", xr.c_str(), root.c_str(),
                                     "CK3_INSTANCE_ID=ep-a", nullptr};
                std::string d = run_with("--statefile", env, self);
                check_eq("state file written via the dir fd", field(d, "write"), "1");
                check_eq("state file symlink not followed", field(d, "symlink"), "0");
                struct stat st;
                check_true("state file is 0600",
                           ::lstat((sdir + "/probe").c_str(), &st) == 0 &&
                               (st.st_mode & 0777) == 0600);
                ::unlink((sdir + "/link").c_str());
                ::unlink((sdir + "/probe").c_str());
                ::unlink(target.c_str());
            }
            const std::string base = std::string(t) + "/ck3agent";
            for (const std::string& p : {ww + "/root/ep-a/telemetry", ww + "/root/ep-a/log-archive",
                                         ww + "/root/ep-a",
                                         ww + "/root", ww, base + "/telemetry", base})
                ::rmdir(p.c_str());
            ::rmdir(t);
        }
    }

    // 11. rc2 B1: leading/trailing ASCII whitespace in the layout and the id
    //     is stripped, exactly as instance.py (_WS) and ck3_instance.sh do.
    //     Before: "legacy " was refused here and accepted by the client, which
    //     then dialled /tmp/ck3-agent.sock with its owner/peer checks off.
    {
        const char* env[] = {"PATH=/usr/bin:/bin", "XDG_RUNTIME_DIR=/xr",
                             "CK3_INSTANCE_LAYOUT=legacy ", nullptr};
        std::string d = dump_with(env, self);
        check_eq("ws layout 'legacy ' -> legacy", field(d, "layout"), "legacy");
        check_true("ws layout 'legacy ' not refused",
                   d.find("\"layout_refused\":false") != std::string::npos);
        check_eq("ws layout socket", field(d, "socket"), "/tmp/ck3-agent.sock");
    }
    {
        const char* env[] = {"PATH=/usr/bin:/bin", "XDG_RUNTIME_DIR=/xr",
                             "CK3_INSTANCE_LAYOUT=\tuser\n", nullptr};
        std::string d = dump_with(env, self);
        check_eq("ws layout tab-user-newline -> user", field(d, "layout"), "user");
        check_true("ws layout user not refused",
                   d.find("\"config_refused\":false") != std::string::npos);
    }
    {
        const char* env[] = {"PATH=/usr/bin:/bin", "XDG_RUNTIME_DIR=/xr",
                             "CK3_INSTANCE_ID= ep-a\v", nullptr};
        std::string d = dump_with(env, self);
        check_eq("ws id ' ep-a' -> ep-a", field(d, "instance_id"), "ep-a");
        check_true("ws id not refused", d.find("\"refused\":false") != std::string::npos);
    }
    {
        const char* env[] = {"PATH=/usr/bin:/bin", "XDG_RUNTIME_DIR=/xr",
                             "CK3_INSTANCE_ID=ep a", nullptr};
        std::string d = dump_with(env, self);
        check_true("internal space in id refused",
                   d.find("\"refused\":true") != std::string::npos);
    }
    {
        const char* env[] = {"PATH=/usr/bin:/bin", "XDG_RUNTIME_DIR=/xr",
                             "CK3_INSTANCE_ID=   ", nullptr};
        std::string d = dump_with(env, self);
        check_eq("blank id -> default", field(d, "instance_id"), "default");
        check_true("blank id not refused", d.find("\"refused\":false") != std::string::npos);
    }

    // 12. rc2 B1: the boot log is written only with CK3_BRIDGE_BOOT_LOG=1 (any
    //     build, any layout), and its base is the resolver's base.
    {
        const char* env[] = {"PATH=/usr/bin:/bin", "CK3_INSTANCE_LAYOUT=legacy", nullptr};
        check_eq("legacy boot log off unless asked",
                 field(run_with("--bootlog", env, self), "opened"), "0");
        const char* ws[] = {"PATH=/usr/bin:/bin", "XDG_RUNTIME_DIR=rel", "HOME=/hh", nullptr};
        std::string d = dump_with(ws, self);
        check_eq("boot log base == resolver base (no XDG)", field(d, "boot_log_base"),
                 field(d, "state_dir"));
        const char* wx[] = {"PATH=/usr/bin:/bin", "XDG_RUNTIME_DIR=/xr", nullptr};
        d = dump_with(wx, self);
        check_eq("boot log base == resolver base (XDG)", field(d, "boot_log_base"),
                 field(d, "state_dir"));

        char tmpl[] = "/tmp/ck3-ipt-XXXXXX";
        const char* t = mkdtemp(tmpl);
        check_true("mkdtemp (12)", t != nullptr);
        if (t) {
            const std::string base = std::string(t) + "/ck3agent";
            ::mkdir(base.c_str(), 0700);
            const std::string xr = std::string("XDG_RUNTIME_DIR=") + t;
            const char* off[] = {"PATH=/usr/bin:/bin", xr.c_str(), nullptr};
            check_eq("user boot log off unless asked",
                     field(run_with("--bootlog", off, self), "opened"), "0");
            const char* on[] = {"PATH=/usr/bin:/bin", xr.c_str(), "CK3_BRIDGE_BOOT_LOG=1",
                                nullptr};
            check_eq("user boot log on when asked",
                     field(run_with("--bootlog", on, self), "opened"), "1");
            struct stat st;
            const std::string log = base + "/bridge_boot.log";
            check_true("boot log is 0600",
                       ::lstat(log.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
            ::unlink(log.c_str()); ::rmdir(base.c_str()); ::rmdir(t);
        }
    }

    // 13. rc2 B1: instance_fopen_private, the by-path open behind every other
    //     bridge-written file (gen_current.jsonl, board carrier, session stamp).
    {
        char tmpl[] = "/tmp/ck3-ipt-XXXXXX";
        const char* t = mkdtemp(tmpl);
        check_true("mkdtemp (13)", t != nullptr);
        if (t) {
            const mode_t want = bridge::instance_file_mode();
            const std::string f = std::string(t) + "/f.jsonl";
            FILE* w = bridge::instance_fopen_private(f.c_str(), "a");
            check_true("private file created", w != nullptr);
            if (w) std::fclose(w);
            struct stat st;
            check_true("private file created 0600 (0644 legacy)",
                       ::lstat(f.c_str(), &st) == 0 && (st.st_mode & 0777) == want);
            ::chmod(f.c_str(), 0666);
            w = bridge::instance_fopen_private(f.c_str(), "a");
            if (w) std::fclose(w);
            check_true("existing loose file tightened on write",
                       ::lstat(f.c_str(), &st) == 0 && (st.st_mode & 0777) == want);
            const std::string l = std::string(t) + "/l.jsonl";
            ::symlink(f.c_str(), l.c_str());
            w = bridge::instance_fopen_private(l.c_str(), "a");
            check_true("private file refuses a symlink", w == nullptr);
            if (w) std::fclose(w);
            const std::string q = std::string(t) + "/fifo";
            ::mkfifo(q.c_str(), 0600);
            w = bridge::instance_fopen_private(q.c_str(), "r");
            check_true("private file refuses a FIFO without blocking", w == nullptr);
            if (w) std::fclose(w);
            ::unlink(q.c_str()); ::unlink(l.c_str()); ::unlink(f.c_str()); ::rmdir(t);
        }
    }

    std::printf("%s: %d checks, %d failures\n",
                g_failures ? "FAILED" : "ok", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
