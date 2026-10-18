# Port questions (W0 and M0)

Each answer fills one field. "Tool" means a key in `tools/inspect_binary.py --json`;
"Manual" means the tool cannot see it. Answer in a `binary-report` issue.
`offset_version.h` constants are the per-platform siblings of the Linux `BUILD_ID`.

## Windows (W0)

| # | Question | Source | Fills |
|---|---|---|---|
| W1 | Which DLLs does `ck3.exe` import? Is `version.dll`, `winmm.dll` or `dinput8.dll` among them? | Tool: `imports`, `proxy_candidates` | proxy DLL name (ADR "Injection") |
| W2 | Does the debug directory carry an `RSDS` record? GUID, age, PDB name? | Tool: `codeview` | `PE_PDB_GUID`, `PE_PDB_AGE` |
| W3 | Is `TimeDateStamp` a clock time or a `/Brepro` hash? | Tool: `time_date_stamp`, `reproducible_build` | whether `PE_TIMESTAMP` can be a fallback key |
| W4 | `SizeOfImage` and `ImageBase`? | Tool: `size_of_image`, `image_base` | `PE_SIZE_OF_IMAGE`, `IMAGE_BASE` |
| W5 | SHA-256 of `ck3.exe` at 1.18.3.1? | Tool: `sha256` | `IMAGE_SHA256` |
| W6 | ASLR, high-entropy, NX, CFG, CET flags? | Tool: `flags` | hook plan, per-hook CFG/CET questions |
| W7 | Are relocations stripped? | Tool: `flags.relocs_stripped` | Ghidra image base, load-base handling |
| W8 | Any TLS callbacks? | Tool: `tls_callbacks` | init order, anti-tamper suspicion |
| W9 | MSVC RTTI present? How many `.?AV` names? | Tool: `msvc_rtti_strings` | feasibility of the P1 RTTI walker |
| W10 | Size of `.pdata`? | Tool: `pdata_size` | unwind coverage for trampolines |
| W11 | Is `ck3.exe` Authenticode-signed, and by whom? | Tool: `authenticode_present`; signer is Manual | signing decision (ADR "Signing") |
| W12 | Any unusual sections or modules (packers, DRM, integrity checks)? | Tool: `sections`, `imports`; rest Manual | anti-tamper kill trigger |
| W13 | What does the Steam entry start? (`ck3.exe` directly, or a launcher first?) | Manual: process tree | launch-chain notes |
| W14 | Is there a Steam Betas branch that serves 1.18.3.1 on Windows? Its name? | Manual: Steam, or `app_info_print` | rollback branch name (kill trigger) |
| W15 | Does the Ghidra PE import and auto-analysis finish? Time, function count? | Manual | P1 readiness |
| W16 | Minimum Windows version the game requires? | Manual: store page | installer requirements |

## macOS (M0)

Run the tool on the Mach-O inside the app bundle (`.../MacOS/ck3`). It reads the
signature itself, so Linux is enough.

| # | Question | Source | Fills |
|---|---|---|---|
| M1 | Which architectures? Intel only, arm64, or both? | Tool: `slices[].arch` | first macOS target |
| M2 | `LC_UUID` per slice? | Tool: `slices[].uuid` | `MACHO_UUID` |
| M3 | SHA-256 of the file? | Tool: `sha256` | `IMAGE_SHA256` |
| M4 | Hardened runtime on (`CS_RUNTIME`)? Library validation? | Tool: `slices[].code_signature` | injection path (ADR "macOS plan") |
| M5 | Which entitlements? Dyld variables, disable library validation, unsigned memory, JIT? | Tool: `slices[].code_signature.entitlements`, `.entitlement_hints` | injection path, hook memory plan |
| M6 | Is there a `__RESTRICT` segment? | Tool: `slices[].restrict_segment` | `DYLD_INSERT_LIBRARIES` viability |
| M7 | Is the binary encrypted (`cryptid`)? | Tool: `slices[].encrypted` | static analysis viability |
| M8 | Does Steam start `ck3` directly or through a launcher? With which environment? | Manual: process tree | launcher-bypass design |
| M9 | Does the game install a crash reporter or Mach exception ports? | Manual | guarded-read design |
| M10 | Is there a Steam Betas branch for 1.18.3.1 on macOS? Minimum macOS version? | Manual | rollback branch, requirements |
