# Building from source

Build the bridge library and the Python package yourself.

## Prerequisites

- CMake 3.16+, a C++17 compiler such as GCC 11 or newer, and git.
- Network access on first configure: CMake downloads pinned Zydis and Zycore archives.
- Python 3.11+.

## Steps

1. Build the bridge, or run `make bridge`:

   ```bash
   cmake -S bridge -B bridge/build -DCMAKE_BUILD_TYPE=RelWithDebInfo \
         -DCK3_STRICT_BUILD_ID=ON -DBUILD_BRIDGE_TESTS=ON
   cmake --build bridge/build -j"$(nproc)"
   (cd bridge/build && ctest --output-on-failure)
   ```

   This builds the default target, as CI does: `bridge/build/libck3_bridge.so`,
   the native tests, and `libck3_bridge_stub.so`, the i386 stub, when the
   compiler accepts `-m32` (else CMake skips it with a status line). Naming
   targets explicitly skips the stub.

2. Install the Python package and run the tests, or run `make test`. No game needed.

   ```bash
   python -m pip install -e ".[test]"
   python -m pytest
   ```

3. Use your build: `ck3agent setup --bridge $PWD/bridge/build/libck3_bridge.so`.

## CMake options

| Option | Default | Effect |
|--------|---------|--------|
| `CK3_STRICT_BUILD_ID` | OFF; ON in releases | refuse any other game build at load. The `CK3_STRICT_BUILD_ID` environment variable overrides it at launch. |
| `CK3_BRIDGE_DEBUG_VERBS` | OFF | compile in the debug IPC verbs; see [SECURITY.md](../SECURITY.md) |
| `BUILD_BRIDGE_TESTS` | OFF | build the native tests and probes; `ctest` runs the engine-free ones |
| `CK3_BUILD_MAPGEN` | OFF | build `tools/ck3mapgen`, the map pre-processor for the map-graph query. It needs the game's data files. |
| `CK3_ZYDIS_FROM_SOURCE` | OFF | build Zydis from the pinned archive even if a prebuilt `third_party/zydis/lib64/` exists |

## Notes

- The library exports only `execve`, `execvp`, `posix_spawn` and
  `posix_spawnp`, listed in `bridge/bridge.version`.
- Release builds need glibc 2.34 and libstdc++ from GCC 11 on the target
  machine: symbol versions `GLIBC_2.34` and `GLIBCXX_3.4.29`. CI fails a build that needs more.
- `ck3agent/server/` is shared with the upstream research tree and uses bare
  sibling imports such as `from tools import ...`. `ck3agent.bootstrap_server()`
  puts that directory on `sys.path`. Do not convert them here; change them upstream.
- The export writes `VERSION`, `ck3agent/_version.py` and
  `ck3agent/_bridge_meta.py`. CMake reads `VERSION`; the Python package reads
  `_version.py`. The release workflow fails unless all three match the tag.
- `.github/workflows/release.yml` builds a `v*` tag into a draft GitHub Release.

Release builds set `SOURCE_DATE_EPOCH` to the tag commit's time, sort and
owner-normalise the tarball and gzip it without a timestamp, so the tarball
is reproducible given the same runner image (ubuntu-22.04, GCC 11). The `.so`
is not yet bit-for-bit reproducible across toolchains; `build_provenance.h`
records the commit and whether the tree was clean, and the release workflow
refuses a dirty one. The bundled wheels are whatever PyPI resolves on release
day; `wheels/SHA256SUMS` in the tarball lists exactly what shipped.

## Check it worked

- `ctest` passes.
- `ck3agent doctor` reports your library under "bridge library".

## If it fails

- Zydis download fails: configure once with network access.
- `ldd bridge/build/libck3_bridge.so` lists anything beyond `libc`, `libm`,
  `libstdc++`, `libgcc_s`, the loader and the vDSO: your toolchain linked
  something extra. CI rejects that; report it.
