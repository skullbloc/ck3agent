// test_os_win_paths_pin.cpp — the Windows path rules (os/win_paths.h) and the
// Linux bridge's own instance_paths.cpp must agree on which CK3_INSTANCE_ID
// values are valid. Linux-only (it forks to set the environment before
// instance_paths.cpp's call_once fixes it, the way test_instance_paths does).
//
// For each id: instance_id_is_slug(id) must equal "instance_paths.cpp did NOT
// refuse it", checked in a child with CK3_INSTANCE_ID set. A disagreement is a
// transcription drift between the third and fourth copies of the slug rule.

#include "os/win_paths.h"
#include "instance_paths.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

namespace os = bridge::os;

static int g_failures = 0;
static int g_checks = 0;

static void check(bool cond, const char* what) {
    ++g_checks;
    if (!cond) { ++g_failures; std::printf("FAIL: %s\n", what); }
    else std::printf("ok:   %s\n", what);
}

// A child sets CK3_INSTANCE_ID and reports, via exit code, whether
// instance_paths.cpp refused it (1) or accepted it (0).
static bool paths_refused(const char* id) {
    pid_t pid = fork();
    if (pid == 0) {
        setenv("CK3_INSTANCE_LAYOUT", "user", 1);
        if (id) setenv("CK3_INSTANCE_ID", id, 1); else unsetenv("CK3_INSTANCE_ID");
        // Keep the child off the real socket dir; only the id verdict matters.
        setenv("XDG_RUNTIME_DIR", "/tmp", 1);
        _exit(bridge::instance_id_was_refused() ? 1 : 0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 1;
}

int main() {
    const char* ids[] = {"default", "arm3", "e2.arm-3", "A", "a_b.c-d",
                         "bad id", ".dot", "a/b", "-lead", ""};
    for (const char* id : ids) {
        const bool slug = os::instance_id_is_slug(id);
        const bool refused = paths_refused(id);
        // An empty / unset id is "default" on both sides: not a slug by the
        // rule, but not refused either (it is the default, not a bad id).
        const bool expect_refused = !slug && id[0] != '\0';
        char what[128];
        std::snprintf(what, sizeof(what), "id '%s': win rule (slug=%d) agrees with instance_paths (refused=%d)",
                      id, slug, refused);
        check(refused == expect_refused, what);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
