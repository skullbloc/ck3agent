# ck3mapgen

Optional, offline. Bakes CK3's static map into `ck3map.bin`, which the bridge
loads at startup for map-graph queries. Reads game files only; no game process.

Output: a county adjacency graph (de-jure duchy/kingdom/empire, centroid,
baronies, edge distance, crossing type: land, strait, river), a province to
county index, terrain and holy-site flags. Layout: `ck3map_format.h`.
Deterministic.

Inputs, under your CK3 `game/` directory: `map_data/{definition.csv,
provinces.png, adjacencies.csv, default.map}`, `common/landed_titles/*.txt`,
`common/province_terrain/*.txt`, `common/religion/holy_sites/*.txt`.

```bash
cmake -S bridge/tools/ck3mapgen -B build-mapgen && cmake --build build-mapgen
./build-mapgen/ck3mapgen --game-dir "/path/to/Crusader Kings III/game" --out ck3map.bin
./build-mapgen/ck3mapgen --bin ck3map.bin --dump-scope c_roma --hops 2   # text view
```

Or build target `ck3mapgen` with `-DCK3_BUILD_MAPGEN=ON` in `bridge/`.

The bridge reads `$CK3MAP_BIN`, else `ck3map.bin` in its private state
directory: `$XDG_RUNTIME_DIR/ck3agent/` (then `/run/user/<uid>/ck3agent/`, then
`~/.local/state/ck3agent/`), `<that>/inst/<id>/` for a named
`CK3_INSTANCE_ID`, or `/tmp/` with `CK3_INSTANCE_LAYOUT=legacy`. It must be a
regular file there, not a symlink. Restart the game after replacing it. The `ck3map.bin.meta.json` sidecar holds source hashes so a stale
artifact is detectable. Uses `bridge/third_party/stb_image.h`.
