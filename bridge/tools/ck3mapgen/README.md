# ck3mapgen — build-time CK3 map-processor (P1)

Bakes the immutable CK3 map *skeleton* into a compact mmap artifact
(`ck3map.bin`) that the bridge loads at init and paints live state onto. This
is **P1** of `re/findings/re_agent_map_design.md`: the static-only graph
(topology + de-jure hierarchy + distance), the province→county reverse index,
and the §5 text serializer. No game process is needed — it reads static files.

## What it produces

A county-granularity adjacency graph with a province-granularity sub-layer:

- **Nodes** = counties (`c_*`), each carrying de-jure duchy/kingdom/empire,
  color, centroid, capital/titular flags, and its baronies (province ids).
- **Edges** = territorial adjacency carrying `centroid_dist` (pixel
  straight-line), `crossing` (land / strait / river / impassable),
  `border_strength` (# province-pairs touching), and the strait/river
  `through_prov`.
- **Reverse index** `province_id → {barony, county, duchy, kingdom, empire}`
  (the bake of the bridge's current O(n) inline title scan).

Topology + distance come from **provinces.png** pixel borders + centroids;
strait/river edges come from **adjacencies.csv** (the pixel scan can't see
crossings where two land tiles don't touch). The engine's in-memory
`SAdjacency[]` is the offline validation oracle, not the build source.

## Inputs (all static, game down)

Under `<game-dir>` (the CK3 install `.../game` directory):
`map_data/{definition.csv, provinces.png, adjacencies.csv, default.map}` and
`common/landed_titles/*.txt`.

## Build

Standalone (recommended; self-contained, only needs the vendored
`bridge/third_party/stb_image.h`):

```bash
cmake -S bridge/tools/ck3mapgen -B build-mapgen
cmake --build build-mapgen
```

Or via the bridge build (opt-in, never affects the default `.so` build):

```bash
cmake -S bridge -B bridge/build -DCK3_BUILD_MAPGEN=ON
cmake --build bridge/build --target ck3mapgen
```

Or via the deploy pipeline (also stages the artifact at `/tmp/ck3map.bin`):

```bash
scripts/bdt.sh mapgen        # standalone
scripts/bdt.sh restart       # regenerates as part of a deploy+restart
```

## Run

```bash
# Bake the artifact
ck3mapgen --game-dir "/path/to/Crusader Kings III/game" --out ck3map.bin

# Dump the §5 LLM text format for a county scope (static, no live overlay)
ck3mapgen --bin ck3map.bin --dump-scope c_roma --hops 2
```

## Artifact format

`ck3map.bin` is little-endian, 8-byte aligned, CSR adjacency — see
`ck3map_format.h` for the exact record layout. A `ck3map.bin.meta.json` sidecar
carries `{version, map_hash, n_prov, n_counties, n_county_edges,
adjacency_source, distance_source, modded, expansion}` so the bridge can
validate the blob (hash of `definition.csv` + `provinces.png` size/mtime)
without parsing it.

## Verified numbers (modded "Asia" install)

- provinces parsed: 13,269 (max id 13,269)
- counties: 4,321 (3,096 with ≥1 barony; 1,225 titular/landless)
- baronies: 10,060 · titles: 15,693
- province edges: 77,718 (directed) · county edges: 14,186 (directed)
- edges fully symmetric, no self-loops, symmetric distances match
- crossings: 13,838 land / 166 strait / 182 river
- `centroid_dist` px: min 17, median 70, p95 136, max 308
- deterministic (byte-identical output across runs)

## P2 static enrichment (done — terrain + holy sites)

The baked graph now carries static-derived attributes resolved at build time:

- **Terrain** — `common/province_terrain/*.txt` (`<province_id> = <key>`, with a
  `default = <key>` fallback) is resolved to the stable `Terrain` enum in
  `ck3map_format.h` and written to every `ProvinceRecord.terrain` and
  `BaronyRecord.terrain`. The enum is a fixed build-time contract because CK3's
  own terrain *index* is data-driven (terrain_types load order) and not stable
  across versions/mods; we key on the terrain *string* instead.
- **Holy sites** — `common/religion/holy_sites/*.txt` (`county = c_*` /
  `barony = b_*`) set the reserved `CF_HAS_HOLY_SITE` (county) and
  `PF_HAS_HOLY_SITE` (province) flags. Faith association is a later phase (no
  faith field is wired into the on-disk record yet).

The `--dump-scope` text output now shows `T<terrain>` per node and a `★holy`
marker; the meta sidecar records `terrain_source`, `holy_site_source`,
`n_terrain_explicit`, `terrain_default`, and `n_holy_site_titles`. The struct
layout is unchanged (these fields were already reserved), so artifacts remain
`VERSION 1` and the P1 loader reads them with the new fields simply zero.

## Deferred (later phases)

Remaining P2 items (combat-profile side-table keyed by terrain, special-building
slots, geographical regions, history-bootstrap holding types) and
P3 bridge loader + live overlay, P4 `query_map_graph` BFS windowing + MCP tool,
P5 engine-`SAdjacency` parity validation + `dist=days` calibration. The format
reserves `holding_type` for the history-bootstrap pass so it grows without a
version bump.
