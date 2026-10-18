# bridge/test/

Engine-free C++ tests; none needs the game. Configure with
`-DBUILD_BRIDGE_TESTS=ON` (`make bridge` does). A file named in
`bridge/CMakeLists.txt` is a target of the same name. Run one:
`./bridge/build/test_json_helpers`.

| Kind | Files | Behavior |
|------|-------|----------|
| Unit tests | `test_*.cpp` | assert; the first failure aborts. CI runs a subset. |
| Probes | `*_probe.cpp` | run every row, print `case<TAB>value`, end with `PASS` or `FAIL`; exit 1 on failure. |
| Acceptances | `acceptance_*.cpp` | like probes, plus `--json` for upstream tooling that also runs a patched-out build, which must fail. |
| Scripts | `*.sh`, `*.py` | host-only checks and generators; the header comment says how to use them. |

## ck-ids

`ck-xxxx` in a comment is an id in the upstream tracker (for example
`ck-dzxr`). It marks the defect or decision behind the line. The tracker is
not public: the comment beside the id states the reason. Keep existing ids.

## Adding a test

Copy the nearest `test_*.cpp`; add its target to `bridge/CMakeLists.txt`.
Keep it free of engine symbols. The CMake comment above `SESSION_HOST_SOURCES`
lists the stubs a target linking `session.cpp` needs.
