// map_graph.cpp — runtime loader + windowed live enrichment for the baked map graph.
//
// Pairs with the build-time map-processor `ck3mapgen` (bridge/tools/ck3mapgen), which
// emits ck3map.bin in the layout defined by bridge/tools/ck3mapgen/ck3map_format.h
// (namespace ck3map). This file does ZERO topology work — adjacency, distance, de-jure
// hierarchy and terrain are all baked. At query time it BFS-windows the baked CSR county
// graph from a scope-derived seed set and paints a LIVE overlay (holder / faith / culture
// / dev / control) onto only the windowed nodes.
//
// CONTRACT (authoritative = ck3map_format.h; reference loader/serializer = ck3mapgen.cpp
// MapView 1166-1205 + run_dump 1243-1311 — this file MIRRORS run_dump):
//   * CountyEdge.to_county_idx is a DENSE COUNTY INDEX (0..n_counties) directly into
//     g_map.county[] — BFS indexes directly, NO tid hashmap.
//   * CountyRecord.county_tid / duchy_tid / kingdom_tid / empire_tid and
//     TitleRecord.parent_tid are DENSE MAPGEN TITLE INDICES into g_map.title[], NOT
//     runtime ids. NO_IDX (0xFFFFFFFF) = none.
//   * De-jure keys are resolved STATICALLY: title_key(tid) = str(title[tid].key_off) for
//     tid < n_titles. ZERO memory reads for de-jure / topology / terrain.
//   * Terrain per county = barony[county.barony_start].terrain when barony_count>0,
//     mapped via terrain_key().
//   * To paint the LIVE overlay we map the baked county KEY string ("c_roma") to a
//     RUNTIME title via resolve_title_id_by_key() (the canonical key->runtime-id map),
//     then reuse lookup_title_obj / resolve_county_data exactly as do_query_county_details.
//
// Multi-client-safe: pure reads, no pause, no DB scan.

#include "bridge.h"
#include "ck3_offsets.h"
#include "action_dispatch_internal.h"  // ResponseBuilder, json_get_*, lookup_title_obj,
                                       // resolve_title_id_by_key, resolve_character,
                                       // ad_db_lookup, ad_read_std_string, resolve_title_realm

// The baked-artifact struct layout is the single source of truth. Include it directly;
// DO NOT redeclare the structs.
#include "../tools/ck3mapgen/ck3map_format.h"
#include "instance_paths.h"   // rc2 B1: state dir + verified fd

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <queue>
#include <unordered_map>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace bridge {

// resolve_county_data is static in query_handlers.cpp; reuse the same chain here.
// Declared file-local (matches the proven query_handlers.cpp:3763 implementation:
// county title -> first barony sub-title -> province PTR -> COUNTY_DATA_PTR).
static uintptr_t mg_resolve_county_data(uint32_t runtime_title_id) {
    uintptr_t db = safe_read_offset<uintptr_t>(ck3::globals::TITLE_DB);
    if (!db || !is_mapped(db)) return 0;
    uintptr_t table = safe_read<uintptr_t>(db + ck3::title_db::ARRAY_PTR);
    uint32_t capacity = safe_read<uint32_t>(db + ck3::title_db::ARRAY_COUNT);
    if (!table || !is_mapped(table) || capacity == 0) return 0;

    uint32_t masked = runtime_title_id & 0xFFFFFF;
    if (masked >= capacity) return 0;

    uintptr_t entry = safe_read<uintptr_t>(table + ck3::title_db::ELEMENT_PTR_OFFSET
                                       + static_cast<uintptr_t>(masked) * ck3::title_db::ELEMENT_STRIDE);
    if (!entry || !is_mapped(entry)) return 0;
    if ((safe_read<uint32_t>(entry + ck3::title_obj::TITLE_ID) & 0xFFFFFF) != masked) return 0;

    int32_t sub_count = safe_read<int32_t>(entry + ck3::title_obj::SUB_TITLE_COUNT);
    if (sub_count <= 0) return 0;
    uintptr_t sub_arr = safe_read<uintptr_t>(entry + ck3::title_obj::SUB_TITLE_ARRAY);
    if (!sub_arr || !is_mapped(sub_arr)) return 0;

    uint32_t barony_id = safe_read<uint32_t>(sub_arr) & 0xFFFFFF;
    if (barony_id >= capacity) return 0;
    uintptr_t barony = safe_read<uintptr_t>(table + ck3::title_db::ELEMENT_PTR_OFFSET
                                        + static_cast<uintptr_t>(barony_id) * ck3::title_db::ELEMENT_STRIDE);
    if (!barony || !is_mapped(barony)) return 0;

    uintptr_t province = safe_read<uintptr_t>(barony + ck3::title_obj::PROVINCE_PTR);
    if (!province || !is_mapped(province)) return 0;

    uintptr_t county_data = safe_read<uintptr_t>(province + ck3::province::COUNTY_DATA_PTR);
    if (county_data && !is_mapped(county_data)) return 0;
    return county_data;
}

namespace {

// ───────────────────────────── Loaded artifact state ─────────────────────────
struct MapArtifact {
    void*                       map_base = nullptr;   // mmap base (for munmap)
    size_t                      map_size = 0;
    const ck3map::Header*       hdr    = nullptr;
    const ck3map::ProvinceRecord* prov = nullptr;
    const ck3map::CountyRecord* county = nullptr;
    const ck3map::CountyEdge*   cedge  = nullptr;
    const ck3map::BaronyRecord* barony = nullptr;
    const ck3map::TitleRecord*  title  = nullptr;
    const char*                 pool   = nullptr;
    bool ok = false;
};

static MapArtifact g_map;

// String pool accessor — mirrors MapView::str (ck3mapgen.cpp:1177): guard off against
// strpool_size, never read past the pool.
//
// HARDENING (cycle-3 crash-fix): the returned const char* is consumed by strcmp
// (county_idx_by_key) and many `std::string +=` appends, each of which scans for a
// NUL. do_query_map_graph drains on the GAME THREAD (it is absent from the inline-
// query classifiers), where an over-read past the mmap'd artifact hard-faults the
// live game (process_vm_readv's EFAULT-safety does NOT cover a direct deref). So we
// require a NUL terminator to exist WITHIN the pool bounds [off, strpool_size); if a
// (corrupt/truncated) key_off lands on a non-terminated tail, return "" rather than
// hand back a string that strlen/strcmp would chase off the end of the mapping.
static const char* mg_str(uint32_t off) {
    if (!g_map.pool || !g_map.hdr || off >= g_map.hdr->strpool_size) return "";
    const char* s = g_map.pool + off;
    size_t maxlen = (size_t)(g_map.hdr->strpool_size - off);
    if (memchr(s, '\0', maxlen) == nullptr) return "";  // unterminated → unsafe to scan
    return s;
}

// De-jure title key from a dense mapgen title index — STATIC, zero memory reads
// (mirrors run_dump's title_key lambda, ck3mapgen.cpp:1272-1275).
static const char* mg_title_key(uint32_t tid) {
    if (tid == ck3map::NO_IDX || !g_map.hdr || tid >= g_map.hdr->n_titles) return "";
    return mg_str(g_map.title[tid].key_off);
}

// Representative terrain for a county = first barony's terrain (run_dump 1293-1296).
static const char* mg_county_terrain(const ck3map::CountyRecord& cr) {
    if (cr.barony_count == 0 || !g_map.hdr || cr.barony_start >= g_map.hdr->n_baronies)
        return nullptr;
    uint8_t t = g_map.barony[cr.barony_start].terrain;
    if (t == ck3map::TERRAIN_UNKNOWN) return nullptr;
    // mirror terrain_key (ck3mapgen.cpp:1207)
    switch (t) {
        case ck3map::TERRAIN_PLAINS: return "plains";
        case ck3map::TERRAIN_FARMLANDS: return "farmlands";
        case ck3map::TERRAIN_HILLS: return "hills";
        case ck3map::TERRAIN_MOUNTAINS: return "mountains";
        case ck3map::TERRAIN_DESERT: return "desert";
        case ck3map::TERRAIN_DESERT_MOUNTAINS: return "desert_mountains";
        case ck3map::TERRAIN_OASIS: return "oasis";
        case ck3map::TERRAIN_JUNGLE: return "jungle";
        case ck3map::TERRAIN_FOREST: return "forest";
        case ck3map::TERRAIN_TAIGA: return "taiga";
        case ck3map::TERRAIN_WETLANDS: return "wetlands";
        case ck3map::TERRAIN_STEPPE: return "steppe";
        case ck3map::TERRAIN_FLOODPLAINS: return "floodplains";
        case ck3map::TERRAIN_DRYLANDS: return "drylands";
        case ck3map::TERRAIN_TERRACED_HILLS: return "terraced_hills";
        case ck3map::TERRAIN_SEA: return "sea";
        case ck3map::TERRAIN_COASTAL_SEA: return "coastal_sea";
        case ck3map::TERRAIN_OCEAN: return "ocean";
        case ck3map::TERRAIN_LAKES: return "lakes";
        default: return nullptr;
    }
}

// crossing sigil — mirror cross_sigil (ck3mapgen.cpp:1232)
static const char* mg_cross_sigil(uint8_t c) {
    switch (c) {
        case ck3map::CROSS_STRAIT: return ":s";
        case ck3map::CROSS_RIVER: return ":r";
        case ck3map::CROSS_IMPASSABLE: return ":x";
        default: return "";
    }
}

// faith name off a CCountyData (county::FAITH_ID 0x208 -> FAITH_DB -> faith::FAITH_KEY 0x98)
static std::string county_faith_name(uintptr_t cdata) {
    uint32_t fid = safe_read<uint32_t>(cdata + ck3::county::FAITH_ID);
    if (fid == 0 || fid == 0xFFFFFFFF) return {};
    uintptr_t fo = ad_db_lookup(ck3::globals::FAITH_DB, fid);
    if (!fo) return {};
    return ad_read_std_string(fo + faith::FAITH_KEY);
}

// culture name off a CCountyData (county::CULTURE_ID 0x20C -> CULTURE_INDEXED_DB -> NAME_KEY 0x10)
static std::string county_culture_name(uintptr_t cdata) {
    uint32_t cid = safe_read<uint32_t>(cdata + ck3::county::CULTURE_ID);
    if (cid == 0 || cid == 0xFFFFFFFF) return {};
    uintptr_t co = ad_db_lookup(ck3::globals::CULTURE_INDEXED_DB, cid);
    if (!co) return {};
    return ad_read_std_string(co + ck3::db_object::NAME_KEY);
}

// ─── seed resolution per scope, returns DENSE COUNTY INDICES into g_map.county[] ──
// Static reverse lookup: county key string -> dense county index (mirrors
// MapView::county_by_key, ck3mapgen.cpp:1200). n_counties is a few thousand; a linear
// scan per seed is fine (scope=realm seeds at most a few hundred). We build a one-shot
// key->idx map for the realm path instead.
static uint32_t county_idx_by_key(const char* key) {
    if (!key || !*key || !g_map.hdr) return ck3map::NO_IDX;
    for (uint32_t i = 0; i < g_map.hdr->n_counties; i++) {
        const char* k = mg_str(g_map.county[i].key_off);
        if (k && strcmp(k, key) == 0) return i;
    }
    return ck3map::NO_IDX;
}

static std::vector<uint32_t> resolve_seeds(const char* params_json, std::string* note) {
    std::vector<uint32_t> seeds;
    const char* scope_c = json_get_string(params_json, "scope");
    std::string scope = scope_c ? scope_c : "";

    if (scope == "target") {
        // target_county may be a runtime title_id (numeric) or a county key. The baked
        // graph is keyed by KEY string (county_tid is a mapgen index, not a runtime id),
        // so resolve via the runtime title's key string.
        const char* tk = json_get_string(params_json, "target_county_key");
        if (tk && *tk) {
            uint32_t ci = county_idx_by_key(tk);
            if (ci != ck3map::NO_IDX) seeds.push_back(ci);
            else if (note) *note = "target_county_key not in baked county set";
            return seeds;
        }
        // numeric runtime title_id -> resolve its key via the title object
        uint32_t rt = (uint32_t)json_get_int(params_json, "target_county") & 0xFFFFFF;
        if (rt) {
            uintptr_t tobj = lookup_title_obj(rt);
            if (tobj) {
                uintptr_t type_ptr = safe_read<uintptr_t>(tobj + ck3::title_obj::TITLE_TYPE_PTR);
                std::string key = type_ptr && is_mapped(type_ptr)
                                ? ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY) : "";
                if (!key.empty()) {
                    uint32_t ci = county_idx_by_key(key.c_str());
                    if (ci != ck3map::NO_IDX) { seeds.push_back(ci); return seeds; }
                }
            }
        }
        if (note) *note = "target_county did not resolve to a baked county "
                          "(pass target_county_key with a county script key, e.g. c_roma)";
        return seeds;
    }

    if (scope == "region") {
        if (note) *note = "scope=region needs the baked region table (not in the P1 artifact)";
        return seeds;
    }

    if (scope == "explicit") {
        // comma list of county KEY strings in "counties"
        const char* raw_c = json_get_string(params_json, "counties");
        std::string raw = raw_c ? raw_c : "";
        size_t i = 0;
        while (i < raw.size()) {
            size_t j = raw.find(',', i);
            std::string tok = raw.substr(i, (j == std::string::npos ? raw.size() : j) - i);
            // trim surrounding spaces
            size_t a = tok.find_first_not_of(" \t");
            size_t b = tok.find_last_not_of(" \t");
            if (a != std::string::npos) {
                std::string key = tok.substr(a, b - a + 1);
                uint32_t ci = county_idx_by_key(key.c_str());
                if (ci != ck3map::NO_IDX) seeds.push_back(ci);
            }
            if (j == std::string::npos) break;
            i = j + 1;
        }
        if (seeds.empty() && note) *note = "no counties resolved from 'counties' key list";
        return seeds;
    }

    // default: scope=realm — seed = every county the character holds.
    void* ch = resolve_character(params_json);
    if (!ch || is_sentinel(ch)) { if (note) *note = "character not found for scope=realm"; return seeds; }
    uintptr_t c = reinterpret_cast<uintptr_t>(ch);
    uintptr_t tr = safe_read<uintptr_t>(c + ck3::character::TITLE_REALM_DATA);
    if (!tr || !is_mapped(tr)) { if (note) *note = "no realm"; return seeds; }
    uintptr_t arr = safe_read<uintptr_t>(tr + ck3::title_realm::TITLE_ARRAY_PTR);
    int32_t cnt = safe_read<int32_t>(tr + ck3::title_realm::TITLE_ARRAY_COUNT);
    if (!arr || !is_mapped(arr) || cnt < 0 || cnt > 10000) cnt = 0;
    for (int32_t i = 0; i < cnt; i++) {
        uint32_t tid = safe_read<uint32_t>(arr + i * 4) & 0xFFFFFF;
        if (!tid || tid == 0xFFFFFF) continue;
        uintptr_t tobj = lookup_title_obj(tid);
        if (!tobj) continue;
        uintptr_t type_ptr = safe_read<uintptr_t>(tobj + ck3::title_obj::TITLE_TYPE_PTR);
        if (!type_ptr || !is_mapped(type_ptr)) continue;
        std::string key = ad_read_std_string(type_ptr + ck3::db_object::NAME_KEY);
        if (key.empty() || key[0] != 'c') continue;   // only county-tier keys index the graph
        uint32_t ci = county_idx_by_key(key.c_str());
        if (ci != ck3map::NO_IDX) seeds.push_back(ci);
    }
    if (seeds.empty() && note) *note = "character holds no counties in baked set";
    return seeds;
}

} // anonymous namespace

// ───────────────────────────── Loader (called from bridge_init) ──────────────
// Artifact path resolution: $CK3MAP_BIN, else <state dir>/ck3map.bin. mmap read-only-private.
void map_graph_init() {
    if (g_map.map_base) { munmap(g_map.map_base, g_map.map_size); }
    g_map = MapArtifact{};

    // rc2 B1: the default is <state dir>/ck3map.bin (/tmp only in the legacy
    // layout), opened through the verified state-dir fd; an explicit
    // $CK3MAP_BIN is the operator's choice and is opened as given.
    const char* env_bin = getenv("CK3MAP_BIN");
    const bool explicit_bin = env_bin && *env_bin;
    std::string path = explicit_bin ? std::string(env_bin)
                                    : std::string(bridge::instance_state_dir()) + "/ck3map.bin";
    int fd = explicit_bin ? open(path.c_str(), O_RDONLY | O_CLOEXEC)
                          : bridge::instance_state_open_fd("ck3map.bin");
    if (fd < 0) {
        ADLOG("map_graph: artifact not found at %s — query_map_graph disabled\n", path.c_str());
        return;
    }
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(ck3map::Header)) { close(fd); return; }

    void* m = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) { ADLOG("map_graph: mmap failed for %s\n", path.c_str()); return; }

    const auto* hdr = reinterpret_cast<const ck3map::Header*>(m);
    if (memcmp(hdr->magic, ck3map::MAGIC, 8) != 0) {
        munmap(m, st.st_size);
        ADLOG("map_graph: bad magic in %s\n", path.c_str());
        return;
    }
    if (hdr->version != ck3map::VERSION) {
        munmap(m, st.st_size);
        ADLOG("map_graph: version mismatch (artifact=%u expected=%u) — disabled\n",
              hdr->version, ck3map::VERSION);
        return;
    }

    // Bounds-check every section offset against the mapped size before trusting it.
    const uint64_t fsz = (uint64_t)st.st_size;
    auto sec_ok = [&](uint64_t off, uint64_t count, uint64_t stride) -> bool {
        if (count == 0) return true;
        if (off > fsz) return false;
        return count <= (fsz - off) / stride;
    };
    if (!sec_ok(hdr->off_provinces,    hdr->n_prov,         sizeof(ck3map::ProvinceRecord)) ||
        !sec_ok(hdr->off_counties,     hdr->n_counties,     sizeof(ck3map::CountyRecord))   ||
        !sec_ok(hdr->off_county_edges, hdr->n_county_edges, sizeof(ck3map::CountyEdge))     ||
        !sec_ok(hdr->off_baronies,     hdr->n_baronies,     sizeof(ck3map::BaronyRecord))   ||
        !sec_ok(hdr->off_titles,       hdr->n_titles,       sizeof(ck3map::TitleRecord))    ||
        hdr->off_strpool > fsz || hdr->strpool_size > fsz - hdr->off_strpool) {
        munmap(m, st.st_size);
        ADLOG("map_graph: section bounds invalid — disabled\n");
        return;
    }

    const auto* b = reinterpret_cast<const uint8_t*>(m);
    g_map.map_base = m;
    g_map.map_size = st.st_size;
    g_map.hdr      = hdr;
    g_map.prov     = reinterpret_cast<const ck3map::ProvinceRecord*>(b + hdr->off_provinces);
    g_map.county   = reinterpret_cast<const ck3map::CountyRecord*>(b + hdr->off_counties);
    g_map.cedge    = reinterpret_cast<const ck3map::CountyEdge*>(b + hdr->off_county_edges);
    g_map.barony   = reinterpret_cast<const ck3map::BaronyRecord*>(b + hdr->off_baronies);
    g_map.title    = reinterpret_cast<const ck3map::TitleRecord*>(b + hdr->off_titles);
    g_map.pool     = reinterpret_cast<const char*>(b + hdr->off_strpool);
    g_map.ok       = true;

    ADLOG("map_graph: loaded %u counties, %u county-edges, %u titles (v%u, hash=%llx)\n",
          hdr->n_counties, hdr->n_county_edges, hdr->n_titles, hdr->version,
          (unsigned long long)hdr->map_hash);
}

// ───────────────────────────── The handler ───────────────────────────────────
bool do_query_map_graph(const char* params_json, std::string* out) {
    // cycle-3 crash-fix (was: attempt-25 game-thread fault, crash ck3_20260630_115545):
    // do_query_map_graph drains on the GAME THREAD (query_map_graph is in is_game_state_query
    // → served inline when PAUSED on the IPC reader thread; falls through to the on_tick
    // g_msg_queue drain when UNPAUSED, same proven path as query_dejure_progress). On the game
    // thread a direct deref of bad memory hard-faults the live game, so EVERY read here is now
    // bounded: game-object reads go through safe_read/ad_* (EFAULT-safe via process_vm_readv),
    // mmap'd artifact array reads are clamped to the header counts, and mg_str() guarantees a
    // pool string is NUL-terminated within strpool_size before any strlen/strcmp/append.
    if (!g_map.ok) {
        if (out) *out = R"({"error":"map_artifact_missing","hint":"build ck3map.bin via ck3mapgen and place it in the bridge state dir (/tmp in the legacy layout) or set $CK3MAP_BIN; bridge restart required"})";
        return false;
    }

    int  hops      = json_get_int(params_json, "hops");      if (hops <= 0) hops = 2;
    if (hops > 8) hops = 8;
    int  max_nodes = json_get_int(params_json, "max_nodes"); if (max_nodes <= 0) max_nodes = 60;
    if (max_nodes > 1000) max_nodes = 1000;
    const char* detail_c = json_get_string(params_json, "detail");
    std::string detail = detail_c ? detail_c : "";
    const char* scope_c = json_get_string(params_json, "scope");
    std::string scope = scope_c ? scope_c : "realm";
    bool brief = (detail == "brief");

    std::string note;
    std::vector<uint32_t> seeds = resolve_seeds(params_json, &note);
    if (seeds.empty()) {
        if (out) {
            ResponseBuilder r;
            r.begin_object();
            r.key("error");  r.value_string("empty_scope");
            r.key("scope");  r.value_string(scope.c_str(), scope.size());
            r.key("hint");   r.value_string(note.empty() ? "no counties resolved for scope" : note.c_str());
            r.end_object();
            *out = r.take();
        }
        return false;
    }

    // ── BFS over baked CSR county edges to `hops`, capped at max_nodes. to_county_idx
    //    is a DENSE COUNTY INDEX — index g_map.county[] directly, no tid map. ──
    std::unordered_map<uint32_t,int> depth;
    std::vector<uint32_t> order;
    std::queue<uint32_t> q;
    for (uint32_t s : seeds) {
        if (s < g_map.hdr->n_counties && !depth.count(s)) {
            depth[s] = 0; q.push(s); order.push_back(s);
        }
    }
    int truncated_at = -1;
    while (!q.empty()) {
        uint32_t ci = q.front(); q.pop();
        int d = depth[ci];
        if (d >= hops) continue;
        const ck3map::CountyRecord& c = g_map.county[ci];
        bool stop = false;
        // Clamp the CSR window to the baked edge array: a corrupt c_edge_start past
        // the edge count means this county has no readable edges (the loader does not
        // validate per-record CSR bounds). 64-bit accumulation avoids uint32 wrap.
        if (c.c_edge_start >= g_map.hdr->n_county_edges) continue;
        for (uint16_t e = 0; e < c.c_edge_count; e++) {
            uint64_t ei = (uint64_t)c.c_edge_start + e;
            if (ei >= g_map.hdr->n_county_edges) break;
            uint32_t ni = g_map.cedge[ei].to_county_idx;
            if (ni >= g_map.hdr->n_counties) continue;
            if (depth.count(ni)) continue;
            if ((int)order.size() >= max_nodes) { truncated_at = d + 1; stop = true; break; }
            depth[ni] = d + 1;
            order.push_back(ni);
            q.push(ni);
        }
        if (stop) break;
    }

    // ── build sigil text (mirrors run_dump's NODES/EDGES blocks + live overlay) ──
    std::string body;
    body.reserve(order.size() * 160);
    body += "# CK3MAP scope="; body += scope;
    body += " hops="; body += std::to_string(hops);
    body += " dist=px\n";
    body += "# legend: @=county +=dejure[d/k/e] !=holder(id) ~=faith *=culture "
            "T=terrain dev=N ctl=N% (cap/titular/holy flags)\n";
    body += "# NODES\n";

    std::vector<std::string> edge_lines;

    for (uint32_t ci : order) {
        const ck3map::CountyRecord& c = g_map.county[ci];
        const char* key = mg_str(c.key_off);

        body += "@"; body += (key && *key ? key : "?");

        // +dejure[d/k/e] — STATIC from the artifact, zero memory reads
        {
            const char* dk = mg_title_key(c.duchy_tid);
            const char* kk = mg_title_key(c.kingdom_tid);
            const char* ek = mg_title_key(c.empire_tid);
            body += " +";
            body += (dk && *dk) ? dk : "-"; body += "/";
            body += (kk && *kk) ? kk : "-"; body += "/";
            body += (ek && *ek) ? ek : "-";
        }

        // static flags (CountyFlags)
        if (c.flags & ck3map::CF_IS_CAPITAL)    body += " cap";
        if (c.flags & ck3map::CF_IS_TITULAR)    body += " titular";
        if (c.flags & ck3map::CF_HAS_HOLY_SITE) body += " holy";

        // static terrain (baked)
        const char* terr = mg_county_terrain(c);
        if (terr) { body += " T"; body += terr; }

        // ── LIVE overlay: resolve the baked KEY to a RUNTIME title, then enrich ──
        if (!brief && key && *key) {
            uint32_t rt = resolve_title_id_by_key(key);
            if (rt != 0xFFFFFFFFu) {
                uintptr_t tobj = lookup_title_obj(rt);
                if (tobj) {
                    uint32_t holder = safe_read<uint32_t>(tobj + ck3::title_obj::HOLDER_CHAR_ID) & 0xFFFFFF;
                    if (holder && holder != 0xFFFFFF) {
                        body += " !"; body += std::to_string(holder);
                    }
                }
                uintptr_t cdata = mg_resolve_county_data(rt);
                if (cdata && is_mapped(cdata)) {
                    std::string fa = county_faith_name(cdata);
                    std::string cu = county_culture_name(cdata);
                    if (!fa.empty()) { body += " ~"; body += fa; }
                    if (!cu.empty()) { body += " *"; body += cu; }
                    int32_t dev = safe_read<int32_t>(cdata + ck3::county::DEVELOPMENT);
                    if (dev >= 0 && dev <= 1000) { body += " dev"; body += std::to_string(dev); }
                    int64_t ctl_fp = safe_read<int64_t>(cdata + ck3::county::CONTROL);
                    double ctl = (double)ctl_fp / 100000.0;
                    if (ctl >= 0.0 && ctl <= 110.0) { body += " ctl"; body += std::to_string((int)(ctl + 0.5)); }
                }
            }
        }
        body += "\n";

        // collect in-window edges (induced subgraph) for the EDGES block
        std::string el = "@"; el += (key && *key ? key : "?"); el += " >";
        bool any = false;
        bool edges_ok = (c.c_edge_start < g_map.hdr->n_county_edges);
        for (uint16_t e = 0; edges_ok && e < c.c_edge_count; e++) {
            uint64_t ei = (uint64_t)c.c_edge_start + e;
            if (ei >= g_map.hdr->n_county_edges) break;
            const ck3map::CountyEdge& ce = g_map.cedge[ei];
            if (ce.to_county_idx >= g_map.hdr->n_counties || !depth.count(ce.to_county_idx)) continue;
            const char* tk = mg_str(g_map.county[ce.to_county_idx].key_off);
            el += " "; el += (tk && *tk ? tk : "?");
            if (!brief) {
                el += ":"; el += std::to_string(ce.centroid_dist);
                el += mg_cross_sigil(ce.crossing);
            }
            any = true;
        }
        if (any) edge_lines.push_back(std::move(el));
    }

    body += "# EDGES\n";
    for (auto& l : edge_lines) { body += l; body += "\n"; }

    if (truncated_at >= 0) {
        body += "# truncated: more counties at hop "; body += std::to_string(truncated_at);
        body += " — narrow scope or lower hops/max_nodes\n";
    }
    if (!note.empty()) { body += "# note: "; body += note; body += "\n"; }

    ResponseBuilder r;
    r.begin_object();
    r.key("scope");      r.value_string(scope.c_str(), scope.size());
    r.key("node_count"); r.value_int((int)order.size());
    r.key("hops");       r.value_int(hops);
    r.key("truncated");  r.value_bool(truncated_at >= 0);
    r.key("dist_units"); r.value_string("px");
    r.key("text");       r.value_string(body.c_str(), body.size());
    r.key("status");     r.value_string("ok");
    r.end_object();
    if (out) *out = r.take();
    return true;
}

} // namespace bridge
