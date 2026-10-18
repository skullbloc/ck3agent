// instance_paths.cpp — see instance_paths.h. Transcription of
// `ck3agent/server/instance.py`; pinned by `scripts/test_multi_instance.py`.
#include "instance_paths.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

namespace bridge {
namespace {

constexpr const char* kDefaultId    = "default";
constexpr const char* kDefaultRoot  = "/tmp/ck3-inst";
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
            g_log_archive CK3_EARLY_INIT;
bool g_refused = false;

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

// mkdir -p, no dependencies. Best effort: a path we cannot create fails later
// at the bind/fopen with a real errno, which is a better message than ours.
void mkdir_p(const std::string& path) {
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
            ::mkdir(acc.c_str(), 0777);
        }
        i = j + 1;
    }
}

void init_once() {
    const char* raw = env_nonempty("CK3_INSTANCE_ID");
    if (raw && !valid_id(raw)) { g_refused = true; raw = nullptr; }
    g_id = (raw && strcmp(raw, kDefaultId) != 0) ? raw : kDefaultId;

    const char* root = env_nonempty("CK3_INSTANCE_ROOT");
    g_dir = std::string(root ? root : kDefaultRoot) + "/" + g_id;
    const bool is_default = (g_id == kDefaultId);

    // Precedence: explicit per-path override > instance id > legacy literal.
    auto pick = [&](const char* key, const char* legacy, const char* member) {
        const char* ov = env_nonempty(key);
        if (ov) return std::string(ov);
        return is_default ? std::string(legacy) : g_dir + "/" + member;
    };
    g_socket = pick("CK3_BRIDGE_SOCKET", kLegacySocket, "agent.sock");
    g_tele   = pick("CK3_TELE",          kLegacyTele,   "telemetry");
    g_log    = pick("CK3_BRIDGE_LOG",    kLegacyLog,    "bridge.log");

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

void instance_make_dirs() {
    ensure();
    mkdir_p(g_dir);
    mkdir_p(g_tele);
    // The socket and the log live in directories that must exist before bind()
    // and fopen(); on the default instance both are /tmp and this is a no-op.
    auto parent = [](const std::string& p) {
        size_t s = p.rfind('/');
        return s == std::string::npos ? std::string(".") : p.substr(0, s);
    };
    mkdir_p(parent(g_socket));
    mkdir_p(parent(g_log));
    if (!g_log_archive.empty()) mkdir_p(g_log_archive);
    // Only a NAMED instance gets its telemetry dir opened up. /tmp/ck3-telemetry
    // already exists on the campaign host with whatever mode the operator gave
    // it, and quietly widening that to 0777 is not this bead's business.
    if (g_id != kDefaultId) ::chmod(g_tele.c_str(), 0777);
}

const char* instance_id()            { ensure(); return g_id.c_str(); }
const char* instance_socket_path()   { ensure(); return g_socket.c_str(); }
const char* instance_telemetry_dir() { ensure(); return g_tele.c_str(); }
const char* instance_bridge_log()    { ensure(); return g_log.c_str(); }
//: Empty when archiving is not configured — an EMPTY STRING, not a path that
//: happens not to exist, so a caller must decide rather than fail at open().
const char* instance_log_archive_dir() { ensure(); return g_log_archive.c_str(); }
bool instance_id_was_refused()       { ensure(); return g_refused; }

}  // namespace bridge
