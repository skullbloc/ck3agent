// instance_paths.cpp — see instance_paths.h. Transcription of
// `ck3agent/server/instance.py`; pinned by `scripts/test_multi_instance.py`.
#include "instance_paths.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <mutex>
#include <pwd.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace bridge {
namespace {

constexpr const char* kDefaultId    = "default";
constexpr const char* kLegacyRoot   = "/tmp/ck3-inst";
constexpr const char* kLegacySocket = "/tmp/ck3-agent.sock";
constexpr const char* kLegacyTele   = "/tmp/ck3-telemetry";
constexpr const char* kLegacyLog    = "/tmp/ck3_bridge.log";

std::once_flag g_once;
std::string g_id CK3_EARLY_INIT, g_socket CK3_EARLY_INIT, g_tele CK3_EARLY_INIT,
            g_log CK3_EARLY_INIT, g_dir CK3_EARLY_INIT,
            // ck-bwz1: CK3_EARLY_INIT like its siblings, NOT by habit — these
            // are read from a constructor-ordering-sensitive path, and ck-475j
            // was an unprioritised _GLOBAL__sub_I zeroing state after dll_init
            // had already used it. A new member of this group inherits the
            // hazard whether or not anyone remembers why.
            g_log_archive CK3_EARLY_INIT,
            g_base CK3_EARLY_INIT, g_state_dir CK3_EARLY_INIT;
std::mutex g_err_mu;
std::string g_private_err CK3_EARLY_INIT;
// rc2 B1: a malformed CK3_INSTANCE_LAYOUT or CK3_INSTANCE_ID. instance.py
// raises on both, so a client resolves no path; the bridge binds none either.
// Paths still resolve (for the banner); nothing is bound or opened.
std::string g_config_err CK3_EARLY_INIT;
bool g_refused = false;
bool g_legacy = false;
bool g_layout_refused = false;
// The state dir, opened once after verification. Flag and lock files are
// opened relative to it (openat + O_NOFOLLOW), so a directory swapped in at
// its path after the check is never read or written.
int g_state_fd = -1;

// ^[A-Za-z0-9][A-Za-z0-9._-]{0,47}$ — the same slug rule as the python and the
// shell. An id that fails it is REFUSED (fall back to `default`), never
// sanitised: a silently-rewritten id is how two "different" instances end up
// sharing one socket.
bool valid_id(const char* s) {
    if (!s || !*s) return false;
    size_t n = strlen(s);
    if (n > 48) return false;
    auto alnum = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    if (!alnum(s[0])) return false;
    for (size_t i = 1; i < n; ++i) {
        if (!alnum(s[i]) && s[i] != '.' && s[i] != '_' && s[i] != '-') return false;
    }
    return true;
}

const char* env_nonempty(const char* key) {
    const char* v = getenv(key);
    return (v && *v) ? v : nullptr;
}

// rc2 B1: CK3_INSTANCE_LAYOUT and CK3_INSTANCE_ID are stripped of leading and
// trailing ASCII whitespace, exactly the set instance.py strips (`_WS`) and
// ck3_instance.sh strips. All three must agree: when the client accepted
// "legacy " and the bridge refused it, the client dialled the /tmp socket
// with its checks off while the bridge bound nothing there.
bool is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string env_stripped(const char* key) {
    const char* v = getenv(key);
    if (!v) return std::string();
    size_t b = 0, e = strlen(v);
    while (b < e && is_ws(v[b])) ++b;
    while (e > b && is_ws(v[e - 1])) --e;
    return std::string(v + b, e - b);
}

// mkdir -p, no dependencies. Best effort: a path we cannot create fails later
// at the bind/fopen with a real errno, which is a better message than ours.
void mkdir_p(const std::string& path, mode_t mode) {
    if (path.empty()) return;
    std::string acc;
    size_t i = 0;
    if (path[0] == '/') { acc = "/"; i = 1; }
    while (i <= path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        if (j > i) {
            if (acc.size() > 1 && acc.back() != '/') acc += '/';
            acc.append(path, i, j - i);
            ::mkdir(acc.c_str(), mode);
        }
        i = j + 1;
    }
}

// The per-user base dir. Mirrors instance.py:user_base().
std::string user_base() {
    const char* xdg = env_nonempty("XDG_RUNTIME_DIR");
    if (xdg && xdg[0] == '/') return std::string(xdg) + "/ck3agent";
    char run[64];
    snprintf(run, sizeof run, "/run/user/%u", static_cast<unsigned>(geteuid()));
    struct stat st;
    if (::stat(run, &st) == 0 && S_ISDIR(st.st_mode)) return std::string(run) + "/ck3agent";
    std::string h;
    if (const char* home = env_nonempty("HOME")) {
        h = home;
    } else {
        struct passwd pw, *res = nullptr;
        char buf[4096];
        if (getpwuid_r(geteuid(), &pw, buf, sizeof buf, &res) == 0 && res && res->pw_dir)
            h = res->pw_dir;
    }
    return h + "/.local/state/ck3agent";
}

// rc2 B1: every ancestor of `d` (symlinks resolved) is owned by root or euid,
// and one that is group/other-writable is sticky (/tmp). Otherwise whoever
// owns or can write an ancestor can rename the verified leaf away and put
// their own directory at its path.
bool verify_chain(const std::string& d, std::string* why) {
    char* rp = ::realpath(d.c_str(), nullptr);
    if (!rp) { *why = d + ": realpath: " + strerror(errno); return false; }
    const std::string real(rp);
    free(rp);
    size_t end = 0;
    for (;;) {
        const std::string pre = end == 0 ? std::string("/") : real.substr(0, end);
        struct stat st;
        if (::lstat(pre.c_str(), &st) != 0) { *why = pre + ": " + strerror(errno); return false; }
        if (!S_ISDIR(st.st_mode)) { *why = pre + ": not a directory"; return false; }
        if (st.st_uid != 0 && st.st_uid != geteuid()) {
            *why = pre + ": ancestor owned by uid " + std::to_string(st.st_uid);
            return false;
        }
        if ((st.st_mode & (S_IWGRP | S_IWOTH)) && !(st.st_mode & S_ISVTX)) {
            *why = pre + ": ancestor group/other writable and not sticky";
            return false;
        }
        if (end == real.size()) break;
        const size_t nx = real.find('/', end + 1);
        end = nx == std::string::npos ? real.size() : nx;
    }
    return true;
}

// lstat-verify a private dir: a directory, owned by euid, no group/other write.
bool verify_private(const std::string& d, std::string* why) {
    struct stat st;
    if (::lstat(d.c_str(), &st) != 0) {
        *why = d + ": " + strerror(errno);
        return false;
    }
    if (!S_ISDIR(st.st_mode)) { *why = d + ": not a directory"; return false; }
    if (st.st_uid != geteuid()) {
        *why = d + ": owned by uid " + std::to_string(st.st_uid) +
               ", not " + std::to_string(geteuid());
        return false;
    }
    if (st.st_mode & (S_IWGRP | S_IWOTH)) { *why = d + ": group/other writable"; return false; }
    // Ours but readable by others (a pre-existing dir made under a loose umask):
    // tighten to 0700 rather than refuse; a failed chmod is a refusal.
    if (st.st_mode & 077) {
        if (::chmod(d.c_str(), 0700) != 0) { *why = d + ": chmod 0700: " + strerror(errno); return false; }
    }
    return true;
}

void init_once() {
    const std::string lay = env_stripped("CK3_INSTANCE_LAYOUT");
    if (!lay.empty()) {
        g_legacy = lay == "legacy";
        g_layout_refused = !g_legacy && lay != "user";
        if (g_layout_refused)
            g_config_err = std::string("CK3_INSTANCE_LAYOUT=") + lay +
                           ": want unset, user or legacy";
    }
    if (!g_legacy) g_base = user_base();

    const std::string id = env_stripped("CK3_INSTANCE_ID");
    const char* raw = id.empty() ? nullptr : id.c_str();
    if (raw && !valid_id(raw)) {
        g_refused = true;
        if (g_config_err.empty())
            g_config_err = std::string("CK3_INSTANCE_ID=") + raw + ": not a valid slug";
        raw = nullptr;
    }
    g_id = (raw && strcmp(raw, kDefaultId) != 0) ? raw : kDefaultId;

    const char* root = env_nonempty("CK3_INSTANCE_ROOT");
    const std::string def_root = g_legacy ? std::string(kLegacyRoot) : g_base + "/inst";
    g_dir = (root ? std::string(root) : def_root) + "/" + g_id;
    const bool is_default = (g_id == kDefaultId);

    // Precedence: explicit per-path override > instance id > layout default.
    auto pick = [&](const char* key, const char* legacy, const char* member) {
        const char* ov = env_nonempty(key);
        if (ov) return std::string(ov);
        if (!is_default) return g_dir + "/" + member;
        return g_legacy ? std::string(legacy) : g_base + "/" + member;
    };
    g_socket = pick("CK3_BRIDGE_SOCKET", kLegacySocket, "agent.sock");
    g_tele   = pick("CK3_TELE",          kLegacyTele,   "telemetry");
    g_log    = pick("CK3_BRIDGE_LOG",    kLegacyLog,    "bridge.log");
    g_state_dir = g_legacy ? std::string("/tmp") : (is_default ? g_base : g_dir);

    // ck-bwz1: where retired log windows are archived, gzipped and unbounded.
    // DELIBERATELY NOT DEFAULTED TO /tmp. The whole reason the ring is bounded
    // at three is a /tmp quota hazard, and an unbounded archive there would
    // trade a known loss for an unknown one. An explicit override is honoured
    // for any instance; a NAMED instance otherwise gets a directory under its
    // own root, which the operator already chose. The default instance gets
    // NOTHING unless it asks — and bridge_init says so at startup when tracing
    // is armed, so a run that wanted archiving and did not get it is visible
    // rather than merely unarchived.
    {
        const char* ov = env_nonempty("CK3_BRIDGE_LOG_ARCHIVE_DIR");
        g_log_archive = ov ? std::string(ov)
                           : (is_default ? std::string() : g_dir + "/log-archive");
    }
}

void ensure() { std::call_once(g_once, init_once); }

}  // namespace

void instance_paths_init() { ensure(); instance_make_dirs(); }

namespace {
// Open the state dir once. User layout: the fd must be the inode that was
// verified (same dev/ino), owned by euid, no group/other write.
void open_state_fd_locked() {
    if (g_state_fd >= 0) return;
    int fd = ::open(g_state_dir.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return;
    if (!g_legacy) {
        struct stat a, b;
        if (::fstat(fd, &a) != 0 || ::lstat(g_state_dir.c_str(), &b) != 0 ||
            a.st_dev != b.st_dev || a.st_ino != b.st_ino || a.st_uid != geteuid() ||
            (a.st_mode & (S_IWGRP | S_IWOTH))) {
            ::close(fd);
            g_private_err = g_state_dir + ": changed after verification";
            return;
        }
    }
    g_state_fd = fd;
}

int state_fd() {
    std::lock_guard<std::mutex> lk(g_err_mu);
    return (g_private_err.empty() && g_config_err.empty()) ? g_state_fd : -1;
}
}  // namespace

void instance_make_dirs() {
    ensure();
    const mode_t m = instance_dir_mode();
    const bool is_default = (g_id == kDefaultId);
    if (!g_config_err.empty()) {
        std::lock_guard<std::mutex> lk(g_err_mu);
        g_private_err = g_config_err;
        return;
    }
    // User layout: the private dir first, verified (leaf and every ancestor)
    // before anything goes in it. Verified here, once; files in it are then
    // opened through g_state_fd, not by path.
    if (!g_legacy) {
        std::string why;
        mkdir_p(g_base, m);
        bool ok = verify_private(g_base, &why) && verify_chain(g_base, &why);
        if (ok && !is_default) {
            mkdir_p(g_dir, m);
            ok = verify_private(g_dir, &why) && verify_chain(g_dir, &why);
        }
        std::lock_guard<std::mutex> lk(g_err_mu);
        g_private_err = ok ? std::string() : why;
        if (!ok) return;
        open_state_fd_locked();
        if (!g_private_err.empty()) return;
    } else {
        mkdir_p(g_dir, m);
        std::lock_guard<std::mutex> lk(g_err_mu);
        open_state_fd_locked();
    }
    mkdir_p(g_tele, m);
    // The socket and the log live in directories that must exist before bind()
    // and fopen(); on the legacy default instance both are /tmp (a no-op).
    auto parent = [](const std::string& p) {
        size_t s = p.rfind('/');
        return s == std::string::npos ? std::string(".") : p.substr(0, s);
    };
    mkdir_p(parent(g_socket), m);
    mkdir_p(parent(g_log), m);
    if (!g_log_archive.empty()) mkdir_p(g_log_archive, m);
}

const char* instance_id()            { ensure(); return g_id.c_str(); }
const char* instance_socket_path()   { ensure(); return g_socket.c_str(); }
const char* instance_telemetry_dir() { ensure(); return g_tele.c_str(); }
const char* instance_bridge_log()    { ensure(); return g_log.c_str(); }
//: Empty when archiving is not configured — an EMPTY STRING, not a path that
//: happens not to exist, so a caller must decide rather than fail at open().
const char* instance_log_archive_dir() { ensure(); return g_log_archive.c_str(); }
bool instance_id_was_refused()       { ensure(); return g_refused; }

const char* instance_layout()        { ensure(); return g_legacy ? "legacy" : "user"; }
bool instance_layout_is_legacy()     { ensure(); return g_legacy; }
bool instance_layout_was_refused()   { ensure(); return g_layout_refused; }
const char* instance_state_dir()     { ensure(); return g_state_dir.c_str(); }
mode_t instance_dir_mode()           { ensure(); return g_legacy ? 0777 : 0700; }

bool instance_private_dir_ok() {
    ensure();
    std::lock_guard<std::mutex> lk(g_err_mu);
    return g_private_err.empty() && g_config_err.empty();
}

bool instance_config_was_refused() { ensure(); return !g_config_err.empty(); }

const char* instance_private_dir_error() {
    ensure();
    // Copied into a thread-local so the pointer survives a concurrent re-verify.
    thread_local std::string copy;
    std::lock_guard<std::mutex> lk(g_err_mu);
    copy = g_private_err.empty() ? g_config_err : g_private_err;
    return copy.c_str();
}

bool instance_peer_admitted(bool have_cred, uid_t peer_uid, uid_t self_uid,
                            bool legacy) {
    if (legacy) return true;
    return have_cred && peer_uid == self_uid;
}

namespace {
// `v` equals `want` once stripped of the same whitespace as env_stripped().
// No allocation: instance_open_boot_log runs pre-main and in the execve
// interposers.
bool stripped_equals(const char* v, const char* want) {
    if (!v) return false;
    while (is_ws(*v)) ++v;
    const size_t n = strlen(want);
    if (strncmp(v, want, n) != 0) return false;
    for (v += n; *v; ++v)
        if (!is_ws(*v)) return false;
    return true;
}
}  // namespace

bool instance_boot_log_base(char* out, size_t n) {
    // The same order as user_base(), minus the passwd lookup (not safe here).
    char run[64];
    snprintf(run, sizeof run, "/run/user/%u", static_cast<unsigned>(geteuid()));
    struct stat rst;
    const char* xdg = getenv("XDG_RUNTIME_DIR");
    const char* home = getenv("HOME");
    int w;
    if (xdg && xdg[0] == '/')
        w = snprintf(out, n, "%s/ck3agent", xdg);
    else if (::stat(run, &rst) == 0 && S_ISDIR(rst.st_mode))
        w = snprintf(out, n, "%s/ck3agent", run);
    else if (home && home[0])
        w = snprintf(out, n, "%s/.local/state/ck3agent", home);
    else
        return false;
    return w > 0 && static_cast<size_t>(w) < n;
}

int instance_open_boot_log() {
    // getenv and raw syscalls only: dll_init and the execve interposers call
    // this. Written ONLY when CK3_BRIDGE_BOOT_LOG=1, in every build and layout.
    // Legacy layout: /tmp/ck3_bridge_boot.log. Otherwise <private base>/
    // bridge_boot.log (instance_boot_log_base), when that dir already exists,
    // is ours and is 0700.
    const char* v = getenv("CK3_BRIDGE_BOOT_LOG");
    if (!(v && strcmp(v, "1") == 0)) return -1;
    if (stripped_equals(getenv("CK3_INSTANCE_LAYOUT"), "legacy")) {
        const int fd = ::open("/tmp/ck3_bridge_boot.log",
                              O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
        struct stat st;
        if (fd >= 0 && ::fstat(fd, &st) == 0 && (st.st_mode & 077)) ::fchmod(fd, 0600);
        return fd;
    }
    char dir[512];
    if (!instance_boot_log_base(dir, sizeof dir)) return -1;
    int dfd = ::open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dfd < 0) return -1;
    struct stat st;
    if (::fstat(dfd, &st) != 0 || st.st_uid != geteuid() || (st.st_mode & 077)) {
        ::close(dfd);
        return -1;
    }
    int fd = ::openat(dfd, "bridge_boot.log",
                      O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    ::close(dfd);
    if (fd >= 0 && ::fstat(fd, &st) == 0 && (st.st_mode & 077)) ::fchmod(fd, 0600);
    return fd;
}

mode_t instance_file_mode() { return instance_layout_is_legacy() ? 0644 : 0600; }

namespace {
// The one open for every file the bridge writes: O_NOFOLLOW|O_CLOEXEC (the
// final component is never a symlink), regular files only (O_NONBLOCK while
// opening, so a planted FIFO cannot stall us), created instance_file_mode().
// A write open of an existing file tightens it to that mode; in the user
// layout a file owned by another uid is refused.
FILE* open_regular(int dfd, const char* name, const char* mode) {
    int flags = O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
    const bool write = mode[0] == 'w' || mode[0] == 'a';
    if (mode[0] == 'r')      flags |= O_RDONLY;
    else if (mode[0] == 'w') flags |= O_WRONLY | O_CREAT | O_TRUNC;
    else if (mode[0] == 'a') flags |= O_WRONLY | O_CREAT | O_APPEND;
    else { errno = EINVAL; return nullptr; }
    const mode_t want = instance_file_mode();
    int fd = ::openat(dfd, name, flags, want);
    if (fd < 0) return nullptr;
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        errno = EINVAL;
        return nullptr;
    }
    if (write) {
        if (!instance_layout_is_legacy() && st.st_uid != geteuid()) {
            ::close(fd);
            errno = EACCES;
            return nullptr;
        }
        if ((st.st_mode & 0777) & ~want) ::fchmod(fd, want);
    }
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    FILE* f = ::fdopen(fd, mode);
    if (!f) { int e = errno; ::close(fd); errno = e; }
    return f;
}
}  // namespace

FILE* instance_state_fopen(const char* name, const char* mode) {
    ensure();
    if (!name || !*name || strchr(name, '/') || !mode) { errno = EINVAL; return nullptr; }
    const int dfd = state_fd();
    if (dfd < 0) { errno = EACCES; return nullptr; }
    return open_regular(dfd, name, mode);
}

FILE* instance_fopen_private(const char* path, const char* mode) {
    ensure();
    if (!path || !*path || !mode) { errno = EINVAL; return nullptr; }
    return open_regular(AT_FDCWD, path, mode);
}

int instance_state_open_fd(const char* name) {
    FILE* f = instance_state_fopen(name, "r");
    if (!f) return -1;
    int fd = ::dup(fileno(f));
    std::fclose(f);
    if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

int instance_state_unlink(const char* name) {
    ensure();
    if (!name || !*name || strchr(name, '/')) { errno = EINVAL; return -1; }
    const int dfd = state_fd();
    if (dfd < 0) { errno = EACCES; return -1; }
    return ::unlinkat(dfd, name, 0);
}

FILE* instance_open_private_log(const char* path, bool truncate) {
    if (!path || !*path) { errno = EINVAL; return nullptr; }
    const mode_t mode = instance_layout_is_legacy() ? 0644 : 0600;
    int flags = O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC;
    if (truncate) {
        if (::unlink(path) != 0 && errno != ENOENT) return nullptr;
        flags |= O_EXCL;
    } else {
        flags |= O_APPEND;
    }
    int fd = ::open(path, flags | O_NONBLOCK, mode);
    if (fd < 0) return nullptr;
    // rc2 B1: an append to an existing log: regular files only, and a looser
    // mode is tightened (SECURITY.md: created or tightened to 0600).
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        errno = EINVAL;
        return nullptr;
    }
    if ((st.st_mode & 0777) & ~mode) ::fchmod(fd, mode);
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    FILE* f = ::fdopen(fd, truncate ? "w" : "a");
    if (!f) { int e = errno; ::close(fd); errno = e; }
    return f;
}

}  // namespace bridge
