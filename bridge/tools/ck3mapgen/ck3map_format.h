// ck3map_format.h — on-disk artifact layout for ck3map.bin
//
// Produced by ck3mapgen (build-time map-processor), consumed by the bridge
// loader. mmap-friendly: little-endian, 8-byte aligned, CSR adjacency.
//
// This is the P1 subset of the design in re/findings/re_agent_map_design.md
// §3.4. Fields not yet populated by P1 (terrain combat profiles, holy sites,
// regions, history holdings) are reserved/zero so the format can grow without
// a version bump breaking the header parse.
//
// All "*_tid" are the mapgen-local dense title index (see TitleRecord); the
// bridge maps these to runtime LANDED_TITLE_DB ids via the title key strings.
// The county graph is keyed by the dense county index (0..n_counties).

#ifndef CK3MAP_FORMAT_H
#define CK3MAP_FORMAT_H

#include <cstdint>

namespace ck3map {

constexpr char     MAGIC[8]  = {'C','K','3','M','A','P','\0','\0'};
constexpr uint32_t VERSION   = 1;

// ----- terrain enum (P2) ----------------------------------------------------
// Stable build-time contract between ck3mapgen and the bridge consumer. CK3's
// own terrain *index* is data-driven (load order of terrain_types/00_terrains.txt)
// and therefore NOT stable across versions/mods, so the baked artifact uses this
// fixed mapping instead. The mapgen resolves a province's terrain *key string*
// from common/province_terrain/*.txt and maps it here; unknown keys -> TERRAIN_UNKNOWN.
// Keys mirror the 15 vanilla terrain types (re_cheap_read_offsets_terrain_*),
// plus the coastal/sea pseudo-terrains the engine also recognizes.
enum Terrain : uint8_t {
    TERRAIN_UNKNOWN        = 0,
    TERRAIN_PLAINS         = 1,
    TERRAIN_FARMLANDS      = 2,
    TERRAIN_HILLS          = 3,
    TERRAIN_MOUNTAINS      = 4,
    TERRAIN_DESERT         = 5,
    TERRAIN_DESERT_MOUNTAINS = 6,
    TERRAIN_OASIS          = 7,
    TERRAIN_JUNGLE         = 8,
    TERRAIN_FOREST         = 9,
    TERRAIN_TAIGA          = 10,
    TERRAIN_WETLANDS       = 11,
    TERRAIN_STEPPE         = 12,
    TERRAIN_FLOODPLAINS    = 13,
    TERRAIN_DRYLANDS       = 14,
    TERRAIN_TERRACED_HILLS = 15,
    TERRAIN_SEA            = 16,
    TERRAIN_COASTAL_SEA    = 17,
    TERRAIN_OCEAN          = 18,
    TERRAIN_LAKES          = 19,
    TERRAIN_MAX            = 20,
};

// ----- crossing / edge type enum -------------------------------------------
enum CrossingType : uint8_t {
    CROSS_LAND       = 0,
    CROSS_STRAIT     = 1,   // adjacencies.csv Type=sea
    CROSS_RIVER      = 2,   // adjacencies.csv Type=river_large
    CROSS_IMPASSABLE = 3,
};

// ----- province flags ------------------------------------------------------
enum ProvFlags : uint16_t {
    PF_LAND          = 1 << 0,
    PF_SEA           = 1 << 1,
    PF_LAKE          = 1 << 2,
    PF_IMPASSABLE    = 1 << 3,
    PF_RIVER         = 1 << 4,
    PF_HAS_HOLY_SITE = 1 << 5,
};

// ----- county flags --------------------------------------------------------
enum CountyFlags : uint16_t {
    CF_IS_CAPITAL    = 1 << 0,   // de-jure capital of its duchy
    CF_IS_TITULAR    = 1 << 1,   // landless / no provinces
    CF_HAS_HOLY_SITE = 1 << 2,
};

#pragma pack(push, 1)

// 64-byte header. Section offsets are absolute byte offsets into the file.
struct Header {
    char     magic[8];          // "CK3MAP\0\0"
    uint32_t version;
    uint32_t _pad0;
    uint64_t map_hash;          // FNV-1a over definition.csv + provinces.png size/mtime
    uint32_t n_prov;            // dense province count (max province id + 1)
    uint32_t n_counties;
    uint32_t n_titles;
    uint32_t n_prov_edges;      // directed (each undirected edge stored twice)
    uint32_t n_county_edges;    // directed
    uint32_t n_baronies;
    uint64_t off_provinces;     // ProvinceRecord[n_prov]
    uint64_t off_counties;      // CountyRecord[n_counties]
    uint64_t off_prov_edges;    // ProvEdge[n_prov_edges]
    uint64_t off_county_edges;  // CountyEdge[n_county_edges]
    uint64_t off_baronies;      // BaronyRecord[n_baronies]
    uint64_t off_titles;        // TitleRecord[n_titles]
    uint64_t off_strpool;       // null-terminated UTF-8 string pool
    uint64_t strpool_size;
};

// Indexed directly by province_id (dense 0..n_prov).
struct ProvinceRecord {        // 48 bytes
    uint32_t province_id;
    uint16_t flags;            // ProvFlags
    uint8_t  terrain;          // terrain enum id (0 = unknown), P2
    uint8_t  _pad;
    uint32_t barony_tid;       // dense title index of owning barony (or 0xFFFFFFFF)
    uint32_t county_idx;       // dense county index (or 0xFFFFFFFF if none)
    uint32_t duchy_tid;
    uint32_t kingdom_tid;
    uint32_t empire_tid;
    uint32_t p_edge_start;     // CSR into ProvEdge[]
    uint16_t p_edge_count;
    uint16_t centroid_x;       // pixel space
    uint16_t centroid_y;
    uint16_t _pad2;
    uint32_t name_off;         // into string pool (province name from definition.csv)
};

struct CountyRecord {          // 56 bytes
    uint32_t county_tid;       // dense title index of the county title
    uint32_t key_off;          // "c_roma" into string pool
    uint32_t name_off;         // display name (county title key fallback)
    uint16_t flags;            // CountyFlags
    uint16_t _pad;
    uint32_t duchy_tid;
    uint32_t kingdom_tid;
    uint32_t empire_tid;
    uint32_t color_rgb;        // packed R<<16|G<<8|B
    uint16_t centroid_x;
    uint16_t centroid_y;
    uint32_t barony_start;     // into BaronyRecord[]
    uint16_t barony_count;
    uint16_t _pad2;
    uint32_t c_edge_start;     // CSR into CountyEdge[]
    uint16_t c_edge_count;
    uint16_t _pad3;
};

struct ProvEdge {              // 12 bytes
    uint32_t to_prov;
    uint32_t dist;             // euclidean pixel distance between province centroids
    uint8_t  type;             // CrossingType
    uint8_t  _pad;
    uint16_t through;          // sea/river province for strait crossings (or 0)
};

struct CountyEdge {            // 12 bytes
    uint32_t to_county_idx;    // dense county index
    uint32_t centroid_dist;    // euclidean pixel distance between county centroids
    uint8_t  crossing;         // CrossingType (strongest constraint across collapsed edges)
    uint8_t  border_strength;  // # of province-pairs touching (saturated at 255)
    uint16_t through_prov;     // through water province (0 if land)
};

struct BaronyRecord {          // 16 bytes
    uint32_t province_id;
    uint32_t barony_tid;       // dense title index
    uint32_t key_off;          // "b_roma" into string pool
    uint8_t  terrain;          // P2
    uint8_t  holding_type;     // P2
    uint16_t _pad;
};

struct TitleRecord {           // 24 bytes
    uint32_t title_idx;        // == its own dense index (self-referential, for clarity)
    uint8_t  tier;             // 0=barony 1=county 2=duchy 3=kingdom 4=empire
    uint8_t  _pad0;
    uint16_t _pad1;
    uint32_t parent_tid;       // de-jure parent dense index (0xFFFFFFFF = none)
    uint32_t color_rgb;
    uint32_t capital_province; // capital province id when declared (else 0)
    uint32_t key_off;          // "d_latium" into string pool
};

#pragma pack(pop)

// Tier constants for TitleRecord.tier
enum Tier : uint8_t {
    TIER_BARONY  = 0,
    TIER_COUNTY  = 1,
    TIER_DUCHY   = 2,
    TIER_KINGDOM = 3,
    TIER_EMPIRE  = 4,
};

constexpr uint32_t NO_IDX = 0xFFFFFFFFu;

} // namespace ck3map

#endif // CK3MAP_FORMAT_H
