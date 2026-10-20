# re/

The reverse-engineering contract between the game binary and the bridge, plus
the method and tools that derived it. The Linux results are in `offsets/`. The
rest is how to re-derive them on a binary we have not mapped yet (a Windows
`ck3.exe`, a Mach-O slice, a patched Linux build). It does not include our
per-system findings (wars, councils and so on), Ghidra projects or decompiler
dumps. See ADR 0001: the 34 Linux signatures are the list of what to re-derive,
not bytes to reuse. The trigger catalogue (`re/rtti/trigger_subclasses.csv`
upstream) ships as `ck3agent/data/trigger_subclasses.csv`; it is per platform too.

| Path | What |
|------|------|
| `offsets/ck3_offsets.h` | struct offsets and function addresses, each with confidence and evidence |
| `offsets/signatures.h` | 34 prologue byte patterns resolved at load; its header gives the wildcard rules |
| `offsets/observer_data_functions.h` | GUI data-function callbacks for observer actions |
| `offsets/offset_version.h` | the one supported BuildID; the bridge refuses any other |
| `offsets/windows/ck3_offsets.generated.h` | GENERATED at export: the Windows build's offsets under the Linux names; its header lists the input shas and the pinned ck3.exe |
| `offsets/windows/ck3_offsets_win.h` | GENERATED at export: the Windows-only names (`ck3_win::`), values without the research annotations |
| `rtti/rtti_scanner.py` | ELF Itanium RTTI scan: typeinfo, vtables, class names to CSV |
| `scripts/ghidra/start.sh` | starts `ghserver.py` detached; configuration in its header |
| `scripts/ghidra/ghserver.py` | resident read-only Ghidra query server (pyghidra), one per project |
| `scripts/ghidra/gh.py` | its client: `func_at`, `decompile`, `xrefs`, `callees`, `findstr`, `findptr`, `read` |
| `scripts/utility/ExportStringXrefs.java` | every defined string with its xrefs and containing function, as CSV |
| `scripts/utility/AutoGenerateSignatures.java` | prologue signature, wildcards and uniqueness count per function |
| `scripts/utility/ImportRTTILabels.java` | labels Itanium vtables in Ghidra from typeinfo |
| `scripts/utility/CountRecoveredClasses.java` | counts recovered classes, vtables and named functions |
| `scripts/utility/find_struct_field_access.py` | every `[reg+off]` access to a field offset, not stack |
| `scripts/utility/verify_offset_header_convention.py` | checks each header address is base-relative, not a Ghidra VA |
| `findings/auto_signatures_summary.txt` | a generator run on Linux: UNIQUE, AMBIGUOUS(n), extended, SKIP |
| `findings/re_offset_header_convention_sweep.md` | the audit that fixed 189 Ghidra-VA leaks, and its method |

## Anchor A: string, then function, then signature

1. Export strings: `ExportStringXrefs.java`. Pick a string unique to the function
   (an assert source path, a console command, a script keyword such as `add_gold`).
2. The xref gives the function; confirm it by decompiling (`gh.py decompile`).
3. Signature: list `NAME,0xOFFSET` rows in a CSV and run
   `AutoGenerateSignatures.java <out> <csv>`. Keep only `UNIQUE` (1 match in
   `.text`). Do not ship `AMBIGUOUS(n)`; lengthen the pattern or anchor elsewhere.
4. Record the pattern in `signatures.h` and the offset in `ck3_offsets.h`.
   `bridge/src/sig_scanner.cpp` resolves it at load and compares.

## Anchor B: RTTI, then vtable, then slot

1. `rtti_scanner.py <binary>` (ELF) or Ghidra's own RTTI analysis (MSVC
   `RTTICompleteObjectLocator`, Mach-O). `ImportRTTILabels.java` labels ELF
   vtables in Ghidra; `CountRecoveredClasses.java` says how many it got.
2. By hand: `findstr` the mangled name (`10CAIManager`); `findptr` to it
   finds typeinfo+8; `findptr` to the typeinfo finds vtable+8; `read` lists slots.
3. A slot index is a fact about one compiler's vtable layout. MSVC can reorder
   overloads, so re-derive every index from a decompiled call site.
4. Record vtable and slot in `ck3_offsets.h`; `runtime_resolver.cpp`
   (`find_typeinfo`, `find_vtable`) can also resolve a class by name at load.

## Struct fields

Read accessor bodies (`mov rax,[rdi+0x1a8]` in `signatures.h` comments), then
`find_struct_field_access.py <off>` for every other access. Field offsets may
carry across platforms: a claim to prove, not to assume.

## Confidence

HIGH: 3+ independent sources, or decompile-verified. MEDIUM: 2 sources, or
structural inference. LOW: one unvalidated source. Never overwrite HIGH with a
guess, and when a value was wrong, record why beside the fix. Offsets are
base-relative (Linux `ghidra_va = offset + 0x100000`; PE `RVA = VA - ImageBase`).

## Running the tools

* Ghidra scripts: `analyzeHeadless <dir> <project> -process <program> -noanalysis
  -scriptPath re/scripts/utility -postScript <Script> [args]`. The output dir is
  the first argument, else `$CK3_RE_OUT`, else the working directory.
* Query server: set `GHIDRA_INSTALL_DIR` and `CK3_GHIDRA_PROJECT_DIR`, run
  `re/scripts/ghidra/start.sh`, poll `python3 re/scripts/ghidra/gh.py '{"cmd":"ping"}'`.
  One server per project; nothing else may open that project while it is up.
* After editing the header: `verify_offset_header_convention.py`, with
  `--image-base` and `--code-max` from your binary. It exits 1 on a suspect.

Results go under `re/offsets/<platform>/` (ADR 0001, decision 4), and each
entry names its platform and BuildID (PE: TimeDateStamp). PRs: CONTRIBUTING
section 6; no binaries, Ghidra projects or bulk output.
