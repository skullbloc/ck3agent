# Building from source

## Bridge

```bash
sudo apt-get install cmake build-essential git          # Debian/Ubuntu
cmake -S bridge -B bridge/build \
      -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCK3_STRICT_BUILD_ID=ON \
      -DBUILD_BRIDGE_TESTS=ON
cmake --build bridge/build --target ck3_bridge test_json_helpers -j"$(nproc)"
./bridge/build/test_json_helpers
```

Produces `bridge/build/libck3_bridge.so`. Notes:

* **Zydis** is fetched from GitHub and built from source at a pinned commit
  the first time you configure (needs network once). The headers in
  `bridge/third_party/zydis/include` are the same version.
* **`CK3_STRICT_BUILD_ID`** defaults to OFF in CMake and ON in release builds;
  the public installers are always strict. The environment variable of the
  same name overrides the compiled default at launch (`0` loosens, `1`
  tightens).
* **`BUILD_BRIDGE_TESTS`** adds ~50 engine-free test executables. CI builds
  and runs a few of them; the rest are acceptance probes from the research
  tree that still compile here.
* The library must not depend on anything but glibc, libstdc++, pthread, dl
  and rt (`ldd` it). Release builds are made on Ubuntu 22.04 for glibc 2.35
  compatibility.
* `-DCK3_BUILD_MAPGEN=ON` also builds `tools/ck3mapgen`, a map pre-processor
  used by the optional map-graph query; it needs the game's data files.

Use your build: `ck3agent setup --bridge $PWD/bridge/build/libck3_bridge.so`
or `CK3AGENT_BRIDGE=... ck3agent launch`.

## Python package

```bash
python -m pip install -e ".[test]"
python -m pytest                     # server suite + CLI tests, no game needed
python -m build                      # wheel + sdist into dist/
```

`ck3agent/server/` is shared verbatim with the upstream research tree and uses
bare sibling imports (`from tools import …`); `ck3agent.bootstrap_server()`
puts that directory on `sys.path`, which is what the CLI does before importing
`mcp_server`. Do not convert them to package-relative imports here; the change
belongs upstream.

## Versioning

`VERSION` at the root is the single source: CMake reads it for the library
version, hatch reads `ck3agent/_version.py` (written from it), and the release
workflow refuses a tag that does not match it.

## Release pipeline

`.github/workflows/release.yml` runs on a `v*` tag: builds the bridge on
Ubuntu 22.04, splits debug symbols, builds the wheel, assembles the tarball,
attests provenance, and opens a **draft** GitHub Release. See the workflow for
the exact asset list.
