// launcher_bypass.cpp — interposed exec*() shims that rewrite the Paradox
// launcher binary path (`/launcher/dowser`) to the actual game binary
// (`/binaries/ck3`) at the moment Steam's pressure-vessel chain tries to
// exec it. This replaces the legacy `/tmp/ck3-launch-wrapper.sh` sed-swap
// (ck-dq7ym.2 / epic-F).
//
// How interposition works:
//   - This .so is LD_PRELOAD'd into every process Steam spawns (Steam
//     itself, the reaper, pressure-vessel-wrap, bwrap, ...). Each one
//     sees the symbols defined here ahead of glibc's.
//   - When any process in that chain ultimately exec's the launcher, our
//     shim catches it, rewrites the path suffix, re-injects LD_PRELOAD so
//     the bridge follows into the new image, and forwards to the real
//     libc entry point.
//   - For processes that aren't the eventual ck3 binary, this hook is a
//     no-op pass-through (path check fails fast).
//
// Path rewrite rule: if pathname ends with "/launcher/dowser", strip that
// suffix and replace with "/binaries/ck3". Anchored to the suffix so we
// don't accidentally rewrite directories that happen to contain "launcher"
// or unrelated tools that share the name.
//
// LD_PRELOAD re-injection: the wrapper script set LD_PRELOAD explicitly on
// every exec because pressure-vessel sometimes sanitizes envp between
// hops. We do the same — when we rewrite the path, we make sure the new
// envp carries LD_PRELOAD pointing at this .so so the child inherits the
// bridge.

// _GNU_SOURCE is already set globally by the bridge CMake target via
// target_compile_definitions; do not redefine.
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>

extern char** environ;

namespace bridge {
namespace launcher_bypass {

// Suffix anchoring: only paths whose tail is exactly "/launcher/dowser"
// get rewritten. Constants chosen to avoid surprising callers whose paths
// happen to contain "launcher" elsewhere.
static constexpr const char DOWSER_SUFFIX[] = "/launcher/dowser";
static constexpr size_t DOWSER_SUFFIX_LEN = sizeof(DOWSER_SUFFIX) - 1;
static constexpr const char CK3_SUFFIX[] = "/binaries/ck3";
static constexpr size_t CK3_SUFFIX_LEN = sizeof(CK3_SUFFIX) - 1;

// Cached absolute path to this .so, used to (re-)inject LD_PRELOAD into
// the spawned process. Resolved lazily via dladdr() on first hook fire.
static char g_self_so_path[PATH_MAX] = {0};
static std::atomic<bool> g_self_so_resolved{false};

static void resolve_self_so_path() {
    if (g_self_so_resolved.load(std::memory_order_acquire)) return;
    Dl_info info;
    if (dladdr(reinterpret_cast<void*>(&resolve_self_so_path), &info) &&
        info.dli_fname && info.dli_fname[0]) {
        if (info.dli_fname[0] == '/') {
            strncpy(g_self_so_path, info.dli_fname, sizeof(g_self_so_path) - 1);
        } else {
            char* real = realpath(info.dli_fname, nullptr);
            if (real) {
                strncpy(g_self_so_path, real, sizeof(g_self_so_path) - 1);
                free(real);
            } else {
                strncpy(g_self_so_path, info.dli_fname, sizeof(g_self_so_path) - 1);
            }
        }
        g_self_so_path[sizeof(g_self_so_path) - 1] = '\0';
    }
    g_self_so_resolved.store(true, std::memory_order_release);
}

// Append a line to /tmp/ck3_bridge_boot.log. Uses raw syscalls so it's
// safe from any context (signal handler, pre-main, in-exec). The file is
// opened with O_APPEND on every call so concurrent writers from sibling
// PIDs don't stomp on each other.
static void bootlog(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (static_cast<size_t>(n) >= sizeof(buf)) n = sizeof(buf) - 1;
    int fd = open("/tmp/ck3_bridge_boot.log",
                  O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    ssize_t w = write(fd, buf, static_cast<size_t>(n));
    (void)w;
    close(fd);
}

// Returns true and fills out_buf if pathname ends with DOWSER_SUFFIX.
// out_buf gets pathname's prefix + CK3_SUFFIX. False means "no rewrite".
static bool try_rewrite_path(const char* pathname,
                              char* out_buf, size_t out_buf_sz) {
    if (!pathname) return false;
    size_t plen = strlen(pathname);
    if (plen < DOWSER_SUFFIX_LEN) return false;
    const char* tail = pathname + plen - DOWSER_SUFFIX_LEN;
    if (memcmp(tail, DOWSER_SUFFIX, DOWSER_SUFFIX_LEN) != 0) return false;

    size_t prefix_len = static_cast<size_t>(tail - pathname);
    size_t new_len = prefix_len + CK3_SUFFIX_LEN;
    if (new_len + 1 > out_buf_sz) return false;
    memcpy(out_buf, pathname, prefix_len);
    memcpy(out_buf + prefix_len, CK3_SUFFIX, CK3_SUFFIX_LEN);
    out_buf[new_len] = '\0';
    return true;
}

// Build a new envp guaranteed to contain LD_PRELOAD=<self_so>. Returns a
// heap-allocated array on success (caller frees via free_injected_envp on
// execve failure; on success the array leaks with the dying process and
// that's fine). Returns nullptr if injection wasn't needed (i.e. our path
// is already present in LD_PRELOAD) or if self path is unknown.
static char** build_envp_with_ld_preload(char* const envp[]) {
    resolve_self_so_path();
    if (!g_self_so_path[0]) return nullptr;

    size_t count = 0;
    int existing_idx = -1;
    if (envp) {
        for (; envp[count]; count++) {
            if (existing_idx < 0 &&
                strncmp(envp[count], "LD_PRELOAD=", 11) == 0) {
                existing_idx = static_cast<int>(count);
            }
        }
    }

    // If LD_PRELOAD already includes us, leave envp alone.
    if (existing_idx >= 0 &&
        strstr(envp[existing_idx] + 11, g_self_so_path) != nullptr) {
        return nullptr;
    }

    // Compose new value: "LD_PRELOAD=<self>" or "LD_PRELOAD=<self>:<old>"
    const char* old_value = (existing_idx >= 0) ? envp[existing_idx] + 11 : "";
    size_t self_len = strlen(g_self_so_path);
    size_t old_len = strlen(old_value);
    size_t buf_len = 11 + self_len + (old_len > 0 ? 1 + old_len : 0) + 1;
    char* new_entry = static_cast<char*>(malloc(buf_len));
    if (!new_entry) return nullptr;
    memcpy(new_entry, "LD_PRELOAD=", 11);
    memcpy(new_entry + 11, g_self_so_path, self_len);
    size_t pos = 11 + self_len;
    if (old_len > 0) {
        new_entry[pos++] = ':';
        memcpy(new_entry + pos, old_value, old_len);
        pos += old_len;
    }
    new_entry[pos] = '\0';

    // Build the new envp array. If an existing LD_PRELOAD slot was there,
    // replace it; otherwise append.
    size_t new_count = (existing_idx >= 0) ? count : count + 1;
    char** new_envp = static_cast<char**>(
        malloc(sizeof(char*) * (new_count + 1)));
    if (!new_envp) {
        free(new_entry);
        return nullptr;
    }
    if (existing_idx >= 0) {
        for (size_t i = 0; i < count; i++) {
            new_envp[i] = (static_cast<int>(i) == existing_idx)
                            ? new_entry : envp[i];
        }
        new_envp[count] = nullptr;
    } else {
        for (size_t i = 0; i < count; i++) new_envp[i] = envp[i];
        new_envp[count] = new_entry;
        new_envp[count + 1] = nullptr;
    }
    return new_envp;
}

// Free the new envp array built by build_envp_with_ld_preload (only when
// execve returned with an error — on success the process image is gone).
static void free_injected_envp(char** new_envp) {
    if (!new_envp) return;
    for (size_t i = 0; new_envp[i]; i++) {
        if (strncmp(new_envp[i], "LD_PRELOAD=", 11) == 0 &&
            strstr(new_envp[i], g_self_so_path) != nullptr) {
            free(new_envp[i]);
            break;
        }
    }
    free(new_envp);
}

// Mirror the path rewrite into argv[0] when it also points at the
// launcher binary. Most callers pass argv[0] == pathname; if so, the
// child sees a consistent self-name. Returns a heap argv (caller frees on
// exec failure) or nullptr to indicate "use original argv".
static char** maybe_rewrite_argv0(char* const argv[], const char* new_path) {
    if (!argv || !argv[0]) return nullptr;
    char tmp[PATH_MAX];
    if (!try_rewrite_path(argv[0], tmp, sizeof(tmp))) return nullptr;
    if (strcmp(tmp, new_path) != 0) {
        // argv[0] suffix matches but resolves to a different ck3 path
        // (e.g. relative vs absolute). Use the rewritten argv[0] as-is.
    }

    size_t count = 0;
    for (; argv[count]; count++) {}
    char** new_argv = static_cast<char**>(
        malloc(sizeof(char*) * (count + 1)));
    if (!new_argv) return nullptr;
    char* new_arg0 = strdup(tmp);
    if (!new_arg0) {
        free(new_argv);
        return nullptr;
    }
    new_argv[0] = new_arg0;
    for (size_t i = 1; i < count; i++) new_argv[i] = argv[i];
    new_argv[count] = nullptr;
    return new_argv;
}

static void free_injected_argv(char** new_argv) {
    if (!new_argv) return;
    free(new_argv[0]);
    free(new_argv);
}

} // namespace launcher_bypass
} // namespace bridge

// ─────────────────────────────────────────────────────────────────────────
// Interposed glibc entry points. LD_PRELOAD ordering places these ahead
// of libc's, so any caller resolving `execve`/`execvp`/`posix_spawn` etc.
// through the PLT lands here first.
// ─────────────────────────────────────────────────────────────────────────

using bridge::launcher_bypass::bootlog;
using bridge::launcher_bypass::build_envp_with_ld_preload;
using bridge::launcher_bypass::free_injected_argv;
using bridge::launcher_bypass::free_injected_envp;
using bridge::launcher_bypass::maybe_rewrite_argv0;
using bridge::launcher_bypass::try_rewrite_path;

extern "C" int execve(const char* pathname,
                       char* const argv[], char* const envp[]) {
    using real_t = int (*)(const char*, char* const[], char* const[]);
    static std::atomic<real_t> real_execve_cached{nullptr};
    real_t real_execve = real_execve_cached.load(std::memory_order_acquire);
    if (!real_execve) {
        real_execve = reinterpret_cast<real_t>(dlsym(RTLD_NEXT, "execve"));
        real_execve_cached.store(real_execve, std::memory_order_release);
    }
    if (!real_execve) {
        errno = ENOSYS;
        return -1;
    }

    char rewritten[PATH_MAX];
    if (!try_rewrite_path(pathname, rewritten, sizeof(rewritten))) {
        return real_execve(pathname, argv, envp);
    }

    bootlog("[launcher-bypass] pid=%d execve '%s' -> '%s'\n",
            getpid(), pathname, rewritten);
    char** new_envp = build_envp_with_ld_preload(envp);
    char** new_argv = maybe_rewrite_argv0(argv, rewritten);
    char* const* argv_use = new_argv ? new_argv : argv;
    char* const* envp_use = new_envp ? new_envp : envp;
    int rc = real_execve(rewritten, argv_use, envp_use);
    // On success real_execve doesn't return; this path is the failure case.
    int saved = errno;
    free_injected_argv(new_argv);
    free_injected_envp(new_envp);
    bootlog("[launcher-bypass] pid=%d execve FAILED (errno=%d) for '%s'\n",
            getpid(), saved, rewritten);
    errno = saved;
    return rc;
}

extern "C" int execvp(const char* file, char* const argv[]) {
    using real_t = int (*)(const char*, char* const[]);
    static std::atomic<real_t> real_execvp_cached{nullptr};
    real_t real_execvp = real_execvp_cached.load(std::memory_order_acquire);
    if (!real_execvp) {
        real_execvp = reinterpret_cast<real_t>(dlsym(RTLD_NEXT, "execvp"));
        real_execvp_cached.store(real_execvp, std::memory_order_release);
    }
    if (!real_execvp) {
        errno = ENOSYS;
        return -1;
    }

    // Only rewrite if `file` is an absolute path. execvp's PATH-resolution
    // for relative names doesn't matter here — the launcher chain always
    // passes an absolute pathname to dowser. glibc declares `file` nonnull
    // so we don't re-check it (would trip -Wnonnull-compare).
    char rewritten[PATH_MAX];
    if (file[0] != '/' ||
        !try_rewrite_path(file, rewritten, sizeof(rewritten))) {
        return real_execvp(file, argv);
    }

    bootlog("[launcher-bypass] pid=%d execvp '%s' -> '%s'\n",
            getpid(), file, rewritten);
    // Fall through to our execve so LD_PRELOAD gets re-injected too.
    return execve(rewritten, argv, environ);
}

extern "C" int posix_spawn(pid_t* pid, const char* pathname,
                            const posix_spawn_file_actions_t* file_actions,
                            const posix_spawnattr_t* attrp,
                            char* const argv[], char* const envp[]) {
    using real_t = int (*)(pid_t*, const char*,
                            const posix_spawn_file_actions_t*,
                            const posix_spawnattr_t*,
                            char* const[], char* const[]);
    static std::atomic<real_t> real_spawn_cached{nullptr};
    real_t real_spawn = real_spawn_cached.load(std::memory_order_acquire);
    if (!real_spawn) {
        real_spawn = reinterpret_cast<real_t>(dlsym(RTLD_NEXT, "posix_spawn"));
        real_spawn_cached.store(real_spawn, std::memory_order_release);
    }
    if (!real_spawn) {
        return ENOSYS;
    }

    char rewritten[PATH_MAX];
    if (!try_rewrite_path(pathname, rewritten, sizeof(rewritten))) {
        return real_spawn(pid, pathname, file_actions, attrp, argv, envp);
    }

    bootlog("[launcher-bypass] pid=%d posix_spawn '%s' -> '%s'\n",
            getpid(), pathname, rewritten);
    char** new_envp = build_envp_with_ld_preload(envp);
    char** new_argv = maybe_rewrite_argv0(argv, rewritten);
    char* const* argv_use = new_argv ? new_argv : argv;
    char* const* envp_use = new_envp ? new_envp : envp;
    int rc = real_spawn(pid, rewritten, file_actions, attrp, argv_use, envp_use);
    free_injected_argv(new_argv);
    free_injected_envp(new_envp);
    if (rc != 0) {
        bootlog("[launcher-bypass] pid=%d posix_spawn FAILED (rc=%d) for '%s'\n",
                getpid(), rc, rewritten);
    }
    return rc;
}

extern "C" int posix_spawnp(pid_t* pid, const char* file,
                             const posix_spawn_file_actions_t* file_actions,
                             const posix_spawnattr_t* attrp,
                             char* const argv[], char* const envp[]) {
    using real_t = int (*)(pid_t*, const char*,
                            const posix_spawn_file_actions_t*,
                            const posix_spawnattr_t*,
                            char* const[], char* const[]);
    static std::atomic<real_t> real_spawnp_cached{nullptr};
    real_t real_spawnp = real_spawnp_cached.load(std::memory_order_acquire);
    if (!real_spawnp) {
        real_spawnp = reinterpret_cast<real_t>(dlsym(RTLD_NEXT, "posix_spawnp"));
        real_spawnp_cached.store(real_spawnp, std::memory_order_release);
    }
    if (!real_spawnp) {
        return ENOSYS;
    }

    // glibc declares `file` nonnull so we don't re-check it (would trip
    // -Wnonnull-compare under -Wall).
    char rewritten[PATH_MAX];
    if (file[0] != '/' ||
        !try_rewrite_path(file, rewritten, sizeof(rewritten))) {
        return real_spawnp(pid, file, file_actions, attrp, argv, envp);
    }

    bootlog("[launcher-bypass] pid=%d posix_spawnp '%s' -> '%s'\n",
            getpid(), file, rewritten);
    // Route through posix_spawn (our interposed one) so LD_PRELOAD gets
    // re-injected via the absolute-path branch.
    return posix_spawn(pid, rewritten, file_actions, attrp, argv, envp);
}
