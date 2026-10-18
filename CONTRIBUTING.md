# Contributing

Thanks for looking. A few things that are not obvious from the tree:

**This repository is a build product.** The code is developed in a private
research checkout and exported here by a manifest-driven script, with fresh
commits per release (see `UPSTREAM` for the exact upstream commit). Pull
requests are welcome and reviewed; accepted changes are ported upstream and
appear in the next export, so your commit will be re-authored in the sync
commit. If that matters to you, say so in the PR and we will credit you in the
changelog.

**Where things are.**

| Path | What |
|------|------|
| `bridge/` | the C++ bridge (`libck3_bridge.so`); `src/dispatch_router.cpp` is the IPC handler table |
| `re/offsets/` | the reverse-engineering contract: struct offsets, byte signatures, the supported BuildID |
| `ck3agent/` | the Python package: `cli.py`, and `server/` (the MCP server and bridge client, shared verbatim with upstream — bare sibling imports by design) |
| `installer/` | the tarball's `install.sh` |
| `docs/` | user docs, reference, and ADRs |

**Build and test.** See `docs/building.md`. In short:

```bash
cmake -S bridge -B bridge/build -DCK3_STRICT_BUILD_ID=ON -DBUILD_BRIDGE_TESTS=ON
cmake --build bridge/build --target ck3_bridge test_json_helpers -j"$(nproc)"
python -m pip install -e ".[test]" && python -m pytest
```

CI runs those on every push. Nothing in CI touches the game; changes to the
hooks or to offsets need a live check against CK3 1.18.3.1 on Linux — say in
the PR what you ran.

**Offsets are sacred.** `re/offsets/ck3_offsets.h` records confidence and
evidence per entry. Never replace a HIGH-confidence value with a guess; when
you find one wrong, document why it was wrong beside the fix.

**Ports.** The design of record for Windows and macOS is
`docs/adr/0001-windows-macos-port.md`. Open an issue before starting a port
so the work can be split.

**Conduct.** Be kind, be specific, assume good faith.
