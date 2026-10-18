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

The platform layer is in the tree as a skeleton. `bridge/include/os/os.h`
declares the interfaces from the ADR's "Platform abstraction" table (process
identity, image base, binary identity, guarded read and write, exec
interposition, IPC transport, paths, input, threads), each returning a typed
`Result`. `bridge/src/os/linux/` forwards to the existing Linux code;
`bridge/src/os/windows/stubs.cpp` and `bridge/src/os/macos/stubs.cpp` return
`not_implemented` or `unavailable_on_platform` and name the phase or the
replacement. Configure with `-DCK3_PLATFORM=windows` or `-DCK3_PLATFORM=macos`
to build only the layer and its stub test (`test_os_stubs`); CI does this on
`windows-latest` and `macos-14`. Replacing a stub with a real implementation is
a port task. The Linux bridge does not call the layer yet, so its behaviour
cannot change until a call site is moved onto it.

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
