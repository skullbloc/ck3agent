// personality_persist.cpp — the CROSS-PROCESS carrier for agent personality
// steering, and the identity header that keeps it inside one world (ck-9fc3).
//
// WHY THIS FILE EXISTS AT ALL. save_personality_locks_locked() and
// load_personality_locks() used to live in personality.cpp, which is deeply
// engine-coupled (character lookup, offset headers, /proc/self/mem writes) and
// cannot be compiled without a CK3 binary. They do not need any of that: they
// are stdio over two std::unordered_maps. Moving them here makes the whole
// ck-9fc3 mechanism — including the negative control, "a DIFFERENT save must
// not restore" — provable on a host with no game running. A guard whose failure
// case nobody has watched is a guard nobody has tested.
//
// WHAT CHANGED BESIDES THE MOVE. The file gains a "world" header:
//
//   {"world":{"episode_id":"ep-...","save_sha256":"0c94...","start_date_year":1066},
//    "locks":{"31808":{"axes":[...],"locked":[...]}}}
//
// written from the identity declared for the world the locks were set in. The
// reader takes the SAME judge_same_save() decision the in-process quarantine
// takes (see world_identity.cpp), and a file with no header — every file
// written before this change — names no save, so it can never match.
//
// This is what makes load_personality_locks WIRABLE. ck-9fc3 filed it as a
// hazard precisely because it was an identity-free payload with zero callers:
// wire it anywhere and the previous run's steering lands on whatever world is
// loaded next, permanently. It now has exactly one caller,
// world_identity_declare(), and it cannot cross a world.
//
// The "locks" object is byte-compatible with the old format on purpose: the old
// parser looked for the "locks" key and ignored everything before it, so a new
// file is readable by an old bridge (it would ignore the header — which is why
// the OLD bridge must never be the one doing a resume).

#include "personality_locks.h"
#include "init_order.h"   // ck-22n3: CK3_EARLY_INIT
#include "world_identity.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace bridge {

// Declared in bridge.h; redeclared so this TU stays engine-free.
extern FILE* g_logfile;
// Defined in instance_paths.cpp (see default_path()).
const char* instance_state_dir() __attribute__((weak));
FILE* instance_state_fopen(const char* name, const char* mode) __attribute__((weak));
}  // namespace bridge
#include "private_fopen.h"   // rc2 B1: explicit-path locks opened 0600, O_NOFOLLOW
namespace bridge {

#define PLOG(fmt, ...) do { \
    if (g_logfile) { fprintf(g_logfile, "[CK3-Bridge] " fmt, ##__VA_ARGS__); fflush(g_logfile); } \
} while(0)

namespace {

constexpr const char* kLegacyPath = "/tmp/ck3-personality-locks.json";
std::string g_path CK3_EARLY_INIT;   // empty: default_path()

// rc2 B1: the default lives in the instance state dir. Weak so host tests that
// do not link instance_paths.cpp keep the legacy literal.
const std::string& default_path() {
    static const std::string p = instance_state_dir
        ? std::string(instance_state_dir()) + "/ck3-personality-locks.json"
        : std::string(kLegacyPath);
    return p;
}
const char* cur_path() { return g_path.empty() ? default_path().c_str() : g_path.c_str(); }

// rc2 B1: the default file is opened through the verified state-dir fd
// (O_NOFOLLOW), never by path. An explicit path (tests) is opened as given.
FILE* open_locks(const char* mode) {
    if (g_path.empty() && instance_state_fopen)
        return instance_state_fopen("ck3-personality-locks.json", mode);
    return private_fopen(cur_path(), mode);   // rc2 B1: 0600, O_NOFOLLOW
}

// The identity the writer stamps. A LEAF lock: this mutex is taken while
// g_personality_lock_mutex is already held (the writer) and while the identity
// module's mutex is already held (the setter), so it must take nothing itself.
std::mutex    g_id_mu;
WorldIdentity g_id CK3_EARLY_INIT;

// Read a JSON string value for `key` out of `json`, bounded and allocation-shy.
// Deliberately a scanner and not a parser: the file is written by this same
// file, and a stricter reader here would only move the failure.
bool scan_string_field(const char* json, const char* key, std::string* out) {
    std::string pat = "\"";
    pat += key;
    pat += "\"";
    const char* k = strstr(json, pat.c_str());
    if (!k) return false;
    const char* c = strchr(k + pat.size(), ':');
    if (!c) return false;
    c++;
    while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') c++;
    if (*c != '"') return false;   // null, a number, anything else -> absent
    c++;
    out->clear();
    while (*c && *c != '"') {
        if (*c == '\\' && c[1]) { c++; }
        *out += *c++;
    }
    return *c == '"';
}

bool scan_uint_field(const char* json, const char* key, uint32_t* out) {
    std::string pat = "\"";
    pat += key;
    pat += "\"";
    const char* k = strstr(json, pat.c_str());
    if (!k) return false;
    const char* c = strchr(k + pat.size(), ':');
    if (!c) return false;
    c++;
    while (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r') c++;
    if (*c < '0' || *c > '9') return false;
    *out = static_cast<uint32_t>(strtoul(c, nullptr, 10));
    return true;
}

// Slurp the file. Returns empty on any failure (absent, too big, unreadable).
std::vector<char> read_file() {
    std::vector<char> empty;
    FILE* f = open_locks("r");
    if (!f) return empty;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 1024 * 1024) {
        PLOG("personality locks: bad file size %ld at %s\n", fsize, cur_path());
        fclose(f);
        return empty;
    }
    std::vector<char> buf(static_cast<size_t>(fsize) + 1);
    size_t got = fread(buf.data(), 1, static_cast<size_t>(fsize), f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

// The "world" header, if any. The header is written FIRST, so bounding the scan
// at the "locks" key keeps a character id from ever being mistaken for it.
bool parse_header(const char* json, WorldIdentity* out) {
    const char* w = strstr(json, "\"world\"");
    if (!w) return false;
    const char* locks = strstr(json, "\"locks\"");
    std::string head(w, locks && locks > w ? static_cast<size_t>(locks - w) : strlen(w));

    WorldIdentity id;
    scan_string_field(head.c_str(), "episode_id", &id.episode_id);
    if (!scan_string_field(head.c_str(), "save_sha256", &id.save_sha256)) return false;
    scan_uint_field(head.c_str(), "start_date_year", &id.start_date_year);
    if (id.save_sha256.empty()) return false;   // names no save == no identity
    *out = id;
    return true;
}

}  // namespace

void set_personality_locks_identity(const WorldIdentity& id) {
    std::lock_guard<std::mutex> lock(g_id_mu);
    g_id = id;
}

void set_personality_locks_path(const char* path) {
    g_path = (path && path[0]) ? path : "";
}

const char* personality_locks_path() { return cur_path(); }

// ---------------------------------------------------------------------------
// Write
// ---------------------------------------------------------------------------
// Call while holding g_personality_lock_mutex. The identity comes from this
// file's own leaf-locked copy, NOT from world_identity_declared(): the identity
// module takes its mutex and then g_personality_lock_mutex (to restore), so a
// writer reaching back the other way would close a lock cycle. See
// set_personality_locks_identity().
void save_personality_locks_locked() {
    WorldIdentity id;
    {
        std::lock_guard<std::mutex> lock(g_id_mu);
        id = g_id;
    }

    FILE* f = open_locks("w");
    if (!f) {
        PLOG("save_personality_locks: failed to open %s for writing\n", cur_path());
        return;
    }

    // The header goes first and is what makes the payload world-scoped. With
    // nothing declared it is written as null — honestly saying "this file names
    // no save", which is exactly what makes it unloadable later.
    fprintf(f, "{\"world\":");
    if (id.empty()) {
        fprintf(f, "null");
    } else {
        fprintf(f, "{\"episode_id\":\"%s\",\"save_sha256\":\"%s\",\"start_date_year\":%u}",
                id.episode_id.c_str(), id.save_sha256.c_str(), id.start_date_year);
    }
    fprintf(f, ",\"locks\":{");

    bool first_char = true;
    for (auto& [cid, pl] : g_personality_locks) {
        // Skip entries with nothing locked
        bool any_locked = false;
        for (int i = 0; i < 10; i++) { if (pl.locked[i]) { any_locked = true; break; } }
        if (!any_locked) continue;

        if (!first_char) fprintf(f, ",");
        first_char = false;
        fprintf(f, "\"%u\":{\"axes\":[%d,%d,%d,%d,%d,%d,%d,%d,%d,%d],\"locked\":[%s,%s,%s,%s,%s,%s,%s,%s,%s,%s]}",
                cid,
                (int)pl.axes[0], (int)pl.axes[1], (int)pl.axes[2], (int)pl.axes[3], (int)pl.axes[4],
                (int)pl.axes[5], (int)pl.axes[6], (int)pl.axes[7], (int)pl.axes[8], (int)pl.axes[9],
                pl.locked[0]?"true":"false", pl.locked[1]?"true":"false", pl.locked[2]?"true":"false",
                pl.locked[3]?"true":"false", pl.locked[4]?"true":"false", pl.locked[5]?"true":"false",
                pl.locked[6]?"true":"false", pl.locked[7]?"true":"false", pl.locked[8]?"true":"false",
                pl.locked[9]?"true":"false");
    }
    fprintf(f, "}}");
    fclose(f);
    PLOG("save_personality_locks: saved %zu entries to %s (world=%s)\n",
         g_personality_locks.size(), cur_path(),
         id.empty() ? "none declared" : id.save_sha256.c_str());
}

// ---------------------------------------------------------------------------
// Read
// ---------------------------------------------------------------------------
bool peek_personality_locks_identity(WorldIdentity* out) {
    if (!out) return false;
    std::vector<char> buf = read_file();
    if (buf.empty()) return false;
    if (!parse_header(buf.data(), out)) {
        PLOG("personality locks: %s carries no world header — it names no save and "
             "will never be restored (ck-9fc3)\n", cur_path());
        return false;
    }
    return true;
}

size_t load_personality_locks_from_disk() {
    std::vector<char> buf = read_file();
    if (buf.empty()) return 0;
    const char* json = buf.data();

    const char* locks_start = strstr(json, "\"locks\"");
    if (!locks_start) {
        PLOG("load_personality_locks: no \"locks\" key found in %s\n", cur_path());
        return 0;
    }
    const char* p = strchr(locks_start + 7, '{');
    if (!p) return 0;
    p++;  // skip {

    size_t loaded = 0;
    std::unique_lock<std::shared_mutex> lock(g_personality_lock_mutex);

    while (*p) {
        // Skip whitespace/commas
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') p++;
        if (*p == '}') break;

        // Expect "char_id"
        if (*p != '"') break;
        p++;
        uint32_t cid = 0;
        while (*p >= '0' && *p <= '9') {
            cid = cid * 10 + static_cast<uint32_t>(*p - '0');
            p++;
        }
        if (*p != '"') break;
        p++;

        // Skip to axes array
        const char* axes_start = strstr(p, "\"axes\":[");
        if (!axes_start) break;
        const char* a = axes_start + 8;

        PersonalityLock pl = {};
        for (int i = 0; i < 10; i++) {
            while (*a == ' ') a++;
            char* end = nullptr;
            long val = strtol(a, &end, 10);
            if (end == a) break;
            pl.axes[i] = static_cast<int8_t>(val);
            a = end;
            if (*a == ',') a++;
        }

        // Skip to locked array
        const char* locked_start = strstr(a, "\"locked\":[");
        if (!locked_start) break;
        const char* l = locked_start + 10;

        for (int i = 0; i < 10; i++) {
            while (*l == ' ' || *l == ',') l++;
            if (strncmp(l, "true", 4) == 0) {
                pl.locked[i] = true;
                l += 4;
            } else if (strncmp(l, "false", 5) == 0) {
                pl.locked[i] = false;
                l += 5;
            } else {
                break;
            }
        }

        g_personality_locks[cid] = pl;
        loaded++;

        // Advance past the closing }} of this entry
        p = strchr(l, '}');
        if (p) p++;
        if (p && *p == '}') p++;
    }

    PLOG("load_personality_locks: loaded %zu entries from %s\n", loaded, cur_path());
    return loaded;
}

}  // namespace bridge
