# Helping with the Windows and macOS port

ck3agent supports one binary: CK3 1.18.3.1 on Linux x86_64. The plan is
`docs/adr/0001-windows-macos-port.md` (phases P0 to P7). Windows comes first,
macOS is deferred. Most of P0 to P3 needs no Windows or Mac machine.

## Ways to help, smallest first

1. **File a binary report.** Get the Windows or macOS files (SteamCMD works from
   Linux, see `proton-runbook.md`), run `python3 tools/inspect_binary.py <file>
   --json`, paste the output into a `binary-report` issue.
2. **Answer a question** from `questions.md`. Each names the field it fills.
3. **Check a runbook step.** Steps nobody has run are marked UNVERIFIED. Run
   one, report what happened.
4. **Report an offset** with the `offset-finding` template.
5. **Take a port task** (`port-task` template, label `port`). Good first ones,
   all doable on Linux: `ck3agent/_pe.py` and `_macho.py` beside `_elf.py`, a PE
   RTTI walker, per-ABI container readers, the `bridge/src/os/` interfaces.

## Where port code goes

The platform layer is `bridge/include/os/os.h`: it declares every row of the
ADR's "Platform abstraction" table (library entry, process identity, image
base and section table, binary identity, guarded read and write, the guarded
region, executable-memory allocation and unwind registration, exec
interposition, IPC transport, paths, input, threads, watchdog), plus the two
ABI-bound pieces that need the game binary (RTTI vtable lookup, hook planning).
Each call returns a typed `Result` (`ok` / `failed` / `not_routed` /
`not_implemented` / `unavailable_on_platform`).

- **`bridge/src/os/linux/os_linux.cpp`** forwards to the existing Linux code
  (the bridge does not link the layer yet, so Linux behaviour cannot change
  until a call site is moved onto it).
- **`bridge/src/os/macos/stubs.cpp`** is typed refusals; macOS is deferred.
- **`bridge/src/os/windows/`** holds the P2 Win32 cells, one file each:
  `process.cpp`, `entry.cpp` + `dll_main.cpp`, `image.cpp`, `identity.cpp`,
  `safe_read.cpp`, `write_protected.cpp`, `threads.cpp`, `paths.cpp`,
  `transport.cpp`, `exec_alloc.cpp`, `guarded.cpp`, `watchdog.cpp`, with
  `common.cpp` (shared Win32 helpers) and `win_internal.h`. Everything not yet
  a cell stays a typed refusal in `windows/stubs.cpp` (`input_click`,
  `rtti_find_vtable`, `hook_plan`; `exec_interposition` is
  `unavailable_on_platform` — a proxy DLL replaces it).
- **Portable helpers** used by the cells live beside the layer and build on all
  three OSes: `bridge/src/os/sha256.cpp`, `pe_image.cpp`, `win_paths.cpp`.
- **The Windows DLL** `ck3_bridge_win` is built as `version.dll` (phase P4): the
  layer, a forward for every export of the real `version.dll`
  (`windows/proxy.cpp`; the `.def` is generated from `windows/version_exports.h`
  and `version_exports_extra.h`), and a `DllMain` that starts one init thread.
  That thread runs the identity gate against the pinned 1.18.3.1 `ck3.exe`
  (`windows/pinned_identity.h`) and logs `match` or `REFUSED`; it starts nothing
  else (no hooks, no IPC listener) on either. Tests: `test_os_win_identity_gate`,
  `test_os_win_proxy_load`, `test_os_win_proxy_refused`, `test_os_win_proxy_exports`.

Configure with `-DCK3_PLATFORM=windows` (MSVC, or `clang-cl` + `xwin` from
Linux — see below) or `-DCK3_PLATFORM=macos` (Apple clang); the CMake file then
stops before the Linux bridge. `-DBUILD_BRIDGE_TESTS=ON` builds the tests: a
portable `test_os_<helper>` everywhere, `test_os_stubs` for the refusals, and a
`test_os_win_<cell>` per Windows cell. CI runs these on `windows-latest` and
`macos-14`; the ADR's "P2 cell status" table lists which cell each test covers.

### Cross-building the Windows layer from Linux

`bridge/cmake/toolchains/clang-cl-xwin.cmake` builds the Windows leg with the
MSVC ABI on a Linux host, and its tests run under Wine (MEDIUM evidence until
re-run on real Windows, per the ADR). The host this was developed on used:

- **llvm-mingw** (provides `clang-cl`, `lld-link`, `llvm-lib`) unpacked under
  `~/.local/opt`, with driver symlinks in `~/.local/opt/clangcl-bin`;
- **xwin** (`xwin --accept-license splat --output ~/.local/opt/xwin-sdk`) for
  the MSVC CRT and the Windows SDK — the licence is the downloader's to accept;
- system **Wine** as the `ctest` emulator.

```bash
cmake -S bridge -B build-win -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=bridge/cmake/toolchains/clang-cl-xwin.cmake \
  -DCK3_PLATFORM=windows -DBUILD_BRIDGE_TESTS=ON
cmake --build build-win && ctest --test-dir build-win   # tests run under wine
```

Replacing a refusal with a real cell is a port task.

## Rules

- **Never share the game.** No binaries, PDBs, depots, Ghidra projects or long
  decompilations in issues or PRs. Hashes, offsets and short quotes are fine.
- Own a licensed copy. The Paradox EULA and Steam Subscriber Agreement are on
  you. Single-player only.
- One game version everywhere: 1.18.3.1, from the Steam Betas rollback branch.
- Offsets are load-base-relative (a PE offset is an RVA). Grade findings: HIGH is
  three sources or a decompile, MEDIUM two, LOW one. Never replace HIGH with a guess.
- Proton results are MEDIUM until repeated on real Windows.
- Open an issue before a large PR. Sign off commits (DCO).
