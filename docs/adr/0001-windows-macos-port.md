# ADR 0001 — Porting the bridge to Windows and macOS

- **Status:** proposed (design of record for releases after 2026.10.0)
- **Date:** 2026-10-17; amended 2026-10-18 (see "Review amendments")
- **Applies to:** `bridge/` (the in-process game bridge), `re/offsets/`, the
  `ck3agent` CLI and the release pipeline
- **Decision owners:** ck3agent maintainers; contributors doing the port

## Status

Proposed. The 2026.10.0 release supports exactly one binary: Crusader Kings III
1.18.3.1 for Linux x86_64, ELF BuildID `e61ff2642f07094defd140fcc98e078d42541275`
(`re/offsets/offset_version.h`). Nothing here changes that release. This
records how the bridge is expected to reach Windows and macOS, in what order,
and which questions must be answered before port code is worth writing. Items
marked *verify* have not been checked by anyone on the project; they are
prerequisites, not facts.

## Review amendments 2026-10-18

Corrections from the 2026.10.0-rc1 review. Where they conflict with the
sections below, they win. The `W*`/`M*` milestone table is superseded by the
phased plan at the end of this section; its ids stay for cross-reference.

**Design corrections**

- **Container layouts differ per ABI.** Field offsets are not the only thing
  that moves. `std::string` is 32 bytes (libstdc++, 15-char SSO), 24 (libc++,
  22-char SSO) or 32 (MSVC, 15-char SSO), each with its own field order.
  `std::map` is 48, 24 and 16 bytes. `std::function`, `unordered_map` and
  `shared_ptr` control blocks differ too. These are textbook sizes; the P3
  probe measures them, nothing is assumed. The Itanium ABI reuses tail padding
  of a base class for derived members and MSVC does not, so a derived field
  can sit at a different offset from identical declarations. Windows is LLP64
  (`long` is 4 bytes, `wchar_t` 2) and bitfield packing differs. So the 1,757
  struct-field offsets (about 59% of the header) are a candidate set that
  needs one reader per ABI (`os/abi/{itanium_libstdcxx,itanium_libcxx,msvc}`),
  not a validator over the Linux readers.
- **No `longjmp` out of a vectored handler.** It is unsafe on Windows. The
  2,441 `safe_read` sites use `ReadProcessMemory` (as the `os::safe_read` row
  already says). The ~273 direct dereferences use recovery by `Rip` rewrite:
  the region records a resume address and stack pointer thread-locally, the
  handler sets `CONTEXT.Rip`/`Rsp` to them and returns
  `EXCEPTION_CONTINUE_EXECUTION`, and the region reports failure. Same rule as
  today: protected bodies own no resources. This replaces the `os::guarded_region`
  Windows cell.
- **Trampolines need unwind data.** Code in `VirtualAlloc`ed pages has no
  `RUNTIME_FUNCTION`, so an exception or stack walk that crosses it
  fail-fasts. Register each trampoline with `RtlAddFunctionTable`, with unwind
  codes matching the relocated prologue.
- **CFG, CET and MSVC inlining are per-hook questions, not per-process ones.**
  For each hook record: is the call site CFG-checked, is the replacement a
  valid call target, does the trampoline pair `call`/`ret` the way a shadow
  stack accepts. A function the Linux bridge hooks may be inlined, folded by
  identical COMDAT folding (two functions, one address) or split by the
  compiler on MSVC. Existence and uniqueness are checked before a hook is
  planned. `tools/inspect_binary.py` reports the process-level flags.
- **Signing is a W0 decision.** A `version.dll` next to a game is the textbook
  sideload pattern, so Defender, SmartScreen and Smart App Control will react.
  Decide signed (OV/EV certificate) or documented-unsigned, and what the
  installer tells the user, before P2 starts, not at packaging time.
- **A second identity witness, everywhere.** The SHA-256 of the image file on
  disk joins the ELF BuildID, PE `RSDS` and Mach-O `LC_UUID`. Those can be
  re-linked, stripped or (`/Brepro`) not time-derived; a file hash cannot
  drift unnoticed. The bridge computes it off the init path, publishes it as a
  witness (`observed_image_sha256`) and compares it with `IMAGE_SHA256` in
  `offset_version.h`. Reference for Linux 1.18.3.1:
  `5c99966cc6829e25ed65cdda74bbc0548f91cd318010cdb61bffa6d1f3abede8`.
- **PE address convention, defined now.** An offset under `re/offsets/windows/`
  is an RVA: address minus the module load base (`GetModuleHandleW(NULL)`),
  the same meaning as the Linux base-relative offsets. Ghidra imports a PE at
  its optional-header `ImageBase`, so the Ghidra address is `ImageBase + RVA`.
  Never a file offset. `offset_version.h` records `IMAGE_BASE` beside the
  identity constants, and `inspect_binary` prints both.
- **One pinned game version on every platform:** 1.18.3.1, via the Steam
  rollback branch, so the Linux bridge stays the reference. The cross-binary
  anchor transfer tool and the patch-adapt tool are the same tool. No rollback
  branch on a platform means no port to that platform (kill trigger below).
- **Observe-only first.** Steering tools on a platform that has not reached P6
  return a typed refusal, `unavailable_on_platform`, naming the platform and
  the phase that unlocks them. The tool surface stays identical on all
  platforms; handlers refuse, they do not vanish.

**Development without a Windows or macOS machine**

SteamCMD downloads both depots from Linux (`+@sSteamCmdForcePlatformType
windows|macos`, app 1158310), so most of W0/M0 is a static read.
`tools/inspect_binary.py` prints what those questions need, and the
`binary-report` issue template collects its output without anyone sharing a
binary. The Windows build runs under Proton on Linux (`docs/porting/proton-runbook.md`):
Wine implements VEH/SEH, loader order and `WINEDLLOVERRIDES` proxy DLLs, and
loopback TCP reaches the host Python. A `clang-cl` + `xwin` cross-build gives
the MSVC ABI locally. Limits; Proton evidence is MEDIUM until re-run on real
Windows:

- Wine does not enforce CFG or CET, so a hook that passes there can fail-fast
  on Windows.
- Loader lock and `DllMain` ordering follow Wine's loader, not Windows'.
- No Defender, no Smart App Control, no SmartScreen.
- Steam starts the game through its own launcher chain; the Proton chain is not
  the Windows one, and the Steam overlay differs.
- `SendInput` is Wine's X11 translation.
- Faults reach Wine's SEH as translated Unix signals; fault ordering and the
  state in `CONTEXT` can differ from Windows.

**Hybrid path (works today).** A Windows or macOS user runs Claude on their
machine and the Linux game on a Linux host or VM over the network
(`docs/claude-desktop.md`). It needs no port and stays supported.

**macOS: deferred, not dropped.** The macOS build is Intel-only and runs under
Rosetta on Apple Silicon. Apple ends general Rosetta support after macOS 27;
whether a games carve-out follows is undefined. macOS is about 2% of Steam.
Shipping needs a paid Developer ID, notarization and rented Mac hardware.
Steam launches CK3 with `DYLD_INSERT_LIBRARIES` (`steamloader.dylib`), so dyld
insertion works at least from Steam's loader: evidence, not proof. Do M0 (one
day, static, `inspect_binary` on the Mach-O) now. Revisit when Windows has
shipped, a Mac contributor exists, and the macOS 28 games carve-out is known.

**Phased plan** (one experienced contributor; ranges widen with RE uncertainty)

| # | Work | Effort | Done when | Kill / stop trigger |
|---|---|---|---|---|
| P0 | Static spikes from Linux: depots, W0/M0 answers | 3-5 days | `binary-report` answers filled for Windows and macOS; Ghidra imports the PE | Anti-tamper or integrity check found |
| P1 | Windows Ghidra project; anchor transfer by RTTI names, string xrefs, registration tables, BSim | 3-5 wks | Every anchor in `re/offsets/` has a Windows candidate or a documented gap | Transfer below ~40% of anchors with no new method |
| P2 | `bridge/src/os/` layer; stub CI on `windows-latest` and `macos-14` | 2-3 wks (parallel with P1) | Linux acceptance unchanged; engine-free tests pass on 3 OSes | Linux behaviour changes |
| P3 | Proton bring-up; layout probe over the 1,757 field offsets, per-ABI readers | 2-3 wks | Probe report: `agreed`/`disagreed`/`unwitnessed` per field | Under ~40% field agreement |
| P4 | Proxy DLL loads and refuses under Proton | 1-2 wks | `match` on 1.18.3.1, `REFUSED` and no listener on any other build | No 1.18.3.1 rollback branch on Windows |
| P5 | Observe parity | 3-5 wks | Every `query_*` equals the Linux bridge on the reference save (scripted diff) | Parity below half the tools at 2x estimate |
| P6 | Hooks and steering | 3-5 wks | Live scenarios give Linux outcomes (research-tree harness; to be published) | A hook class cannot be made unwind- and CFG-safe |
| P7 | Real-Windows validation on a rented GPU VM; signed zip + `install.ps1` | 2-3 wks | P4-P6 repeated on Windows; installer tested | Proton result does not reproduce on Windows |

Windows preview: 15-20 weeks from start (about 2027.03 for a November 2026
start); parity: 24-30. macOS is not scheduled.

`re/findings/<platform>/` and `bridge/harness/` (cited below) exist in the
research checkout only. The `re/scripts` and `re/findings` subset behind the
string-anchor/RTTI method ships under `re/`: the tools, the two anchor
procedures and how to run them on a new binary are in
[re/README.md](../../re/README.md).

## Context

The bridge is a shared library (`libck3_bridge.so`, ~60 C++17 sources under
`bridge/src/`) that is `LD_PRELOAD`ed into the `ck3` binary. Almost every
mechanism it uses is Linux-specific, and most of them are load-bearing:

| Concern | Current implementation |
|---|---|
| Entry point | `dll_init()` in `bridge/src/bridge_init.cpp`, `__attribute__((constructor))`, calls `bridge::bridge_init()` |
| "Am I in the game?" | `is_ck3_process()` in `bridge/src/bridge_init.cpp`: `readlink("/proc/self/exe")` basename must be exactly `ck3`, because `LD_PRELOAD` also lands the library in Steam and every helper it spawns |
| Image base | `find_binary_base()` in `bridge/src/bridge_init.cpp`: `dl_iterate_phdr`, with `/proc/self/maps` as a diagnostic fallback; section bounds (`.text`, `.rodata`, `.data.rel.ro`) come from program headers in `init_context()` in `bridge/src/runtime_resolver.cpp` |
| Binary identity | `buildid_phdr_callback()` / `check_build_id()` in `bridge/src/runtime_resolver.cpp` walk `PT_NOTE` for `NT_GNU_BUILD_ID` and compare against `ck3_version::BUILD_ID` from `re/offsets/offset_version.h`; the result is published as a witness (`bridge/src/build_id_witness.cpp`) and reported in the IPC `handshake_ack` as `observed_build_id`. With `CK3_STRICT_BUILD_ID` (CMake option, ON in release builds, overridable by the env var of the same name) `bridge_init()` refuses a mismatch: no hooks, no socket |
| Guarded reads | `safe_read<T>()` in `bridge/include/bridge.h` uses `process_vm_readv` on the bridge's own process, which returns `EFAULT` instead of faulting; regions that must dereference directly are wrapped in `sigsetjmp(g_recovery_point)` with `bridge_sigsegv_handler()` doing `siglongjmp` (`bridge/src/bridge_init.cpp`); a `SIGTRAP` handler covers INT3-first hook installation and `SIGALRM` is a dispatch watchdog |
| Memory writes | `write_to_readonly()` in `bridge/include/bridge.h` and `write_code_bytes()` in `bridge/src/ai_eval_hook.cpp` `pwrite` through `/proc/self/mem`, which ignores page protection (RELRO vtables, `.text`); `ScopedWritable` (`mprotect`) exists as the other path |
| Hooks | vtable slot swaps (`install_main_tick_hooks()` in `bridge/src/bridge_init.cpp` patches `CAIMAN_VTABLE`); inline trampolines via `install_hook()` in `bridge/src/hooks.cpp`, which decodes the prologue with Zydis and allocates RWX pages within ±2 GB (`alloc_rwx_near()`, `mmap`); call-site rel32 patches (`arm_score_hook()` in `bridge/src/ai_eval_hook.cpp`) |
| Runtime offset recovery | `bridge/src/runtime_resolver.cpp`: Itanium-ABI RTTI walks (`find_typeinfo()`, `find_vtable()`), byte-pattern scans (`resolve_signatures()`, matcher in `bridge/src/sig_scanner.cpp`, 34 patterns in `re/offsets/signatures.h`), RIP-relative anchors (`resolve_rip_anchors()`), all x86-64 via Zydis 5.0.0 |
| Launcher bypass | `bridge/src/launcher_bypass.cpp` interposes `execve`/`execvp`/`posix_spawn` through `dlsym(RTLD_NEXT, …)`, rewrites a path ending in `/launcher/dowser` to the game binary and re-injects `LD_PRELOAD` into the child's `envp` |
| IPC | `bridge/src/ipc_server.cpp`: an `AF_UNIX` listener (`server_loop()`), a reader thread (`reader_loop()`), NDJSON frames, a typed handshake; the path is `RUN/agent.sock` or `RUN/inst/<id>/agent.sock` (`CK3_INSTANCE_ID`), where `RUN` is the private per-user `$XDG_RUNTIME_DIR/ck3agent`, from `bridge/src/instance_paths.cpp`; `CK3_INSTANCE_LAYOUT=legacy` keeps the old `/tmp` paths |
| UI input | `bridge/src/action_handlers.cpp` `dlopen`s `libX11.so.6` / `libXtst.so.6` for `XTestFakeMotionEvent` / `XTestFakeButtonEvent`, with `/dev/uinput` as the fallback |
| Threads, logs | `std::thread`; `SYS_gettid` in `bridge/include/thread_registry.h`; logs under `RUN` (`instance_paths.cpp`) |
| Build | CMake, `-D_GNU_SOURCE`, Zydis fetched and built from source when the prebuilt `lib64/` is absent (`bridge/CMakeLists.txt`), RapidJSON header-only; native unit tests behind `BUILD_BRIDGE_TESTS` |
| Python side | pure Python (`mcp`, `pydantic`, 3.11+). `ck3agent/cli.py` (`setup`, `doctor`, `launch`, `serve`, `run`, …) reads the ELF BuildID itself (`ck3agent/_elf.py`), sets `LD_PRELOAD` in `cmd_launch()`, and probes the socket path from `ck3agent/server/instance.py` |

The offsets are the other half of the problem. `re/offsets/ck3_offsets.h`
(~7,400 lines) holds base-relative addresses derived in Ghidra from the Linux
ELF. CK3 is one C++ code base compiled per platform (GCC/Clang for Linux, MSVC
for Windows, Clang for macOS). Struct field offsets *may* coincide, since the
same declarations drive layout and both ABIs place plain members in
declaration order with the same x86-64 alignment. Everything else will not:
function, global and vtable addresses, often vtable slot order (MSVC orders
overloaded virtuals in reverse and splits multiple-inheritance vtables
differently), RTTI encoding, prologues (so every byte signature), and calling
conventions (SysV vs Microsoft x64: `this` in `rdi` vs `rcx`, shadow space,
different callee-saved sets). Every address must be re-derived per binary;
only the struct-field subset is a candidate for carrying over, and that must be
proven, not assumed.

CK3 has no anti-cheat. What is actually verified is narrower: the Linux build
has run for months with the bridge preloaded under Steam and no integrity
check has fired. The Windows and macOS builds have not been inspected.

## Decision

1. Port the bridge as **one code base with a platform layer**, not as three
   forks. Engine logic (query/action handlers, hooks' business logic, IPC
   protocol, witnesses, drains) stays platform-neutral and calls into
   `bridge/src/os/`.
2. **Windows first, then macOS.** Windows is the largest CK3 population, has
   the simplest injection story (a proxy DLL), and forces the MSVC/RTTI/
   calling-convention work that macOS does not need. macOS reuses the Itanium
   ABI work from Linux and is gated on an unanswered code-signing question.
3. **One supported binary per platform, same refusal semantics.** The BuildID
   check becomes a per-format binary identity check with the same three
   outcomes (`match` / `mismatch` / `not_found`), the same witness, the same
   `handshake_ack` field and the same strict refusal in `bridge_init()`.
4. **Offsets live per platform** under `re/offsets/<platform>/`, selected at
   configure time by `-DCK3_PLATFORM=linux|windows|macos`. Nothing is shared by
   copy; the shared subset is proven by a validator and then *referenced*.
5. **x86-64 only** for the first Windows and macOS releases. arm64 (Apple
   Silicon native) is a separate, later decision because the decoder, the
   signatures and the hook engine are all x86-specific.
6. The 2026.10.0 Linux release is the behavioural reference. "Done" for a
   platform means the same save produces the same query results and the same
   steering effects as on Linux, measured, not asserted.

## Platform abstraction

Introduce `bridge/include/os/` (interfaces) and `bridge/src/os/{linux,windows,macos}/`
(implementations). One implementation directory is compiled per
`CK3_PLATFORM`; nothing in `bridge/src/*.cpp` may include a platform header
directly once the layer exists. The interface is deliberately small:

| Interface | Linux (today) | Windows | macOS |
|---|---|---|---|
| `os::entry` — library entry | `__attribute__((constructor))` → `dll_init()` | `DllMain(DLL_PROCESS_ATTACH)`; must only record the module handle and start one thread (loader lock: no `LoadLibrary`, no synchronous socket work in `DllMain`); that thread runs `bridge_init()`. The existing deferred-hook design (`is_game_ready()` polling, ck-crm5) already tolerates init happening after the loader returns | `__attribute__((constructor))` in a dylib, as Linux |
| `os::process_is_game()` | `/proc/self/exe` basename == `ck3` | `GetModuleFileNameW(NULL)` basename, case-insensitive, == `ck3.exe` | `_NSGetExecutablePath()` basename == `ck3` (and bundle path check, *verify* name) |
| `os::image_base()` + section table | `dl_iterate_phdr`; `PT_LOAD`/`PT_NOTE` | `GetModuleHandleW(NULL)`; walk `IMAGE_NT_HEADERS` sections (`.text`, `.rdata`, `.data`) | `_dyld_get_image_header(0)` + `_dyld_get_image_vmaddr_slide(0)`; `getsectiondata()` for `__TEXT,__text`, `__DATA_CONST,__const` etc. |
| `os::binary_identity()` | ELF `NT_GNU_BUILD_ID` (20 bytes) | PE CodeView `RSDS` record (PDB GUID + age) from the debug directory, **and** `TimeDateStamp` + `SizeOfImage` from the optional header as the fallback key if the debug directory is stripped (*verify* which `ck3.exe` has) | `LC_UUID` of the loaded slice (per-architecture in a fat binary) |
| `os::safe_read(dst, src, n)` | `process_vm_readv` on self | `ReadProcessMemory(GetCurrentProcess(), …)` — fails instead of faulting, same contract | `mach_vm_read_overwrite(mach_task_self(), …)` — returns `KERN_INVALID_ADDRESS` instead of faulting |
| `os::guarded_region` (direct derefs that may fault) | `sigsetjmp`/`siglongjmp` + `SIGSEGV` handler, re-armed after the engine installs its crash reporter | A vectored exception handler (`AddVectoredExceptionHandler`, first position) that checks the thread-local `g_in_protected_region` flag and longjmps; `__try/__except` is not usable from the generic code because the protected bodies hold C++ objects. The same "the engine's crash handler displaces ours" race exists (CK3's Windows crash reporter); re-arm the same way | `sigsetjmp` + handlers for `SIGSEGV` **and** `SIGBUS` (macOS raises `SIGBUS` for many unmapped accesses). Mach exception ports take precedence over signals; if CK3's crash reporter registers one, the bridge must register its own task-level port for `EXC_BAD_ACCESS` and forward. *verify* |
| `os::write_protected(addr, data, n)` | `/proc/self/mem` | `WriteProcessMemory(GetCurrentProcess(), …)` (handles protection itself) or `VirtualProtect` → write → restore; `FlushInstructionCache` after code writes | `mach_vm_protect(…, VM_PROT_COPY)` for `__TEXT` (plain `mprotect` fails on the shared-cache-style max protection); `mprotect` suffices for `__DATA_CONST`; `sys_icache_invalidate` after code writes |
| `os::alloc_exec_near(target, size)` | `mmap` RWX in ±2 GB | `VirtualAlloc` with `PAGE_EXECUTE_READWRITE` probing addresses near the target (`VirtualQuery` to skip reserved ranges) | `mmap` RWX; under the hardened runtime this needs `com.apple.security.cs.allow-unsigned-executable-memory` or `allow-jit` on the host process (see macOS plan). Split allocate-RW / flip-to-RX to stay W^X-friendly |
| `os::transport` (listen/accept/read/write) | `AF_UNIX` | TCP loopback (see IPC section) | `AF_UNIX` (socket path under `~/Library/Application Support/ck3agent/`; the 104-byte `sun_path` limit is tighter than Linux's 108) |
| `os::thread_id()`, thread naming | `SYS_gettid`, `pthread_setname_np` | `GetCurrentThreadId`, `SetThreadDescription` | `pthread_threadid_np`, `pthread_setname_np` (self only) |
| `os::watchdog` | `SIGALRM` | a timer thread + `QueueUserAPC`/flag polling (no `SIGALRM` on Windows) | `SIGALRM` as Linux |
| `os::paths` | `$XDG_RUNTIME_DIR/ck3agent/inst/<id>/…` | `%LOCALAPPDATA%\ck3agent\inst\<id>\…` | `~/Library/Application Support/ck3agent/inst/<id>/…`, logs in `~/Library/Logs/ck3agent/` |
| `os::early_init` priority | `CK3_EARLY_INIT` / `init_priority(101)` | `#pragma init_seg(lib)` on the TUs that carry witnesses; the `bridge/test/test_init_order.cpp` invariant needs a PE equivalent (walk `.CRT$XC*`) | as Linux (Clang) |
| `os::input` | XTest, `/dev/uinput` | `SendInput` | `CGEventPost` (needs Accessibility/TCC approval for the game process; *verify* whether posting to one's own process is exempt) |

What does **not** move into the layer: the NDJSON protocol, handshake, session
slots, the witness vocabulary (`match`/`mismatch`/`not_found`/`unchecked`),
drains, ledgers and pipeline modes. They are the contract the Python side
relies on and are platform-free already.

One piece of engine logic is ABI-sensitive and gets a per-platform
implementation: the RTTI walk in `runtime_resolver.cpp` (`find_typeinfo()` /
`find_vtable()`) assumes Itanium `std::type_info`. The MSVC version walks
`RTTICompleteObjectLocator` → `TypeDescriptor` (`.?AVCAIManager@@`) and finds
vtables by the locator pointer one slot before each. macOS keeps the Itanium walk.

## Windows plan

**Injection.** Three options were weighed:

(a) **Proxy DLL beside `ck3.exe`** (`version.dll`, `winmm.dll` or `dinput8.dll`,
whichever `ck3.exe` imports; *verify* from its import table). The loader
resolves an application-directory DLL before the system one for any import
that is not on the `KnownDLLs` list. The proxy forwards every export to the
real system DLL (`GetSystemDirectoryW` + `LoadLibraryW`) and runs the bridge
entry on attach. No launcher is needed, the Paradox launcher (`dowser.exe`)
becomes irrelevant (it only spawns `ck3.exe`, which loads the proxy on its
own), and launching from Steam works unchanged.

(b) **A launcher that does `CreateProcess(CREATE_SUSPENDED)` + remote-thread
`LoadLibraryW`.** Works, but it is the Windows analogue of `launch_ck3.sh`:
it bypasses Steam's own launch (which breaks Steam overlay/cloud saves for
some users) and needs the dowser bypass re-implemented — `dowser.exe` would
have to be either skipped (start `ck3.exe` directly) or hooked for
`CreateProcessW` to inject into its child.

(c) **Detours-style import hooking** is a hooking technique, not an injection
one; it still needs (a) or (b) to get into the process, and the inline
trampolines already cover what import hooking would.

**Decision: (a) ships; (b) is kept as a diagnostic** (`ck3agent launch
--inject`) for when a proxy load fails silently. `is_ck3_process()` stays even
with (a). Users run other tools that ship proxy DLLs (overlays, ReShade); the
installer must detect an existing `version.dll` beside the game and refuse
rather than chain.

**Build.** MSVC 2022, CMake, `/std:c++17`, `/permissive-`, `/EHsc`. GCC
attributes (`format`, `init_priority`, `constructor`) and `<sys/...>` headers
move behind `os/`. Zydis builds with MSVC from the same `FetchContent` block.
The DLL exports only the proxy forwards.

**Calling convention.** The function-pointer typedefs in `bridge/include/bridge.h`
(`fn_ai_char_toggle`, `fn_get_ai_controller`, …) carry no explicit convention
and follow the platform default, which is right on both ABIs. Hook replacements
for engine virtuals take `this` as the first parameter and must not assume
registers. `install_hook()`'s prologue relocation must learn MSVC prologues
(`mov [rsp+8], rcx` home-slot spills, shadow-space `sub rsp, N`).

**Signatures.** All 34 patterns in `re/offsets/signatures.h` are re-derived;
the format and `sig_scanner.cpp` stay. `resolve_rip_anchors()` carries over
because PE code on x86-64 is also RIP-relative.

**P2 cell status.** The platform layer (`bridge/src/os/windows/`) built and
tested with `clang-cl` + `xwin`, tests under Wine (MEDIUM until re-run on real
Windows). Each **done** cell has an engine-free `test_os_win_<cell>`; the
portable helpers it leans on (`pe_image`, `sha256`, `win_paths` rules) have a
`test_os_<helper>` that also runs on Linux and macOS. A **stub** is a typed
refusal in `windows/stubs.cpp` whose refusal `test_os_stubs` asserts, so it
cannot be mistaken for working code.

| `os::` cell | File | State | Test | Why, if not done |
|---|---|---|---|---|
| `process_identity` / `exe_path_is_game` | `process.cpp` | done | `test_os_win_process` | |
| `entry_state` (DllMain) | `entry.cpp`, `dll_main.cpp` | done (exe path); init thread untested | `test_os_win_entry` | the `attached` path needs the DLL in `ck3.exe` (P4) |
| `image_base` / `image_sections` | `image.cpp` | done | `test_os_win_image`, `test_os_pe_image` | |
| `binary_identity` | `identity.cpp` | done | `test_os_win_identity`(+`_nodebug`), `test_os_sha256` | `check` stays `unchecked` until `offset_version.h` carries PE constants (P1) |
| `safe_read` | `safe_read.cpp` | done | `test_os_win_safe_read` | |
| `write_protected` | `write_protected.cpp` | done | `test_os_win_write_protected` | |
| `thread_id` / `set_thread_name` | `threads.cpp` | done | `test_os_win_threads` | |
| `paths` | `paths.cpp` | done | `test_os_win_paths`, `test_os_win_paths_rules`, `test_os_win_paths_pin` | |
| `ipc_endpoint` / `transport_*` | `transport.cpp` | done (byte transport) | `test_os_win_transport` | the token + `endpoint.json` are P4 |
| `alloc_exec_near` / `free_exec` / `register_unwind` | `exec_alloc.cpp` | done | `test_os_win_exec_alloc` | |
| `guarded_call` / `guard_rearm` | `guarded.cpp` | done | `test_os_win_guarded` (faults deliberately) | |
| `watchdog_arm` / `watchdog_disarm` | `watchdog.cpp` | done | `test_os_win_watchdog` | |
| `input_click` | `stubs.cpp` | stub | `test_os_stubs` | `SendInput`; steering is phase P6 |
| `rtti_find_vtable` | `stubs.cpp` | stub | `test_os_stubs` | needs `ck3.exe` (MSVC RTTI, P1/P5) |
| `hook_plan` | `stubs.cpp` | stub | `test_os_stubs` | needs `ck3.exe` (per-hook CFG/CET, P6) |
| `exec_interposition` | `stubs.cpp` | n/a | `test_os_stubs` | `unavailable_on_platform`: a proxy DLL replaces it |

The `ck3_bridge_win` DLL target links the layer plus an inert `DllMain`; the
proxy export table is P4. The `windows-latest` CI leg builds all of it and runs
the tests (`continue-on-error` until green on real MSVC twice). macOS is all
typed refusals (`macos/stubs.cpp`), built on `macos-14`.

**Binary identity on the Python side.** `ck3agent/_elf.py` gains siblings
`_pe.py` (debug directory → `RSDS` GUID/age, optional-header
`TimeDateStamp`/`SizeOfImage`) and later `_macho.py` (`LC_UUID`, per slice),
stdlib only; `cmd_setup`/`cmd_doctor`/`cmd_launch` call a dispatcher instead
of `read_build_id` directly. `offset_version.h` already carries
`GAME_PLATFORM` with a `linux / windows` comment; it gets per-platform
identity constants (`PE_PDB_GUID`, `PE_PDB_AGE`, `PE_TIMESTAMP`,
`PE_SIZE_OF_IMAGE`; `MACHO_UUID`) alongside `BUILD_ID`.

**Status (2026-10-18).** Findings are in the research checkout
(`re/findings/windows/W0-binary-report.md`, `re_anchor_transfer.md`). The
binary read was 1.20.1.1, so its identity is recorded in the report only, not
in `offset_version.h`.

| Item | State |
|---|---|
| W0 (W1-W16) | Answered 2026-10-18, from a static read of the Windows depot. **The depot read is 1.20.1.1** (Steam public branch), not 1.18.3.1; answers that depend on the build (identity) are for 1.20.1.1 only |
| Kill trigger: anti-tamper | Not fired (MEDIUM, static only): standard MSVC sections, CRT-only TLS callbacks, no protector. Callers of the debugger/module-enumeration APIs not yet classified |
| Kill trigger: no 1.18.3.1 rollback branch | Not fired, provisional: a public `1.18.3` branch exists for the Windows binaries depot; that it serves 1.18.3.1 is MEDIUM until it is downloaded and its `rawVersion` read |
| Injection | `version.dll` and `winmm.dll` both imported; no CFG enforcement, not CET-compatible; exe Authenticode-signed by Paradox |
| P1 anchors | **0 transferred** on the pinned binary (not yet downloaded). Method check on 1.20.1.1: of the first 20 first-tier anchors, 15 found and 5 documented gaps (20 of 20 resolved); of 30, 22 found, 5 gaps, 3 open. Raw MSVC RTTI walk and string anchors both work |
| Gaps | MSVC inlined four small functions the bridge calls (`GET_PLAYER_CHAR_ID`, `IS_MANUALLY_PAUSED`, `AI_CHAR_TOGGLE`, `CHAR_GET_AI_CONTROLLER`): the bridge needs an inline-read recipe per anchor, whose field offsets P3 confirms; the CAIManager tick lives in the vtable of the `+8` subobject (no thunk), not the `+0` one |
| Next, in order | Download the `1.18.3` branch and re-run; `GAME_COMMAND_DISPATCH` and the command classes; `CMD_PAUSE_ON`; the 34 signatures re-derived in the Windows Ghidra project; the managers under `GAME_STATE_ROOT` |

P1's kill trigger (transfer below ~40% with no new method) is not yet
measurable: the method check ran on the wrong version, though every method
tried worked.

**Status under Proton (2026-10-19).** One Linux host, Proton
experimental-11.0, no Steam client installed. Proton evidence is MEDIUM (limits
above). Commands and log lines are in `docs/porting/proton-runbook.md` §3-§5.

| Item | Phase | Status | What was seen |
|---|---|---|---|
| Windows depot at the 1.18.3.1 pin | P0 | **not met** | The depot used was the public branch, 1.20.1.1. Nothing below is an offset result for the pin |
| `ck3.exe` reaches the main menu under Proton | P3 | verified | 85 s from `proton run`; DXVK, D3D11 feature level 11_0 on an NVIDIA GPU; the Paradox launcher is never started |
| Steam API check for a copy outside a Steam library | P3 | not a blocker without Steam | Proton's `steam.exe` asserts unless `~/.steam/sdk64/steamclient.so` exists (workaround: steamcmd's copy). Then the game's `SteamAPI_Init` fails and the game runs anyway. Not tested with the Steam client running |
| Proxy `version.dll` loads `native` and forwards | P4 (load half) | verified | `Loaded ...\binaries\VERSION.dll ... : native`; the stub resolved 16 of 16 exports from the builtin; the game called 3 of them through it and reached the menu |
| `match` / `REFUSED` on build identity, no listener otherwise | P4 | not started | Needs the bridge DLL (P2) |
| ELF core of the Wine process readable by the core reader | P3 | verified | `gcore` in 18 s; `MZ` at base `0x140000000` (the PE `ImageBase`; Wine kept it); `resolve(rva)` returns bytes equal to the file |
| MSVC container layouts in memory | P3 probe | `std::string` seen, others not measured | In the core, 54,753 records of the MSVC string shape (length at +16, capacity 15 at +24) against 921 of the libstdc++ shape in the same 256 MiB of heap; `std::map` needs an anchored object (P1) |
| Layout probe over the 1,757 field offsets | P3 | not started | Waits for the pinned depot and the P1 anchors |

## macOS plan

**First thing to verify: is CK3's main Mach-O signed with the hardened
runtime, and with which entitlements?** (`codesign -d --entitlements - -vv
<app>/Contents/MacOS/ck3`.) Nobody on the project has run this. Everything
below branches on the answer.

- If the binary is **not** hardened (or carries
  `com.apple.security.cs.allow-dyld-environment-variables`), then
  `DYLD_INSERT_LIBRARIES` works and the macOS bridge is the Linux bridge with
  the `os/macos` layer: `ck3agent launch` sets the variable the way
  `cmd_launch()` sets `LD_PRELOAD` today. dyld strips `DYLD_*` from the
  environment of restricted children; if the macOS launcher sits between the
  user and the game (*verify* whether the macOS Steam entry starts a launcher
  or `ck3` directly), the `posix_spawn` interposition in `launcher_bypass.cpp`
  ports directly (dyld interposing via `__attribute__((section("__DATA,__interpose")))`
  rather than `dlsym(RTLD_NEXT)`).
- If it **is** hardened without that entitlement, `DYLD_INSERT_LIBRARIES` is
  ignored. The plan is the common modding technique: **a relauncher that
  copies the app bundle** to `~/Library/Application Support/ck3agent/
  CK3-bridged.app` (copying `Contents/MacOS`, symlinking `Contents/Resources`
  so the copy is not 3 GB), adds the dylib via an `LC_LOAD_DYLIB` load
  command, and **re-signs the copy ad hoc** (`codesign -s -`) with
  entitlements allowing dyld variables and unsigned executable memory. Ad-hoc
  signatures run locally without notarization. *verify* that the copy still
  passes CK3's Steam DRM handshake.
- **`task_for_pid` injection is rejected.** It needs SIP off or a
  `com.apple.security.cs.debugger` entitlement on a notarized injector, and
  "disable SIP" is not a supported configuration.

**Architecture.** CK3 for macOS may ship as x86_64 only, arm64 only, or a
universal binary (*verify* with `lipo -archs`). Zydis decodes x86 only; the
signatures, the RIP-anchor logic and `install_hook()`'s relocation are x86
code. Therefore:

- First macOS target is **x86_64**, run under Rosetta 2 if the machine is
  Apple Silicon and the game offers an Intel slice. This shares the decoder,
  the Ghidra workflow and the signature tooling with Linux and Windows. Rosetta
  supports self-modifying code (it re-translates after writes), so inline hooks
  and vtable swaps work, at a translation cost on the first execution after a
  patch; `sys_icache_invalidate` is still called for hygiene.
- An **arm64** bridge is a separate milestone: Capstone (BSD) or a hand-rolled
  minimal AArch64 decoder for `install_hook()` relocation (ADRP/ADR/B/BL/LDR
  literal fixups), 4-byte-aligned signatures with a different mask format,
  `pthread_jit_write_protect_np` toggling for trampoline pages, and a full
  offset re-derivation from the arm64 slice. Apple Silicon runs x86_64 games
  acceptably; the arm64 port is a quality step, not a prerequisite.

**Clang ABI.** The macOS binary is Itanium ABI like the Linux one, so vtable
slot order, RTTI layout and the `find_typeinfo()` walk carry over with only
section-name changes. Struct field offsets are the same question as for
Windows and go through the validator; they are expected to agree with Linux
far more often than MSVC's do.

**Guarded reads.** `mach_vm_read_overwrite` on `mach_task_self()` is the
non-faulting read. The `sigsetjmp` handler must cover `SIGBUS` as well as
`SIGSEGV`; if CK3 registers a mach exception port (*verify*), the bridge sets
its own `EXC_BAD_ACCESS` port via `task_set_exception_ports` and forwards,
mirroring the Linux `g_prev_sigsegv_action` chain.

## Offsets and reverse engineering per platform

- **One Ghidra project per binary.** The existing project is the Linux ELF.
  Windows (`ck3.exe`, PE) and macOS (`ck3`, Mach-O, per slice) get their own
  projects; Ghidra imports all three formats and its auto-analysis recovers
  MSVC RTTI (`RTTICompleteObjectLocator`) and Itanium RTTI alike. Paradox
  ships no PDBs, so the Windows project is as symbol-free as the Linux one.
- **Same loop.** investigate → validate (two sources for MEDIUM, three or a
  decompile for HIGH) → record with confidence and evidence in the comment →
  implement → test against a live game. Findings go to
  `re/findings/<platform>/re_<system>.md` (research checkout; see amendments).
- **Layout.** `re/offsets/<platform>/{ck3_offsets.h, signatures.h,
  observer_data_functions.h, offset_version.h}`, with `re/offsets/linux/`
  created by moving the present files (CMake's include path follows
  `CK3_PLATFORM`). Within `ck3_offsets.h`, split the content that is a
  property of *declarations* (struct field offsets, enum values, fixed-point
  scales) from the content that is a property of a *binary* (function
  addresses, vtable addresses, globals, slot indices). The first group becomes
  `ck3_struct_offsets.h`, the second `ck3_code_offsets.h`; only the first is a
  cross-platform candidate.
- **Cross-platform offset validator.** Before re-deriving ~7,000 lines by
  hand, prove which struct offsets already hold. Build on
  `bridge/src/offset_validator.cpp` (`OffsetCheck`: `READABLE`, `PTR_NONNULL`,
  `PTR_MAPPED`, `BYTE_NONZERO`, `U32_RANGE`): add a *layout-probe* mode that
  runs with hooks off on a fixed reference save, walks each struct with the
  Linux field offsets and emits one row per field — `agreed`, `disagreed`
  (found vs expected) or `unwitnessed` (could not be evaluated) — as JSON.
  Known-value checks (the player's gold, birth date, the 1066 start date) make
  it discriminating rather than "the pointer looked mapped". Agreeing fields
  get the Linux evidence re-cited with a `cross-platform: agreed on <platform>
  <identity>` note; the rest are re-derived. An `agreed` row is one source,
  never HIGH on its own.
- **Signatures and RTTI** are never shared: the 34 Linux patterns are the
  list of what to re-derive, not reusable bytes. `re/rtti/trigger_subclasses.csv`
  (read at runtime by the named-trigger evaluator) is likewise per platform.

## IPC and the Python side

- **Linux and macOS:** `AF_UNIX`, unchanged. macOS uses the per-user
  Application Support directory so the path stays under the 104-byte limit
  and is not world-writable `/tmp`.
- **Windows:** TCP on `127.0.0.1`, OS-assigned port. The bridge writes
  `endpoint.json` (`{"port": N, "token": "<32 random bytes hex>", "pid": …}`)
  into the instance directory with a user-only ACL; a handshake without the
  token is refused with the existing typed `handshake_ack`. Over named pipes:
  the Python client (`BridgeClient` in `ck3agent/server/bridge_client.py`,
  `AsyncBridgeClient`) stays one `socket`/`asyncio` code path with only the
  endpoint resolver differing; pipes need the Proactor loop and another read
  model. Over `AF_UNIX` on Windows: the kernel has had it since Windows 10
  1803, but CPython's support is version-dependent; revisit when the minimum
  Python ships it. The token exists because a loopback port, unlike a socket
  file, is reachable by every local user.
- **Protocol:** unchanged, with two additions: `observed_build_id` becomes
  platform-tagged (`elf:<hex>`, `pe:<guid>-<age>`, `macho:<uuid>`) and the
  handshake gains `platform`. An old client comparing it to bare hex reads a
  mismatch on non-Linux, which is correct.
- **`instance.py`:** `resolve()` grows a platform branch for the instance
  root and endpoint; `socket_path()` keeps its meaning on POSIX and returns
  the `endpoint.json` path on Windows.
- **CLI:** `setup` finds the game per platform (`libraryfolders.vdf` on all
  three, the bundle path on macOS); `launch` is per platform (`LD_PRELOAD`;
  on Windows "start via Steam, the proxy loads itself";
  `DYLD_INSERT_LIBRARIES` or the relauncher); `doctor` checks the proxy's
  forwards, the bundle signature and the endpoint. `serve` and `run` are
  untouched. Process identification stays identity-based
  (`QueryFullProcessImageNameW`, `proc_pidpath`), never a name match.

## CI and packaging

- **Build matrix:** `ubuntu-22.04`, `ubuntu-latest` (as today),
  `windows-latest` (MSVC, `-DCK3_PLATFORM=windows`), `macos-14` (Clang,
  `-DCK3_PLATFORM=macos`, `-DCMAKE_OSX_ARCHITECTURES=x86_64`), all with
  `-DCK3_STRICT_BUILD_ID=ON -DBUILD_BRIDGE_TESTS=ON`. No game in CI anywhere.
  The engine-free native tests (`test_json_helpers`, `test_script_block_parser`,
  `test_instance_paths`, `test_game_date`, `test_war_history`) must build and
  pass on all three, so they cannot include `os/linux` headers — a forcing
  function for the layer. Add `test_binary_identity` over checked-in minimal
  ELF / PE / Mach-O fixtures asserting the three outcomes.
- **Python:** the `python` and `wheel + sdist` jobs run on all three OSes;
  `_pe.py` / `_macho.py` get fixture tests beside `_elf.py`'s.
- **Windows packaging:** a zip (the DLL under its proxy name, `VERSION`, the
  trigger CSV, `install.ps1`); Inno Setup later if wanted. **Prerequisite, not
  assumption:** an Authenticode certificate. An unsigned DLL beside a Steam
  game is what SmartScreen and AV engines flag. Decide OV/EV purchase versus
  documented-unsigned before W5 closes and say which in the release notes.
- **macOS packaging:** a `.dmg` or `.pkg` with the dylib, the relauncher and
  the wheel. **Prerequisite, not assumption:** a paid Apple Developer account
  for Developer ID signing and notarization of both; without it Gatekeeper
  quarantines the dylib and a hardened process will not load it. The user's
  game copy is ad-hoc signed locally and never distributed.
- **Release pipeline:** `release/export.py` and `manifest.toml` list the
  per-platform offset directories and overlay files; `release.yml` gains
  per-OS build jobs attaching to one tag. One tag, three archives, one
  `VERSION`. `installer/install.sh` keeps its Linux-only `uname` gate; the
  other platforms get their own installers.

## Milestones (superseded by the phased plan in "Review amendments")

Effort is for one experienced contributor with the Linux bridge as reference
and Ghidra familiarity; ranges are wide because the RE is the uncertain part.

| # | Milestone | Done when | Effort |
|---|---|---|---|
| W0 | Windows verification spike | Written answers to: which DLLs `ck3.exe` imports (proxy candidate), whether its debug directory carries an `RSDS` record, how the Steam entry launches (`dowser.exe` → `ck3.exe`?), whether any integrity check or anti-tamper section exists, Ghidra import of the PE completes. No code | 1 wk |
| W1 | Platform layer + Windows build | `bridge/src/os/` exists, Linux build is behaviourally unchanged (the 2026.10 Linux acceptance suite is green), `windows-latest` builds the DLL and the engine-free tests pass | 2–3 wks |
| W2 | Loads and refuses | The proxy DLL loads in `ck3.exe`, `is_ck3_process()` is true only there, identity is read, the bridge log shows `REFUSED` on a wrong build and `match` on the target, and a wrong build leaves no listener. `ck3agent doctor` reports the chain on Windows | 1–2 wks |
| W3 | Read-only observe | Globals (`GAME_STATE_ROOT`, `AI_ENABLED_FLAG`, the manager singletons) and enough struct offsets for the `query_*` handlers; the layout-probe report exists; on the reference save every `query_*` tool returns values equal to the Linux bridge's (a scripted diff, not a glance) | 3–5 wks |
| W4 | Hooks and steering | `install_main_tick_hooks()` on the MSVC vtable, the inline hooks, the personality writes; the live scenarios (research checkout `bridge/harness/`, not published) pass on Windows with the same outcomes as Linux | 3–5 wks |
| W5 | Full tool surface, installer, CI | Every registered MCP tool behaves as on Linux; signed (or documented-unsigned) zip installer; `release.yml` publishes the Windows archive; `ck3agent launch/doctor` work for a Steam install | 2–4 wks |
| M0 | macOS verification spike | Hardened-runtime and entitlement answer, `lipo -archs`, launcher chain, crash-reporter/mach-port behaviour, Ghidra import | 1 wk |
| M1 | Injection path | Either `DYLD_INSERT_LIBRARIES` or the relauncher copies, re-signs and starts the game with the dylib loaded; `LC_UUID` identity read; refusal path works | 1–3 wks |
| M2 | Observe | As W3; expected to go faster because the Itanium RTTI walk and the vtable order carry over | 2–4 wks |
| M3 | Hooks, steering, tool surface | As W4 + W5; `.dmg`, Developer ID signing and notarization in `release.yml` | 3–5 wks |
| A1 | arm64 native (optional, later) | Decoder swap, arm64 signatures and offsets, JIT write-protect handling; a separate ADR when it starts | not estimated |

Windows total: roughly 12–20 weeks. macOS (x86_64) after Windows: roughly 7–13
weeks. These are ranges for elapsed engineering time, not calendar promises.

## Risks and open questions

- **EULA and anti-tamper.** Verified: no anti-cheat on Linux, months of
  preload operation. Not verified: the Windows/macOS binaries (W0/M0 inspect
  them). Nobody qualified has reviewed the Paradox EULA's position on in-memory
  modification of a single-player game; the README should say the tool is for
  research, modifies the running game in memory, is unsupported by Paradox and
  must not be used in multiplayer — on Linux too.
- **Game patches invalidate offsets.** Already true on Linux; three platforms
  triple the surface and patches land on all three the same day. Mitigation:
  the strict refusal (a wrong build never hooks), the layout probe as the first
  step of every patch-adapt, and high signature coverage so
  `resolve_signatures()` recovers most function addresses without a Ghidra
  session. A release current on one platform and stale on another must say so
  in `ck3agent version`.
- **SIP, hardened runtime, notarization.** The whole macOS plan hinges on M0.
  If the binary is hardened and the relauncher approach fails Steam's DRM
  check on a re-signed copy, macOS support may be limited to users who buy
  the game outside Steam, or may not be feasible; that is a legitimate outcome
  of M0 and should be recorded as such rather than worked around with SIP-off
  instructions.
- **MSVC vtable layout.** Slot indices (`g_vtable_tick_slot`, the `send_cmd`
  vtables from `discover_send_cmd_vtable()`) are re-derived, never
  translated. A carried-over slot index is the likeliest way to crash the
  game at tick time, so before any swap the probe must confirm the slot
  points into `.text` and its prologue matches the expected function.
- **Signature scanning across compilers.** 0 of 34 patterns are expected to
  match as-is; the subtler risk is a pattern matching *something* wrong.
  `cross_validate()` in `runtime_resolver.cpp` already demotes a hit that
  disagrees with the hardcoded offset; keep that gate on.
- **Rosetta and self-modifying code.** Works in principle; the cost of a
  trampoline install under translation is unmeasured. Measure in M2.
- **Loader lock and init order on Windows.** Blocking work in `DllMain`
  deadlocks the game at start; the design keeps it to a thread spawn. The
  init-order bug class documented in `init_order.h` (a later static
  constructor erasing a recorded witness, which `CK3_EARLY_INIT` prevents on
  Linux) has an MSVC analogue; `init_seg` is the tool and
  `bridge/test/test_init_order.cpp` needs a PE reader.
- **Local-port exposure on Windows.** The token stops other local users, not a
  process running as the same user. That is weaker than the Linux
  default (a 0600 socket plus a peer-uid check) and matches only the legacy
  0666 layout; acceptable for a research tool, but it must be stated.
- Open: does CK3 on macOS start a launcher process at all? What minimum
  Windows/macOS versions does the game require (they set ours)? Does the
  Windows `ck3.exe` have a stripped `.reloc` (affects the Ghidra image base)?

## Alternatives considered

- **Do not port; document WSL2 instead.** Windows users can run the Linux
  CK3 build inside WSL2 with GPU passthrough at zero RE cost. Worth a README
  note as a stopgap, but not the decision: it excludes macOS, the WSL graphics
  path is fragile, and it asks most players to run a second OS.
- **Out-of-process reader only** (`ReadProcessMemory` / `task_for_pid`, no
  injection). Reads only; the project's value is steering through hooks on the
  AI tick. Rejected, though a read-only mode is a useful intermediate (W3/M2)
  and could ship as "observe-only" if a platform's hooks slip.
- **Three forks of the bridge.** Faster to first light, slower forever; every
  handler fix lands three times and drifts. Rejected.
- **Frida (frida-gum) as the hook and injection engine.** Cross-platform and
  mature; would replace `hooks.cpp`, `launcher_bypass.cpp` and much of `os/`.
  Rejected for the shipped bridge: a large dependency with its own runtime
  inside the game process, a different crash-safety model from the guarded-read
  discipline, and a notarization surface the project does not control. Kept as
  a development-time tool for the verification spikes.
- **Deriving offsets from PDB / dSYM.** Would settle the struct-offset
  question. Paradox ships neither.
- **The game's modding API instead of hooking.** Clausewitz script has no
  outbound IPC, no arbitrary state reads and no way to intercept the AI's
  decisions; the project exists because that is true. Rejected.
- **TCP loopback on every platform.** One transport, but it would regress
  Linux and macOS from filesystem-permissioned sockets to a token scheme, and
  the Linux socket path is part of the published instance contract. Rejected;
  the transport interface is small enough to carry two implementations.
