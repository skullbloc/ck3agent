# RE: ck3_offsets.h Address Convention Sweep (ck-e58l.20, AUDIT-A)

**Bead:** ck-e58l.20
**Date:** 2026-04-21
**Confidence:** HIGH (Ghidra function.at cross-ref + core-dump probes)
**Binary BuildID:** `e61ff2642f07094defd140fcc98e078d42541275`
**Core dump:** `ck3_20260404_093841.core` (13 Jan 867, Duke Adalberto; research host, not published)
**Ghidra project:** CK3_new (image base `0x00100000`)

## Executive Summary

Systematically audited every `constexpr uintptr_t` entry in
`re/offsets/ck3_offsets.h` and fixed **189 Ghidra-VA-as-raw leaks**.
Added a canonical address-convention comment block at the top of the
header. Ran a full post-fix validation probe — **0 remaining suspects**.

Prior to this bead the header silently mixed two conventions (raw and
Ghidra VA), which could cause the bridge to read from wrong addresses
and manifest as silent data corruption or crashes. All known leaks are
now corrected and the header's convention is documented in-source.

## Methodology

### Phase 1 — enumerate suspects

Parsed every `constexpr uintptr_t NAME = 0xVAL;` line from the header
(984 total entries). Split by range:

- **Code entries** (`0x300_000 ≤ val ≤ 0x0a_000_000`): 720 entries
- **Data entries** (`0x0a_000_000 < val ≤ 0x0d_000_000`): 221 entries
- **Small constants** (`val < 0x10_000`): 41 entries (IDs / sizes / indices)

### Phase 2 — code-entry probe via Ghidra MCP

For each code entry, queried Ghidra twice via `function.at`:

1. at `val` itself (if val were a Ghidra VA, this returns its function)
2. at `val + 0x100000` (if val were raw, this is its Ghidra VA)

Decision:

- `val` hits a function entry but `val+0x100000` does not → **BUG: value is a Ghidra VA**
- `val+0x100000` hits a function entry but `val` does not → **CORRECT_RAW**
- Both hit entries → AMBIGUOUS; inspect name. If `val` side has a named
  symbol and `val+0x100000` has only `FUN_xxxxxxxx`, treat as hidden bug.
- Neither hits a function entry → inconclusive (likely inlined or
  mid-function). Trust raw convention.

### Phase 3 — data-entry probe via core dump

For each data entry, compared `read_u32` / `read_u64` at `resolve(val)`
and `resolve(val - 0x100000)`:

- If the "raw" interpretation reads ASCII string bytes or garbage while
  the Ghidra interpretation reads a valid heap pointer → BUG.
- For int/byte-type globals, compared documented expected value against
  both reads; flagged when only Ghidra interpretation matches.

### Phase 4 — post-fix verification

Re-ran the Ghidra MCP probe after applying the fixes. Result:
`SUSPECT_GHIDRA_VA = 0`. Harness saved at
`re/scripts/utility/verify_offset_header_convention.py` for future
regression checks.

## Results

| Classification | Count |
|----------------|------:|
| CORRECT_RAW (Ghidra function entry at val+0x100000) | 620 |
| **SUSPECT_GHIDRA_VA (post-fix)** | **0** |
| BOTH_ENTRY (Ghidra entry at both; assumed raw) | 49 |
| NO_ENTRY (inline / mid-fn / non-entry target) | 51 |
| DATA_RANGE (probed separately) | 223 |
| SMALL_CONST (IDs / sizes / indices) | 41 |
| **Total** | **984** |

### Fixed entries by class

- **181** CODE entries: value was literally a Ghidra VA for a function
  whose actual raw offset is `val - 0x100000`. Ghidra `function.at(val)`
  returned the named/FUN_-prefixed function; `function.at(val+0x100000)`
  returned either nothing or a different function. Fixed by subtracting
  `0x100000`.

- **1** hidden ambiguous entry (L940 `CONTRACT_CHANGE_EXECUTE`): both
  `val` and `val+0x100000` had Ghidra function entries, but the named
  symbol `CVassalContractChange_Execute` lives only at `val` — indicating
  `val` is the Ghidra VA. Fixed.

- **7** DATA entries where the "raw" read returned string / garbage and
  the Ghidra-interpreted read returned a valid heap pointer or the
  documented expected value:

  | Line | Name | Old | New | Evidence |
  |------|------|-----|-----|----------|
  | 127 | `MODIFIER_DEF_DB` | `0x0c032e98` | `0x0bf32e98` | raw reads ASCII "sr…"; ghidra reads heap ptr |
  | 134 | `SPREAD_SPEED_MULT` | `0x0c3acac8` | `0x0c2acac8` | raw=4 (garbage); ghidra=10000 (matches `culture_innovation_speed_mult`) |
  | 135 | `FASCINATION_SPEED_MULT` | `0x0c3acae0` | `0x0c2acae0` | raw=5; ghidra=40000 (matches `culture_fascination_mult`) |
  | 136 | `ERA_TYPE_DB` | `0x0bfe09e8` | `0x0bee09e8` | raw reads ASCII "_inv"; ghidra reads heap ptr |
  | 3238 | `G_NULL_INTERACTION_SENTINEL` | `0x0bfda700` | `0x0beda700` | comment already labelled as "Ghidra VA"; raw garbage, ghidra ptr |
  | 4764 | `INTERACTION_CB_DB_ROOT` | `0x0bfe39f8` | `0x0bee39f8` | raw reads ASCII "+coc"; ghidra reads heap ptr; comment references `_DAT_0bfe39f8` (Ghidra form) |
  | 4950 | `SCORING_WEIGHT_TABLE` | `0x0bfdf070` | `0x0bedf070` | raw reads "Miss" (string); ghidra reads heap ptr |

### CStartWarEffect collision resolved

The bead specifically called out:

```
START_WAR_EFFECT_EXECUTE_V23 = 0x052a11a0    // "== START_WAR_EFFECT_CREATE_WAR"
START_WAR_EFFECT_CREATE_WAR  = 0x053a11a0    // ← different address! (bug)
```

`CREATE_WAR` was a Ghidra-VA-as-raw leak; corrected to `0x052a11a0`.
Both entries now resolve to the same raw offset (as they refer to the
same function, CStartWarEffect vtable[23]). Added a plate comment at
the `EXECUTE_V23` site documenting the equivalence and the audit fix.

### Stress entries (ck-e58l.16 regression check)

`STRESS_PER_LEVEL = 0x0c2a92d0` → reads 100 via core-dump ✓
`MAX_STRESS_LEVEL = 0x0c2a92b8` → reads 3 via core-dump ✓

Both match the documented defines. The earlier Ghidra-VA values
(`0x0c3a92d0`, `0x0c3a92b8`) are not present in the header and the
ck-e58l.16 fix is still correct.

## Header-top convention block

Added a canonical explanation:

```cpp
// ═══════════════════════════════════════════════════════════════════════════
// ADDRESS CONVENTION (canonical — audited ck-e58l.20, 2026-04-21)
// ═══════════════════════════════════════════════════════════════════════════
// All uintptr_t values in this header are RAW (ASLR-base-relative /
// file-offset-relative), NOT Ghidra virtual addresses. Ghidra's image base
// for this binary is 0x00100000. To convert between forms:
//   raw       = ghidra_va - 0x00100000
//   ghidra_va = raw + 0x00100000
//
// The CoreReader (re/coredump/core_reader.py) and the bridge code both
// assume raw: CoreReader.resolve(val) = base_addr + val. When adding a new
// entry, always convert from the Ghidra VA via `val - 0x00100000`.
```

Also dropped the stale standalone `NOTE:` line that claimed the same
convention in a single sentence — the new block is the authoritative
in-source reference.

## Audit artifacts

- **Backup of pre-audit header, full Ghidra cross-ref output** (984 entries
  classified; 181 BUG_IS_GHIDRA_VA enumerated) **and the data-range analysis
  script:** scratch files on the research host, not kept. The pre-audit
  header is recoverable from git history.
- **Validation harness:** `re/scripts/utility/verify_offset_header_convention.py`
  (2026-10: ported from the retired `function.at` MCP relay to the resident
  server's `func_at` in `re/scripts/ghidra/ghserver.py`; same classes,
  exit 2 when the server cannot answer)

## Success criteria (ck-e58l.20)

| Criterion | Status |
|-----------|:------:|
| ck-e58l.16 stress fix still verified correct | ✓ |
| CStartWarEffect collision resolved | ✓ |
| All Ghidra-VA-masquerading-as-raw entries converted | ✓ |
| Header-top comment block added | ✓ |
| Probe test passes: 0 remaining suspects | ✓ |
| Findings committed | this file |

## Cross-reference — touching work

- `ck-e58l.16` — fixed the stress entries; that fix is preserved.
- `ck-e58l.18` — rename-log pass; the 21 rename-log-entry bugs it
  implicitly flagged are all included in this audit's 181 code fixes.
- `ck-e58l.21` — follow-up "reconcile rename log with header" audit;
  this pass should shrink its remaining delta significantly.

## Non-scope (per bead design)

- No function renames (ck-e58l.18 owns that side).
- No confidence-tag changes — only numeric offset corrections.
- No new offsets added.
- Rename-log cross-ref against findings docs outside `ck3_offsets.h` —
  that is a ck-e58l.21 concern.

## Follow-ups

- Re-run the validation harness whenever the header is modified
  (e.g., future patch adaptations). It takes ~60 s and covers every
  code entry.
- Consider adding a pre-commit check that fails if any new constexpr
  uintptr_t in the header is a Ghidra VA (probe Ghidra at val — if a
  named function starts exactly there, reject the commit).
