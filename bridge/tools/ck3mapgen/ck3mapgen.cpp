// ck3mapgen — build-time CK3 map-processor (P1).
//
// Parses the static map files (definition.csv, default.map, adjacencies.csv,
// landed_titles/*.txt, provinces.png) into a compact mmap artifact
// (ck3map.bin) plus a sidecar ck3map.meta.json, and ships a text serializer
// (artifact -> scoped LLM text) per re/findings/re_agent_map_design.md §5.
//
// Build source (per the design's resolved adjacency question): topology +
// distance come from provinces.png pixel borders + centroids; straits/rivers
// come from adjacencies.csv. The engine's in-memory SAdjacency[] is NOT used
// here (it is the offline validation oracle, not the build source).
//
// Usage:
//   ck3mapgen --game-dir <CK3 install game/> --out <ck3map.bin>
//             [--meta <ck3map.meta.json>] [--quiet]
//   ck3mapgen --bin <ck3map.bin> --dump-scope <c_xxx> [--hops N]   (text dump)
//
// Self-contained: depends only on stb_image.h (vendored) and the C++17 stdlib.

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "ck3map_format.h"
#include "pdx_parser.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <dirent.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace ck3map;

// ===========================================================================
// Small utilities
// ===========================================================================

static bool g_quiet = false;
#define LOGF(...) do { if (!g_quiet) fprintf(stderr, __VA_ARGS__); } while (0)

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::vector<std::string> list_dir_txt(const std::string& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (!d) return out;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name.size() > 4 && name.substr(name.size() - 4) == ".txt")
            out.push_back(name);
    }
    closedir(d);
    return out;
}

static uint64_t fnv1a(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// ===========================================================================
// String pool — dedup-by-offset accumulator
// ===========================================================================
struct StringPool {
    std::string buf;
    std::unordered_map<std::string, uint32_t> seen;
    StringPool() { buf.push_back('\0'); }  // offset 0 == empty string
    uint32_t intern(const std::string& s) {
        if (s.empty()) return 0;
        auto it = seen.find(s);
        if (it != seen.end()) return it->second;
        uint32_t off = (uint32_t)buf.size();
        buf += s;
        buf.push_back('\0');
        seen.emplace(s, off);
        return off;
    }
};

// ===========================================================================
// Parsed game data model (in-memory before serialization)
// ===========================================================================

struct DefRow { uint32_t id; uint8_t r, g, b; std::string name; };

struct TitleNode {
    std::string key;
    uint8_t     tier = 255;
    uint32_t    parent = NO_IDX;     // dense title index of de-jure parent
    uint32_t    color_rgb = 0;
    uint32_t    capital_province = 0;
    bool        landless = false;
    // for counties: barony children (province ids). for higher: child titles.
    std::vector<uint32_t> baronies_prov;   // province ids in a county
};

// ===========================================================================
// 1. definition.csv
// ===========================================================================
struct Definitions {
    uint32_t max_id = 0;
    std::vector<DefRow> rows;                 // sparse-by-vector, indexed by id
    std::unordered_map<uint32_t, uint32_t> rgb_to_id;  // packed RGB -> id
};

static uint32_t pack_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static Definitions parse_definitions(const std::string& path) {
    Definitions d;
    std::string text = read_file(path);
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // id;R;G;B;name;x;
        std::vector<std::string> f;
        size_t start = 0;
        for (;;) {
            size_t pos = line.find(';', start);
            if (pos == std::string::npos) { f.push_back(line.substr(start)); break; }
            f.push_back(line.substr(start, pos - start));
            start = pos + 1;
        }
        if (f.size() < 4) continue;
        DefRow row;
        row.id = (uint32_t)strtoul(f[0].c_str(), nullptr, 10);
        if (row.id == 0) continue;  // id 0 is the dummy "x" row
        row.r = (uint8_t)strtoul(f[1].c_str(), nullptr, 10);
        row.g = (uint8_t)strtoul(f[2].c_str(), nullptr, 10);
        row.b = (uint8_t)strtoul(f[3].c_str(), nullptr, 10);
        row.name = f.size() > 4 ? f[4] : "";
        if (row.id > d.max_id) d.max_id = row.id;
        d.rows.push_back(row);
        d.rgb_to_id[pack_rgb(row.r, row.g, row.b)] = row.id;
    }
    return d;
}

// ===========================================================================
// 2. default.map — accumulate sea/lake/river/impassable id classes
// ===========================================================================
struct ProvClasses {
    std::unordered_set<uint32_t> sea, lake, river, impassable;
};

static void accumulate_class(const std::string& body, std::unordered_set<uint32_t>& out) {
    // body is the text after '{' up to matching '}', a list of ints
    std::istringstream ss(body);
    std::string tok;
    while (ss >> tok) {
        uint32_t v = (uint32_t)strtoul(tok.c_str(), nullptr, 10);
        if (v) out.insert(v);
    }
}

static ProvClasses parse_default_map(const std::string& path) {
    ProvClasses pc;
    std::string text = read_file(path);
    // strip comments line-by-line
    std::istringstream in(text);
    std::string line, clean;
    while (std::getline(in, line)) {
        size_t h = line.find('#');
        if (h != std::string::npos) line = line.substr(0, h);
        clean += line;
        clean += '\n';
    }
    // scan for `<key> = RANGE { a b }` or `<key> = LIST { ... }` (case-insensitive RANGE/LIST)
    size_t i = 0;
    auto target_for = [&](const std::string& key) -> std::unordered_set<uint32_t>* {
        if (key == "sea_zones") return &pc.sea;
        if (key == "lakes") return &pc.lake;
        if (key == "river_provinces") return &pc.river;
        if (key == "impassable_mountains" || key == "impassable_seas") return &pc.impassable;
        return nullptr;
    };
    while (i < clean.size()) {
        // read a key token
        while (i < clean.size() && (isspace((unsigned char)clean[i]))) ++i;
        size_t ks = i;
        while (i < clean.size() && (isalnum((unsigned char)clean[i]) || clean[i] == '_')) ++i;
        std::string key = clean.substr(ks, i - ks);
        if (key.empty()) { ++i; continue; }
        // expect '='
        while (i < clean.size() && isspace((unsigned char)clean[i])) ++i;
        if (i >= clean.size() || clean[i] != '=') continue;
        ++i;
        while (i < clean.size() && isspace((unsigned char)clean[i])) ++i;
        // optional RANGE/LIST keyword
        size_t ws = i;
        while (i < clean.size() && isalpha((unsigned char)clean[i])) ++i;
        std::string kw = clean.substr(ws, i - ws);
        bool is_range = (kw == "RANGE" || kw == "range");
        bool is_list  = (kw == "LIST"  || kw == "list");
        if (!is_range && !is_list) continue;
        while (i < clean.size() && isspace((unsigned char)clean[i])) ++i;
        if (i >= clean.size() || clean[i] != '{') continue;
        ++i;
        size_t bs = i;
        while (i < clean.size() && clean[i] != '}') ++i;
        std::string body = clean.substr(bs, i - bs);
        if (i < clean.size()) ++i; // consume '}'
        auto* tgt = target_for(key);
        if (!tgt) continue;
        if (is_range) {
            std::istringstream rs(body);
            uint32_t a = 0, b = 0;
            rs >> a >> b;
            if (a && b) for (uint32_t v = a; v <= b; ++v) tgt->insert(v);
        } else {
            accumulate_class(body, *tgt);
        }
    }
    return pc;
}

// ===========================================================================
// 3. landed_titles — recursive-descent de-jure tree
// ===========================================================================
struct Titles {
    std::vector<TitleNode> nodes;
    std::unordered_map<std::string, uint32_t> key_to_idx;   // "c_roma" -> dense idx
    std::unordered_map<uint32_t, uint32_t> prov_to_county;  // province id -> county idx
    std::unordered_map<uint32_t, uint32_t> prov_to_barony;  // province id -> barony title idx
};

static uint32_t color_from_tokens(pdx::Lexer& lex) {
    // We are positioned just after the '=' for a color. The next token is
    // either '{' (RGB list) or "hsv" then '{'. Returns packed RGB (hsv -> 0,
    // we don't convert; color is non-critical for P1 topology).
    char punct;
    std::string t = lex.next(punct);
    bool is_hsv = false;
    if (t == "hsv" || t == "hsv360" || t == "rgb") {
        is_hsv = (t != "rgb");
        t = lex.next(punct);  // should be '{'
    }
    if (punct != '{' && t != "{") return 0;
    // read up to 3-4 numbers until '}'
    std::vector<double> vals;
    for (;;) {
        std::string v = lex.next(punct);
        if (punct == '}' || v == "}") break;
        if (v.empty()) break;
        vals.push_back(atof(v.c_str()));
        if (vals.size() > 8) break;
    }
    if (is_hsv) return 0;  // skip hsv conversion in P1
    if (vals.size() >= 3) {
        uint8_t r = (uint8_t)std::min(255.0, std::max(0.0, vals[0]));
        uint8_t g = (uint8_t)std::min(255.0, std::max(0.0, vals[1]));
        uint8_t b = (uint8_t)std::min(255.0, std::max(0.0, vals[2]));
        return pack_rgb(r, g, b);
    }
    return 0;
}

// Recursively parse the contents of a title block (we are positioned just
// after its opening '{'). Returns the dense index of the created node.
// parent_idx is the de-jure parent (NO_IDX at top level).
static void parse_title_body(pdx::Lexer& lex, Titles& T, uint32_t self_idx);

static uint32_t make_title(Titles& T, const std::string& key) {
    auto it = T.key_to_idx.find(key);
    if (it != T.key_to_idx.end()) return it->second;
    uint32_t idx = (uint32_t)T.nodes.size();
    TitleNode n;
    n.key = key;
    n.tier = pdx::tier_of(key);
    T.nodes.push_back(std::move(n));
    T.key_to_idx.emplace(key, idx);
    return idx;
}

// Skip an opaque '{ ... }' block (we are just after its '{').
static void skip_block(pdx::Lexer& lex) {
    int depth = 1;
    char punct;
    while (depth > 0) {
        std::string t = lex.next(punct);
        if (t.empty()) break;
        if (punct == '{') ++depth;
        else if (punct == '}') --depth;
    }
}

static void parse_title_body(pdx::Lexer& lex, Titles& T, uint32_t self_idx) {
    char punct;
    for (;;) {
        std::string key = lex.next(punct);
        if (key.empty()) return;          // EOF
        if (punct == '}') return;         // end of this block
        if (punct == '{' || punct == '=') continue;  // stray

        // peek the '='
        std::string eq = lex.next(punct);
        if (punct != '=' && eq != "=") {
            // not a key=val; if it opened a block, skip it
            if (punct == '{') skip_block(lex);
            continue;
        }

        // now read the value (token or block)
        if (key == "color" || key == "color2") {
            uint32_t c = color_from_tokens(lex);
            if (key == "color" && self_idx != NO_IDX) T.nodes[self_idx].color_rgb = c;
            continue;
        }
        if (pdx::is_title_key(key)) {
            // a nested title or a barony province binding
            std::string v = lex.next(punct);
            if (punct == '{') {
                uint32_t child = make_title(T, key);
                if (self_idx != NO_IDX) T.nodes[child].parent = self_idx;
                parse_title_body(lex, T, child);
            } else {
                // unexpected (title key without block) — skip
            }
            continue;
        }
        // scalar or block value for a known/unknown key
        std::string v = lex.next(punct);
        if (punct == '{') {
            // opaque block (can_create, ai_primary_priority, province as block? no)
            skip_block(lex);
            continue;
        }
        // scalar key=val on the current title
        if (self_idx == NO_IDX) continue;
        TitleNode& node = T.nodes[self_idx];
        if (key == "province") {
            uint32_t pid = (uint32_t)strtoul(v.c_str(), nullptr, 10);
            if (pid) {
                node.baronies_prov.push_back(pid);
                T.prov_to_barony[pid] = self_idx;
                // attribute the province to the nearest ancestor county
                uint32_t anc = node.parent;
                while (anc != NO_IDX && T.nodes[anc].tier != TIER_COUNTY)
                    anc = T.nodes[anc].parent;
                if (anc != NO_IDX) T.prov_to_county[pid] = anc;
            }
        } else if (key == "capital") {
            // capital = c_xxx is a county *key*; resolve to a province id in a
            // post-pass once all counties are known. Stash the raw key by node.
            extern std::unordered_map<uint32_t, std::string> g_capital_keys;
            g_capital_keys[self_idx] = v;
        } else if (key == "landless") {
            node.landless = (v == "yes");
        }
    }
}

std::unordered_map<uint32_t, std::string> g_capital_keys;

static void parse_landed_titles_file(const std::string& path, Titles& T) {
    std::string text = read_file(path);
    if (text.empty()) return;
    pdx::Lexer lex(text);
    // top level: skip @vars and non-title keys; recurse into title blocks
    char punct;
    for (;;) {
        std::string key = lex.next(punct);
        if (key.empty()) break;
        if (punct == '}' || punct == '{' || punct == '=') continue;
        std::string eq = lex.next(punct);
        if (punct != '=' && eq != "=") {
            if (punct == '{') skip_block(lex);
            continue;
        }
        if (pdx::is_title_key(key)) {
            std::string v = lex.next(punct);
            if (punct == '{') {
                uint32_t idx = make_title(T, key);
                T.nodes[idx].parent = NO_IDX;
                parse_title_body(lex, T, idx);
            }
        } else {
            // @var = value, or some top-level key = value/block
            std::string v = lex.next(punct);
            if (punct == '{') skip_block(lex);
        }
    }
}

// ===========================================================================
// 4. provinces.png — single raster pass: adjacency + centroid + area
// ===========================================================================
struct PixelData {
    int width = 0, height = 0;
    std::vector<uint32_t> centroid_x;   // indexed by province id (sum then mean)
    std::vector<uint32_t> centroid_y;
    std::vector<uint32_t> area;         // pixel count per province id
    // undirected province adjacency with border strength
    std::map<std::pair<uint32_t,uint32_t>, uint32_t> edges;  // (lo,hi)->count
};

static void add_border(PixelData& pd, uint32_t a, uint32_t b) {
    if (a == b || a == 0 || b == 0) return;
    if (a > b) std::swap(a, b);
    pd.edges[{a, b}]++;
}

static bool parse_provinces_png(const std::string& path, const Definitions& def,
                                PixelData& pd) {
    int w, h, comp;
    unsigned char* img = stbi_load(path.c_str(), &w, &h, &comp, 3);
    if (!img) {
        fprintf(stderr, "ck3mapgen: failed to decode %s: %s\n",
                path.c_str(), stbi_failure_reason());
        return false;
    }
    pd.width = w;
    pd.height = h;
    uint32_t maxid = def.max_id;
    pd.centroid_x.assign(maxid + 1, 0);
    pd.centroid_y.assign(maxid + 1, 0);
    pd.area.assign(maxid + 1, 0);

    // Map every pixel to a province id. Build a per-row id buffer to avoid
    // recomputing the rgb->id hash for the "up" neighbor.
    std::vector<uint32_t> row_ids(w), prev_ids(w);

    auto rgb_id = [&](size_t off) -> uint32_t {
        uint8_t r = img[off], g = img[off+1], b = img[off+2];
        auto it = def.rgb_to_id.find(pack_rgb(r, g, b));
        return it == def.rgb_to_id.end() ? 0 : it->second;
    };

    for (int y = 0; y < h; ++y) {
        size_t base = (size_t)y * w * 3;
        for (int x = 0; x < w; ++x) {
            uint32_t id = rgb_id(base + (size_t)x * 3);
            row_ids[x] = id;
            if (id && id <= maxid) {
                pd.centroid_x[id] += (uint32_t)x;
                pd.centroid_y[id] += (uint32_t)y;
                pd.area[id]++;
            }
            // left neighbor border
            if (x > 0 && row_ids[x-1] != id) add_border(pd, row_ids[x-1], id);
            // up neighbor border
            if (y > 0 && prev_ids[x] != id) add_border(pd, prev_ids[x], id);
        }
        std::swap(prev_ids, row_ids);
    }
    stbi_image_free(img);

    // finalize centroids (mean). Use double for the divide.
    for (uint32_t id = 1; id <= maxid; ++id) {
        if (pd.area[id]) {
            pd.centroid_x[id] = pd.centroid_x[id] / pd.area[id];
            pd.centroid_y[id] = pd.centroid_y[id] / pd.area[id];
        }
    }
    return true;
}

// ===========================================================================
// 5b. province_terrain — province_id -> terrain key (P2 static enrichment)
//
// Format (common/province_terrain/*.txt): a `default = <key>` fallback plus
// per-province `<province_id> = <key>` lines. We resolve the key string to the
// stable Terrain enum in ck3map_format.h. The engine's own terrain index is
// load-order/data-driven (unstable), so the baked artifact uses the fixed map.
// ===========================================================================
static uint8_t terrain_id_from_key(const std::string& k) {
    static const std::unordered_map<std::string, uint8_t> m = {
        {"plains", TERRAIN_PLAINS}, {"farmlands", TERRAIN_FARMLANDS},
        {"hills", TERRAIN_HILLS}, {"mountains", TERRAIN_MOUNTAINS},
        {"desert", TERRAIN_DESERT}, {"desert_mountains", TERRAIN_DESERT_MOUNTAINS},
        {"oasis", TERRAIN_OASIS}, {"jungle", TERRAIN_JUNGLE},
        {"forest", TERRAIN_FOREST}, {"taiga", TERRAIN_TAIGA},
        {"wetlands", TERRAIN_WETLANDS}, {"steppe", TERRAIN_STEPPE},
        {"floodplains", TERRAIN_FLOODPLAINS}, {"drylands", TERRAIN_DRYLANDS},
        {"terraced_hills", TERRAIN_TERRACED_HILLS},
        {"sea", TERRAIN_SEA}, {"sea_zone", TERRAIN_SEA},
        {"coastal_sea", TERRAIN_COASTAL_SEA}, {"ocean", TERRAIN_OCEAN},
        {"lakes", TERRAIN_LAKES}, {"lake", TERRAIN_LAKES},
    };
    auto it = m.find(k);
    return it == m.end() ? (uint8_t)TERRAIN_UNKNOWN : it->second;
}

struct ProvTerrain {
    uint8_t def = TERRAIN_UNKNOWN;                 // `default = <key>` fallback
    std::unordered_map<uint32_t, uint8_t> by_id;   // explicit province_id -> terrain
};

static ProvTerrain parse_province_terrain(const std::string& dir) {
    ProvTerrain pt;
    // The folder is loaded in sorted order; later files override earlier ones,
    // matching the engine's flat-merge of common/province_terrain/*.txt.
    std::vector<std::string> files = list_dir_txt(dir);
    std::sort(files.begin(), files.end());
    for (const auto& fn : files) {
        std::string text = read_file(dir + "/" + fn);
        if (text.empty()) continue;
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) {
            size_t h = line.find('#');
            if (h != std::string::npos) line = line.substr(0, h);
            // split on '='
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string lhs = line.substr(0, eq);
            std::string rhs = line.substr(eq + 1);
            auto trim = [](std::string& s) {
                size_t a = s.find_first_not_of(" \t\r\n");
                size_t b = s.find_last_not_of(" \t\r\n");
                if (a == std::string::npos) { s.clear(); return; }
                s = s.substr(a, b - a + 1);
            };
            trim(lhs); trim(rhs);
            if (lhs.empty() || rhs.empty()) continue;
            uint8_t tid = terrain_id_from_key(rhs);
            if (lhs == "default") { pt.def = tid; continue; }
            // lhs must be a numeric province id
            char* end = nullptr;
            unsigned long pid = strtoul(lhs.c_str(), &end, 10);
            if (end == lhs.c_str() || *end != '\0' || pid == 0) continue;
            pt.by_id[(uint32_t)pid] = tid;
        }
    }
    return pt;
}

// ===========================================================================
// 5c. religion/holy_sites — county -> holy site marker (P2 static enrichment)
//
// Each holy_site block carries `county = c_xxx` (and historically `barony =`).
// We only need the boolean "this county/province hosts a holy site" for the
// reserved CF_HAS_HOLY_SITE / PF_HAS_HOLY_SITE flags; the faith association is
// a later phase (the on-disk struct has no faith field wired yet).
// ===========================================================================
static std::unordered_set<std::string> parse_holy_site_counties(const std::string& dir) {
    std::unordered_set<std::string> out;
    std::vector<std::string> files = list_dir_txt(dir);
    std::sort(files.begin(), files.end());
    for (const auto& fn : files) {
        std::string text = read_file(dir + "/" + fn);
        if (text.empty()) continue;
        pdx::Lexer lex(text);
        char punct;
        std::string lhs;     // last identifier seen before an '='
        bool after_eq = false;
        for (;;) {
            std::string t = lex.next(punct);
            if (t.empty() && punct == 0) break;   // EOF
            if (punct == '=') { after_eq = true; continue; }
            if (punct == '{' || punct == '}') { after_eq = false; lhs.clear(); continue; }
            // bareword/value token
            if (after_eq) {
                // value for `lhs = t`: capture `county = c_xxx` / `barony = b_xxx`
                if ((lhs == "county" || lhs == "barony") &&
                    (t.rfind("c_", 0) == 0 || t.rfind("b_", 0) == 0))
                    out.insert(t);
                after_eq = false;
                lhs.clear();
            } else {
                lhs = t;
            }
        }
    }
    return out;
}

// ===========================================================================
// 5. adjacencies.csv — strait/river special edges
// ===========================================================================
struct SpecialEdge { uint32_t from, to, through; uint8_t crossing; };

static std::vector<SpecialEdge> parse_adjacencies(const std::string& path) {
    std::vector<SpecialEdge> out;
    std::string text = read_file(path);
    std::istringstream ss(text);
    std::string line;
    bool first = true;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (first) { first = false; continue; }  // header row
        // From;To;Type;Through;...
        std::vector<std::string> f;
        size_t start = 0;
        for (;;) {
            size_t pos = line.find(';', start);
            if (pos == std::string::npos) { f.push_back(line.substr(start)); break; }
            f.push_back(line.substr(start, pos - start));
            start = pos + 1;
        }
        if (f.size() < 4) continue;
        long from = strtol(f[0].c_str(), nullptr, 10);
        long to   = strtol(f[1].c_str(), nullptr, 10);
        if (from < 0 || to < 0) continue;  // -1 terminator / blanks
        SpecialEdge e;
        e.from = (uint32_t)from;
        e.to = (uint32_t)to;
        e.through = (uint32_t)std::max(0L, strtol(f[3].c_str(), nullptr, 10));
        const std::string& ty = f[2];
        if (ty.find("river") != std::string::npos) e.crossing = CROSS_RIVER;
        else e.crossing = CROSS_STRAIT;  // "sea" and anything else strait-like
        out.push_back(e);
    }
    return out;
}

// ===========================================================================
// Build the artifact
// ===========================================================================

static uint32_t euclid(int ax, int ay, int bx, int by, int mapw) {
    int dx = ax - bx;
    int dy = ay - by;
    int adx = dx < 0 ? -dx : dx;
    // E-W wrap guard (conservative: only shrinks distance for real neighbors)
    if (mapw > 0 && adx > mapw / 2) adx = mapw - adx;
    double d = std::sqrt((double)adx * adx + (double)dy * dy);
    return (uint32_t)(d + 0.5);
}

struct Built {
    Header header{};
    std::vector<ProvinceRecord> provinces;
    std::vector<CountyRecord>   counties;
    std::vector<ProvEdge>       prov_edges;
    std::vector<CountyEdge>     county_edges;
    std::vector<BaronyRecord>   baronies;
    std::vector<TitleRecord>    titles;
    StringPool pool;
};

int run_generate(const std::string& game_dir, const std::string& out_path,
                 const std::string& meta_path);
int run_dump(const std::string& bin_path, const std::string& county_key, int hops);

int main(int argc, char** argv) {
    std::string game_dir, out_path, meta_path, bin_path, dump_county;
    int hops = 2;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (a == "--game-dir") game_dir = next();
        else if (a == "--out") out_path = next();
        else if (a == "--meta") meta_path = next();
        else if (a == "--bin") bin_path = next();
        else if (a == "--dump-scope") dump_county = next();
        else if (a == "--hops") hops = atoi(next().c_str());
        else if (a == "--quiet") g_quiet = true;
        else if (a == "-h" || a == "--help") {
            printf("ck3mapgen --game-dir <dir> --out <ck3map.bin> [--meta <json>] [--quiet]\n");
            printf("ck3mapgen --bin <ck3map.bin> --dump-scope <c_xxx> [--hops N]\n");
            return 0;
        }
    }
    if (!dump_county.empty()) {
        if (bin_path.empty()) { fprintf(stderr, "--dump-scope needs --bin\n"); return 2; }
        return run_dump(bin_path, dump_county, hops);
    }
    if (game_dir.empty() || out_path.empty()) {
        fprintf(stderr, "usage: ck3mapgen --game-dir <dir> --out <ck3map.bin>\n");
        return 2;
    }
    if (meta_path.empty()) meta_path = out_path + ".meta.json";
    return run_generate(game_dir, out_path, meta_path);
}

// ---------------------------------------------------------------------------

static void resolve_capitals(Titles& T, const Definitions& /*def*/) {
    // capital = c_xxx -> capital_province = first barony province of that county
    for (auto& kv : g_capital_keys) {
        uint32_t idx = kv.first;
        const std::string& ckey = kv.second;
        auto it = T.key_to_idx.find(ckey);
        if (it == T.key_to_idx.end()) continue;
        uint32_t cidx = it->second;
        if (!T.nodes[cidx].baronies_prov.empty())
            T.nodes[idx].capital_province = T.nodes[cidx].baronies_prov[0];
    }
}

int run_generate(const std::string& game_dir, const std::string& out_path,
                 const std::string& meta_path) {
    std::string map_data = game_dir + "/map_data";
    std::string common   = game_dir + "/common";

    LOGF("[1/7] definition.csv ...\n");
    Definitions def = parse_definitions(map_data + "/definition.csv");
    if (def.rows.empty()) { fprintf(stderr, "no provinces parsed\n"); return 1; }
    LOGF("      provinces: %zu, max_id: %u\n", def.rows.size(), def.max_id);

    LOGF("[2/7] default.map ...\n");
    ProvClasses pc = parse_default_map(map_data + "/default.map");
    LOGF("      sea=%zu lake=%zu river=%zu impassable=%zu\n",
         pc.sea.size(), pc.lake.size(), pc.river.size(), pc.impassable.size());

    LOGF("[3/7] landed_titles/*.txt ...\n");
    Titles T;
    {
        std::string lt = common + "/landed_titles";
        std::vector<std::string> files = list_dir_txt(lt);
        std::sort(files.begin(), files.end());   // deterministic load order
        for (auto& fn : files) parse_landed_titles_file(lt + "/" + fn, T);
    }
    resolve_capitals(T, def);
    LOGF("      titles: %zu, prov->county: %zu, prov->barony: %zu\n",
         T.nodes.size(), T.prov_to_county.size(), T.prov_to_barony.size());

    LOGF("[4/7] provinces.png (pixel pass) ...\n");
    PixelData pd;
    if (!parse_provinces_png(map_data + "/provinces.png", def, pd)) return 1;
    LOGF("      raster %dx%d, province edges: %zu\n",
         pd.width, pd.height, pd.edges.size());

    LOGF("[5/7] adjacencies.csv (straits/rivers) ...\n");
    std::vector<SpecialEdge> specials = parse_adjacencies(map_data + "/adjacencies.csv");
    LOGF("      special edges: %zu\n", specials.size());

    // P2 static enrichment: terrain + holy sites.
    ProvTerrain pterr = parse_province_terrain(common + "/province_terrain");
    std::unordered_set<std::string> holy = parse_holy_site_counties(common + "/religion/holy_sites");
    LOGF("      [P2] province_terrain: %zu explicit (default=%u), holy-site titles: %zu\n",
         pterr.by_id.size(), (unsigned)pterr.def, holy.size());

    // -----------------------------------------------------------------------
    // Assemble dense county index. A county is any title with tier==COUNTY.
    // -----------------------------------------------------------------------
    LOGF("[6/7] assembling graph ...\n");
    Built B;
    B.titles.resize(T.nodes.size());
    // county dense index map: title idx -> county dense idx
    std::vector<uint32_t> title_to_county(T.nodes.size(), NO_IDX);

    // First: title records + count counties
    for (uint32_t i = 0; i < T.nodes.size(); ++i) {
        const TitleNode& n = T.nodes[i];
        TitleRecord& tr = B.titles[i];
        tr.title_idx = i;
        tr.tier = (n.tier == 255) ? 0 : n.tier;
        tr.parent_tid = n.parent;
        tr.color_rgb = n.color_rgb;
        tr.capital_province = n.capital_province;
        tr.key_off = B.pool.intern(n.key);
    }
    for (uint32_t i = 0; i < T.nodes.size(); ++i) {
        if (T.nodes[i].tier == TIER_COUNTY) {
            title_to_county[i] = (uint32_t)B.counties.size();
            B.counties.emplace_back();
        }
    }

    // ancestor resolution helper: walk parent chain for a given tier
    auto ancestor_of_tier = [&](uint32_t idx, uint8_t want) -> uint32_t {
        uint32_t a = idx;
        while (a != NO_IDX) {
            if (T.nodes[a].tier == want) return a;
            a = T.nodes[a].parent;
        }
        return NO_IDX;
    };

    // Fill county records (de-jure + identity); baronies filled next.
    for (uint32_t ti = 0; ti < T.nodes.size(); ++ti) {
        if (T.nodes[ti].tier != TIER_COUNTY) continue;
        uint32_t ci = title_to_county[ti];
        CountyRecord& cr = B.counties[ci];
        cr.county_tid = ti;
        cr.key_off = B.titles[ti].key_off;
        cr.name_off = cr.key_off;  // P1: name == key (display localization is P2)
        cr.flags = 0;
        if (T.nodes[ti].landless)
            cr.flags |= CF_IS_TITULAR;   // refined below once baronies counted
        uint32_t d = ancestor_of_tier(ti, TIER_DUCHY);
        uint32_t k = ancestor_of_tier(ti, TIER_KINGDOM);
        uint32_t e = ancestor_of_tier(ti, TIER_EMPIRE);
        cr.duchy_tid   = d;
        cr.kingdom_tid = k;
        cr.empire_tid  = e;
        cr.color_rgb = T.nodes[ti].color_rgb;
        // de-jure duchy capital flag: county is its duchy's `capital`
        if (d != NO_IDX) {
            auto it = g_capital_keys.find(d);
            if (it != g_capital_keys.end() && it->second == T.nodes[ti].key)
                cr.flags |= CF_IS_CAPITAL;
        }
    }

    // Collect each county's barony title nodes (direct de-jure children that
    // are baronies — the b_* blocks parse as child titles of the county).
    std::vector<std::vector<uint32_t>> county_baronies(B.counties.size());
    for (uint32_t bi = 0; bi < T.nodes.size(); ++bi) {
        if (T.nodes[bi].tier != TIER_BARONY) continue;
        uint32_t parent = T.nodes[bi].parent;
        if (parent == NO_IDX || T.nodes[parent].tier != TIER_COUNTY) continue;
        uint32_t ci = title_to_county[parent];
        if (ci != NO_IDX) county_baronies[ci].push_back(bi);
    }

    // Barony records, grouped per county (CSR-style barony_start/count).
    for (uint32_t ci = 0; ci < B.counties.size(); ++ci) {
        CountyRecord& cr = B.counties[ci];
        cr.barony_start = (uint32_t)B.baronies.size();
        bool county_has_holy = holy.count(T.nodes[cr.county_tid].key) != 0;
        for (uint32_t btid : county_baronies[ci]) {
            const TitleNode& bn = T.nodes[btid];
            uint32_t pid = bn.baronies_prov.empty() ? 0 : bn.baronies_prov[0];
            BaronyRecord br{};
            br.province_id = pid;
            br.barony_tid = btid;
            br.key_off = B.titles[btid].key_off;
            // P2 terrain: explicit province_terrain entry, else folder default.
            if (pid) {
                auto tit = pterr.by_id.find(pid);
                br.terrain = (tit != pterr.by_id.end()) ? tit->second : pterr.def;
            }
            if (holy.count(bn.key)) county_has_holy = true;  // barony-level holy site
            B.baronies.push_back(br);
        }
        cr.barony_count = (uint16_t)(B.baronies.size() - cr.barony_start);
        if (cr.barony_count == 0) cr.flags |= CF_IS_TITULAR;
        if (county_has_holy) cr.flags |= CF_HAS_HOLY_SITE;
    }

    // -----------------------------------------------------------------------
    // Province records (dense by id) + classification flags + de-jure tags
    // -----------------------------------------------------------------------
    uint32_t n_prov = def.max_id + 1;
    B.provinces.assign(n_prov, ProvinceRecord{});
    for (auto& pr : B.provinces) {
        pr.county_idx = NO_IDX;
        pr.barony_tid = NO_IDX;
        pr.duchy_tid = NO_IDX;
        pr.kingdom_tid = NO_IDX;
        pr.empire_tid = NO_IDX;
    }
    // name + rgb from definition
    std::unordered_map<uint32_t, const DefRow*> id_row;
    for (auto& row : def.rows) id_row[row.id] = &row;

    for (uint32_t id = 1; id <= def.max_id; ++id) {
        ProvinceRecord& pr = B.provinces[id];
        pr.province_id = id;
        auto rit = id_row.find(id);
        if (rit != id_row.end() && !rit->second->name.empty())
            pr.name_off = B.pool.intern(rit->second->name);
        // classify
        uint16_t fl = 0;
        bool sea = pc.sea.count(id), lake = pc.lake.count(id),
             river = pc.river.count(id), imp = pc.impassable.count(id);
        if (sea) fl |= PF_SEA;
        if (lake) fl |= PF_LAKE;
        if (river) fl |= PF_RIVER;
        if (imp) fl |= PF_IMPASSABLE;
        if (!sea && !lake && !river && !imp) fl |= PF_LAND;
        pr.flags = fl;
        // centroid
        if (id < pd.centroid_x.size() && pd.area[id]) {
            pr.centroid_x = (uint16_t)std::min<uint32_t>(65535, pd.centroid_x[id]);
            pr.centroid_y = (uint16_t)std::min<uint32_t>(65535, pd.centroid_y[id]);
        }
        // de-jure via barony binding
        auto cit = T.prov_to_county.find(id);
        if (cit != T.prov_to_county.end()) {
            uint32_t ti = cit->second;
            pr.county_idx = title_to_county[ti];
            pr.duchy_tid = ancestor_of_tier(ti, TIER_DUCHY);
            pr.kingdom_tid = ancestor_of_tier(ti, TIER_KINGDOM);
            pr.empire_tid = ancestor_of_tier(ti, TIER_EMPIRE);
        }
        auto bit = T.prov_to_barony.find(id);
        if (bit != T.prov_to_barony.end()) {
            pr.barony_tid = bit->second;
            // P2 holy-site flag: this province's barony (or its county) is a holy site.
            if (holy.count(T.nodes[bit->second].key)) pr.flags |= PF_HAS_HOLY_SITE;
            else if (cit != T.prov_to_county.end() &&
                     holy.count(T.nodes[cit->second].key))
                pr.flags |= PF_HAS_HOLY_SITE;
        }
        // P2 terrain: explicit province_terrain entry, else folder default.
        {
            auto tit = pterr.by_id.find(id);
            pr.terrain = (tit != pterr.by_id.end()) ? tit->second : pterr.def;
        }
    }

    // -----------------------------------------------------------------------
    // Province edge set: pixel land borders ∪ special edges (strait/river).
    // We carry a crossing type per directed edge.
    // -----------------------------------------------------------------------
    struct PEdge { uint32_t to; uint32_t dist; uint8_t type; uint16_t through; uint8_t bs; };
    std::vector<std::vector<PEdge>> padj(n_prov);

    auto prov_centroid = [&](uint32_t id, int& x, int& y) -> bool {
        if (id >= n_prov) return false;
        if (id < pd.area.size() && pd.area[id]) {
            x = (int)pd.centroid_x[id];
            y = (int)pd.centroid_y[id];
            return true;
        }
        return false;
    };

    // pixel land borders
    for (auto& kv : pd.edges) {
        uint32_t a = kv.first.first, b = kv.first.second;
        if (a >= n_prov || b >= n_prov) continue;
        uint8_t bs = (uint8_t)std::min<uint32_t>(255, kv.second);
        // crossing classification: if either side is impassable -> impassable;
        // otherwise land (water-to-water borders are kept as land-of-water but
        // they won't surface as county edges).
        uint8_t cross = CROSS_LAND;
        if ((B.provinces[a].flags & PF_IMPASSABLE) || (B.provinces[b].flags & PF_IMPASSABLE))
            cross = CROSS_IMPASSABLE;
        int ax, ay, bx, by;
        uint32_t dist = 0;
        if (prov_centroid(a, ax, ay) && prov_centroid(b, bx, by))
            dist = euclid(ax, ay, bx, by, pd.width);
        padj[a].push_back({b, dist, cross, 0, bs});
        padj[b].push_back({a, dist, cross, 0, bs});
    }
    // special edges (strait / river) — union (avoid duplicating a pixel edge)
    auto has_edge = [&](uint32_t a, uint32_t b) -> bool {
        for (auto& e : padj[a]) if (e.to == b) return true;
        return false;
    };
    for (auto& se : specials) {
        if (se.from >= n_prov || se.to >= n_prov) continue;
        if (has_edge(se.from, se.to)) continue;
        int ax, ay, bx, by;
        uint32_t dist = 0;
        if (prov_centroid(se.from, ax, ay) && prov_centroid(se.to, bx, by))
            dist = euclid(ax, ay, bx, by, pd.width);
        padj[se.from].push_back({se.to, dist, se.crossing, (uint16_t)se.through, 1});
        padj[se.to].push_back({se.from, dist, se.crossing, (uint16_t)se.through, 1});
    }

    // flatten province CSR
    for (uint32_t id = 0; id < n_prov; ++id) {
        B.provinces[id].p_edge_start = (uint32_t)B.prov_edges.size();
        for (auto& e : padj[id]) {
            ProvEdge pe{};
            pe.to_prov = e.to;
            pe.dist = e.dist;
            pe.type = e.type;
            pe.through = e.through;
            B.prov_edges.push_back(pe);
        }
        B.provinces[id].p_edge_count =
            (uint16_t)(B.prov_edges.size() - B.provinces[id].p_edge_start);
    }

    // -----------------------------------------------------------------------
    // Project to county graph. County A<->B exists iff any province in A
    // borders any province in B. border_strength = # province-pairs;
    // crossing = strongest constraint (land < river < strait < impassable
    // for "ease", but we want to report the *kind* — prefer land if any
    // land border exists, else river, else strait).
    // -----------------------------------------------------------------------
    struct CAgg { uint32_t dist_min = 0xFFFFFFFF; uint32_t bs = 0; uint8_t cross = 255; uint16_t through = 0; };
    std::vector<std::map<uint32_t, CAgg>> cadj(B.counties.size());

    auto county_of = [&](uint32_t prov) -> uint32_t {
        if (prov >= n_prov) return NO_IDX;
        return B.provinces[prov].county_idx;
    };
    // crossing precedence for reporting: land is best/most permissive; we keep
    // the most permissive crossing that actually exists between the two counties.
    auto better_cross = [](uint8_t cur, uint8_t cand) -> uint8_t {
        // order of permissiveness: LAND(0) < RIVER(2) < STRAIT(1) < IMPASSABLE(3)
        auto rank = [](uint8_t c) -> int {
            switch (c) { case CROSS_LAND: return 0; case CROSS_RIVER: return 1;
                         case CROSS_STRAIT: return 2; case CROSS_IMPASSABLE: return 3; }
            return 4;
        };
        if (cur == 255) return cand;
        return rank(cand) < rank(cur) ? cand : cur;
    };

    for (uint32_t a = 0; a < n_prov; ++a) {
        uint32_t ca = county_of(a);
        if (ca == NO_IDX) continue;
        uint32_t es = B.provinces[a].p_edge_start;
        uint32_t ec = B.provinces[a].p_edge_count;
        for (uint32_t e = 0; e < ec; ++e) {
            const ProvEdge& pe = B.prov_edges[es + e];
            uint32_t cb = county_of(pe.to_prov);
            if (cb == NO_IDX || cb == ca) continue;
            CAgg& agg = cadj[ca][cb];
            agg.bs++;
            if (pe.dist < agg.dist_min) agg.dist_min = pe.dist;
            agg.cross = better_cross(agg.cross, pe.type);
            if (pe.through && !agg.through) agg.through = pe.through;
        }
    }
    // county centroids = area-weighted mean of member-province centroids
    for (uint32_t ci = 0; ci < B.counties.size(); ++ci) {
        CountyRecord& cr = B.counties[ci];
        uint64_t sx = 0, sy = 0, sw = 0;
        for (uint16_t k = 0; k < cr.barony_count; ++k) {
            uint32_t pid = B.baronies[cr.barony_start + k].province_id;
            if (pid < n_prov && pid < pd.area.size() && pd.area[pid]) {
                uint64_t w = pd.area[pid];
                sx += (uint64_t)pd.centroid_x[pid] * w;
                sy += (uint64_t)pd.centroid_y[pid] * w;
                sw += w;
            }
        }
        if (sw) {
            cr.centroid_x = (uint16_t)std::min<uint64_t>(65535, sx / sw);
            cr.centroid_y = (uint16_t)std::min<uint64_t>(65535, sy / sw);
        }
    }
    // flatten county CSR; recompute centroid_dist from county centroids
    for (uint32_t ci = 0; ci < B.counties.size(); ++ci) {
        CountyRecord& cr = B.counties[ci];
        cr.c_edge_start = (uint32_t)B.county_edges.size();
        for (auto& kv : cadj[ci]) {
            uint32_t cj = kv.first;
            const CAgg& agg = kv.second;
            CountyEdge ce{};
            ce.to_county_idx = cj;
            ce.crossing = (agg.cross == 255) ? (uint8_t)CROSS_LAND : agg.cross;
            ce.border_strength = (uint8_t)std::min<uint32_t>(255, agg.bs);
            ce.through_prov = agg.through;
            ce.centroid_dist = euclid(cr.centroid_x, cr.centroid_y,
                                      B.counties[cj].centroid_x,
                                      B.counties[cj].centroid_y, pd.width);
            B.county_edges.push_back(ce);
        }
        cr.c_edge_count = (uint16_t)(B.county_edges.size() - cr.c_edge_start);
    }

    // -----------------------------------------------------------------------
    // Header + map_hash
    // -----------------------------------------------------------------------
    uint64_t mh;
    {
        std::string defcsv = read_file(map_data + "/definition.csv");
        mh = fnv1a(defcsv.data(), defcsv.size());
        struct stat st{};
        if (stat((map_data + "/provinces.png").c_str(), &st) == 0) {
            uint64_t sz = (uint64_t)st.st_size, mt = (uint64_t)st.st_mtime;
            mh = fnv1a(&sz, sizeof(sz), mh);
            mh = fnv1a(&mt, sizeof(mt), mh);
        }
    }

    Header& H = B.header;
    memcpy(H.magic, MAGIC, 8);
    H.version = VERSION;
    H.map_hash = mh;
    H.n_prov = n_prov;
    H.n_counties = (uint32_t)B.counties.size();
    H.n_titles = (uint32_t)B.titles.size();
    H.n_prov_edges = (uint32_t)B.prov_edges.size();
    H.n_county_edges = (uint32_t)B.county_edges.size();
    H.n_baronies = (uint32_t)B.baronies.size();

    // section layout (8-byte aligned)
    auto align8 = [](uint64_t v) { return (v + 7) & ~7ull; };
    uint64_t off = align8(sizeof(Header));
    H.off_provinces = off;     off += (uint64_t)B.provinces.size() * sizeof(ProvinceRecord);
    off = align8(off);
    H.off_counties = off;      off += (uint64_t)B.counties.size() * sizeof(CountyRecord);
    off = align8(off);
    H.off_prov_edges = off;    off += (uint64_t)B.prov_edges.size() * sizeof(ProvEdge);
    off = align8(off);
    H.off_county_edges = off;  off += (uint64_t)B.county_edges.size() * sizeof(CountyEdge);
    off = align8(off);
    H.off_baronies = off;      off += (uint64_t)B.baronies.size() * sizeof(BaronyRecord);
    off = align8(off);
    H.off_titles = off;        off += (uint64_t)B.titles.size() * sizeof(TitleRecord);
    off = align8(off);
    H.off_strpool = off;       H.strpool_size = B.pool.buf.size();
                               off += B.pool.buf.size();
    uint64_t total = off;

    // -----------------------------------------------------------------------
    // Write
    // -----------------------------------------------------------------------
    LOGF("[7/7] writing %s (%.2f MB) ...\n", out_path.c_str(), total / 1048576.0);
    std::vector<char> blob(total, 0);
    memcpy(blob.data(), &H, sizeof(Header));
    auto put = [&](uint64_t o, const void* src, size_t n) { if (n) memcpy(blob.data()+o, src, n); };
    put(H.off_provinces, B.provinces.data(), B.provinces.size()*sizeof(ProvinceRecord));
    put(H.off_counties, B.counties.data(), B.counties.size()*sizeof(CountyRecord));
    put(H.off_prov_edges, B.prov_edges.data(), B.prov_edges.size()*sizeof(ProvEdge));
    put(H.off_county_edges, B.county_edges.data(), B.county_edges.size()*sizeof(CountyEdge));
    put(H.off_baronies, B.baronies.data(), B.baronies.size()*sizeof(BaronyRecord));
    put(H.off_titles, B.titles.data(), B.titles.size()*sizeof(TitleRecord));
    put(H.off_strpool, B.pool.buf.data(), B.pool.buf.size());

    std::ofstream out(out_path, std::ios::binary);
    if (!out) { fprintf(stderr, "cannot open %s for write\n", out_path.c_str()); return 1; }
    out.write(blob.data(), blob.size());
    out.close();

    // meta sidecar
    {
        std::ofstream m(meta_path);
        m << "{\n";
        m << "  \"version\": " << VERSION << ",\n";
        m << "  \"map_hash\": " << mh << ",\n";
        m << "  \"n_prov\": " << H.n_prov << ",\n";
        m << "  \"n_counties\": " << H.n_counties << ",\n";
        m << "  \"n_titles\": " << H.n_titles << ",\n";
        m << "  \"n_baronies\": " << H.n_baronies << ",\n";
        m << "  \"n_prov_edges\": " << H.n_prov_edges << ",\n";
        m << "  \"n_county_edges\": " << H.n_county_edges << ",\n";
        m << "  \"raster\": [" << pd.width << ", " << pd.height << "],\n";
        m << "  \"adjacency_source\": \"provinces.png+adjacencies.csv\",\n";
        m << "  \"distance_source\": \"pixel_centroid\",\n";
        m << "  \"terrain_source\": \"common/province_terrain\",\n";
        m << "  \"holy_site_source\": \"common/religion/holy_sites\",\n";
        m << "  \"n_terrain_explicit\": " << pterr.by_id.size() << ",\n";
        m << "  \"terrain_default\": " << (unsigned)pterr.def << ",\n";
        m << "  \"n_holy_site_titles\": " << holy.size() << ",\n";
        m << "  \"modded\": true,\n";
        m << "  \"expansion\": \"asia\"\n";
        m << "}\n";
    }

    LOGF("done. counties=%u prov_edges=%u(dir) county_edges=%u(dir) baronies=%u titles=%u\n",
         H.n_counties, H.n_prov_edges, H.n_county_edges, H.n_baronies, H.n_titles);
    return 0;
}

// ===========================================================================
// Text serializer (artifact -> scoped LLM text), §5 of the design.
//
// Reads ck3map.bin back via a thin mmap-style view (we just read into a
// buffer here) and emits the sigil-tagged node/edge format for a county
// scope (seed county + BFS to `hops`). This is the static, no-live-overlay
// path; dynamic fields (ruler/faith/culture/dev) are omitted (the bridge
// paints those at query time).
// ===========================================================================

namespace {

struct MapView {
    std::vector<char> data;
    const Header* H = nullptr;
    const ProvinceRecord* prov = nullptr;
    const CountyRecord*   county = nullptr;
    const ProvEdge*       pedge = nullptr;
    const CountyEdge*     cedge = nullptr;
    const BaronyRecord*   barony = nullptr;
    const TitleRecord*    title = nullptr;
    const char*           pool = nullptr;

    const char* str(uint32_t off) const {
        if (!pool || off >= H->strpool_size) return "";
        return pool + off;
    }
    bool load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::ostringstream ss; ss << f.rdbuf();
        std::string s = ss.str();
        data.assign(s.begin(), s.end());
        if (data.size() < sizeof(Header)) return false;
        H = reinterpret_cast<const Header*>(data.data());
        if (memcmp(H->magic, MAGIC, 8) != 0) return false;
        const char* base = data.data();
        prov   = reinterpret_cast<const ProvinceRecord*>(base + H->off_provinces);
        county = reinterpret_cast<const CountyRecord*>(base + H->off_counties);
        pedge  = reinterpret_cast<const ProvEdge*>(base + H->off_prov_edges);
        cedge  = reinterpret_cast<const CountyEdge*>(base + H->off_county_edges);
        barony = reinterpret_cast<const BaronyRecord*>(base + H->off_baronies);
        title  = reinterpret_cast<const TitleRecord*>(base + H->off_titles);
        pool   = base + H->off_strpool;
        return true;
    }
    uint32_t county_by_key(const std::string& key) const {
        for (uint32_t i = 0; i < H->n_counties; ++i)
            if (key == str(county[i].key_off)) return i;
        return NO_IDX;
    }
};

const char* terrain_key(uint8_t t) {
    switch (t) {
        case TERRAIN_PLAINS: return "plains";
        case TERRAIN_FARMLANDS: return "farmlands";
        case TERRAIN_HILLS: return "hills";
        case TERRAIN_MOUNTAINS: return "mountains";
        case TERRAIN_DESERT: return "desert";
        case TERRAIN_DESERT_MOUNTAINS: return "desert_mountains";
        case TERRAIN_OASIS: return "oasis";
        case TERRAIN_JUNGLE: return "jungle";
        case TERRAIN_FOREST: return "forest";
        case TERRAIN_TAIGA: return "taiga";
        case TERRAIN_WETLANDS: return "wetlands";
        case TERRAIN_STEPPE: return "steppe";
        case TERRAIN_FLOODPLAINS: return "floodplains";
        case TERRAIN_DRYLANDS: return "drylands";
        case TERRAIN_TERRACED_HILLS: return "terraced_hills";
        case TERRAIN_SEA: return "sea";
        case TERRAIN_COASTAL_SEA: return "coastal_sea";
        case TERRAIN_OCEAN: return "ocean";
        case TERRAIN_LAKES: return "lakes";
        default: return "?";
    }
}

const char* cross_sigil(uint8_t c) {
    switch (c) {
        case CROSS_STRAIT: return "s";
        case CROSS_RIVER: return "r";
        case CROSS_IMPASSABLE: return "x";
        default: return "";
    }
}

} // namespace

int run_dump(const std::string& bin_path, const std::string& county_key, int hops) {
    MapView v;
    if (!v.load(bin_path)) {
        fprintf(stderr, "ck3mapgen: cannot load artifact %s\n", bin_path.c_str());
        return 1;
    }
    uint32_t seed = v.county_by_key(county_key);
    if (seed == NO_IDX) {
        fprintf(stderr, "ck3mapgen: county '%s' not found\n", county_key.c_str());
        return 1;
    }
    // BFS to `hops`
    std::unordered_map<uint32_t,int> depth;
    std::queue<uint32_t> q;
    depth[seed] = 0; q.push(seed);
    std::vector<uint32_t> window;
    while (!q.empty()) {
        uint32_t ci = q.front(); q.pop();
        window.push_back(ci);
        int d = depth[ci];
        if (d >= hops) continue;
        const CountyRecord& cr = v.county[ci];
        for (uint16_t e = 0; e < cr.c_edge_count; ++e) {
            uint32_t cj = v.cedge[cr.c_edge_start + e].to_county_idx;
            if (depth.find(cj) == depth.end()) { depth[cj] = d + 1; q.push(cj); }
        }
    }
    std::sort(window.begin(), window.end());

    auto title_key = [&](uint32_t tid) -> const char* {
        if (tid == NO_IDX || tid >= v.H->n_titles) return "";
        return v.str(v.title[tid].key_off);
    };

    // header / legend
    printf("# CK3MAP scope=county:%s+%dhop dist=px\n", county_key.c_str(), hops);
    printf("# legend: @=county ⌖=dejure[d/k/e] H=baronies(N) T=terrain ★=titular/cap/holy\n");
    printf("# EDGES: FROM > TO:dist[:type]  (type: s=strait r=river x=impassable; omitted=land)\n");
    printf("# NODES\n");
    for (uint32_t ci : window) {
        const CountyRecord& cr = v.county[ci];
        const char* key = v.str(cr.key_off);
        const char* dk = title_key(cr.duchy_tid);
        const char* kk = title_key(cr.kingdom_tid);
        const char* ek = title_key(cr.empire_tid);
        printf("@%-22s ⌖%s/%s/%s H%u",
               key,
               *dk ? dk : "-", *kk ? kk : "-", *ek ? ek : "-",
               (unsigned)cr.barony_count);
        // P2 terrain: report the county's primary (first-barony) terrain.
        if (cr.barony_count && cr.barony_start < v.H->n_baronies) {
            uint8_t t = v.barony[cr.barony_start].terrain;
            if (t != TERRAIN_UNKNOWN) printf(" T%s", terrain_key(t));
        }
        if (cr.flags & CF_IS_CAPITAL) printf(" ★cap");
        if (cr.flags & CF_IS_TITULAR) printf(" ★titular");
        if (cr.flags & CF_HAS_HOLY_SITE) printf(" ★holy");
        printf("\n");
    }
    printf("# EDGES\n");
    std::unordered_set<uint32_t> inwin(window.begin(), window.end());
    for (uint32_t ci : window) {
        const CountyRecord& cr = v.county[ci];
        std::string line;
        for (uint16_t e = 0; e < cr.c_edge_count; ++e) {
            const CountyEdge& ce = v.cedge[cr.c_edge_start + e];
            if (!inwin.count(ce.to_county_idx)) continue;  // induced subgraph
            const char* tk = v.str(v.county[ce.to_county_idx].key_off);
            line += " ";
            line += tk;
            line += ":";
            line += std::to_string(ce.centroid_dist);
            const char* cs = cross_sigil(ce.crossing);
            if (*cs) { line += ":"; line += cs; }
        }
        if (!line.empty())
            printf("%-22s >%s\n", v.str(cr.key_off), line.c_str());
    }
    return 0;
}
