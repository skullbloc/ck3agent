#!/usr/bin/env bash
# ck-duv5 acceptance. Host-only: no game, no bridge, no Ghidra.
#
# Exit: 0 pass / 1 FAIL / 3 NOT EVALUABLE.
#
# TWO SECTIONS, BECAUSE THE DELIVERABLE HAS TWO CLAIMS AND THEY ARE ESTABLISHED
# DIFFERENTLY:
#   §A/§B  the probe binary  — DRIVEN. The ucontext technique works on this
#          platform, and the shared formatter refuses rather than printing a
#          plausible 0x0.
#   §C     the call sites    — READ. bridge_init.cpp cannot link without the
#          engine, so that the handler USES the formatter on all three log
#          paths is established by reading the source, not by running it. Said
#          out loud because a reader would otherwise assume §A covered it.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 3

SRC=bridge/test/fault_location_probe.cpp
HDR=bridge/include/fault_site.h
HND=bridge/src/bridge_init.cpp
REG=bridge/include/thread_registry.h
for f in "$SRC" "$HDR" "$HND" "$REG"; do
    [ -f "$f" ] || { echo "NOT EVALUABLE: missing $f"; exit 3; }
done
command -v g++ >/dev/null || { echo "NOT EVALUABLE: no g++"; exit 3; }

BIN=$(mktemp -d) || exit 3
trap 'rm -rf -- "${BIN:?}"' EXIT

echo "── §A/§B: the probe ──────────────────────────────────────────────"
if ! g++ -std=c++17 -O1 -Wall -pthread -o "$BIN/probe" "$SRC"; then
    echo "NOT EVALUABLE: probe did not build"; exit 3
fi
"$BIN/probe"; probe_st=$?

echo
echo "── §C: the handler's call sites (a READ) ─────────────────────────"
fails=0
# Every fatal log path must go through the one formatter. A path that formats
# its own rip is a path that can drift back to printing 0x0 for "unknown".
sites=$(grep -c 'fmt_rip(ripbuf, sizeof ripbuf)' "$HND")
# ck-65lv: the register dump is the formatter that ALREADY REGRESSED — the
# recovered path had rip and thread and no registers, and that is the path that
# fired five times live. fmt_rip and fmt_thread each had a 3/3 recurrence row
# and this one had none, so the fix was guarded and the CLASS was not.
#
# Named so it cannot be mistaken for a thread-registry row: §C already prints
# registers_GameThread, registers_IpcReader and three more, every one about the
# thread REGISTRY rather than the register DUMP. ck-t5bc nearly read §C as
# covered because of that collision.
rsites=$(grep -c 'format_fault_regs(regbuf, sizeof regbuf' "$HND")
# ck-65lv ladder: the same 3/3 rule for the stack ladder — a fatal path that
# prints registers and the one word but not the ladder is the dare's gap kept.
lsites=$(grep -c 'format_stack_ladder(ladbuf, sizeof ladbuf' "$HND")
if [ "$lsites" -eq 3 ]; then
    echo -e "handler_log_sites_print_the_STACK_LADDER\t1\t($lsites/3)"
else
    echo -e "handler_log_sites_print_the_STACK_LADDER\t0\t($lsites/3 — a fatal path without the ladder cannot name the caller's caller)"; fails=$((fails+1))
fi
if [ "$rsites" -eq 3 ]; then
    echo -e "handler_log_sites_dump_the_REGISTER_FILE\t1\t($rsites/3)"
else
    echo -e "handler_log_sites_dump_the_REGISTER_FILE\t0\t($rsites/3)"; fails=1
fi
# and the frame word, on the recovered path that needs it
# ck-65lv: 3/3 like its neighbours, not >=1. A capability on one of three paths
# is the asymmetry this file's rip hoist exists to end, and a >=1 row cannot see
# a path that lacks it.
# ck-65lv: and the handler must pass a REAL rsp. A mutant that sets fault_rsp
# to 0 leaves every row above green — the probe drives format_frame_word with
# synthetic values and cannot see the handler's extraction — while every live
# capture would print "[rsp]=UNREADABLE (no rsp or no probe)" and the instrument
# would deliver nothing. Capability present, wiring dead: the same shape as the
# ip_is_at_entry flag this redesign replaced.
# ck-t5bc's N2, which my first version of this row could not see: they kept the
# extraction TEXT, routed it into an unused variable, and left fault_rsp at 0.
# Every row stayed green while every live capture would print
# "[rsp]=UNREADABLE" — the exact defect this row was written for, in the one
# spelling it could not detect, because it asserted PRESENCE of the extraction
# rather than its USE.
#
# The fix is the PY_BUILDERS rule I wrote for ck-qnft, turned on myself: a
# declared capability must be referenced TWICE, a definition AND a use, because
# the defect removes only the argument and leaves the assignment behind. Here
# the leaf is a variable rather than a source path; the rule is identical.
# ── DO NOT COLLAPSE THE NEXT TWO ROWS. THEY LOOK REDUNDANT AND ARE NOT. ─────
# ck-t5bc drove both halves separately and neither is sufficient alone:
#
#   fault_rsp_is_INITIALISED_from_REG_RSP   reads RAW SOURCE, so a mutant of the
#       form `const uintptr_t fault_rsp = 0 /* was REG_RSP */;` SATISFIES it —
#       the comment contains the token. Caught only by the second row, because
#       the real extraction is gone.
#   the_handler_extracts_rsp_AND_PASSES_IT  counts the NAME at its use sites, so
#       a mutant that keeps `fault_rsp` at all three calls and sets it to a
#       constant SATISFIES it. Caught only by the first row.
#
# So: comment-satisfiable alone, name-satisfiable alone, and together neither
# evasion survives. The obvious tidy-up is to merge them; that reopens a hole in
# whichever direction is dropped. This is the third place tonight where a
# raw-source grep was the weak link (after the registration census counting
# comment lines and the bare-envelope census flagging a docstring), and the
# repair each time was a SECOND check that fails differently, not a cleverer
# single one.
#
# AND MY FIRST FIX FOR THAT WAS ALSO INSUFFICIENT — the fifth instance. Tying
# the definition to the use by NAME still passes ck-t5bc's mutant, because it
# keeps the name `fault_rsp` at all three call sites and only kills its VALUE
# (`const uintptr_t fault_rsp = 0;` beside an unused copy of the extraction).
# The grep saw the token and not the semantics.
#
# So the binding must be DEFINITION-TO-SOURCE, not definition-to-name: whatever
# initialises fault_rsp must itself mention REG_RSP. That is the smallest check
# that cannot be satisfied by a constant.
rspdef=$(python3 - "$HND" <<'PYEOF'
import re, sys
src = open(sys.argv[1]).read()
m = re.search(r"const\s+uintptr_t\s+fault_rsp\s*=(.*?);", src, re.S)
print(1 if (m and "REG_RSP" in m.group(1)) else 0)
PYEOF
)
if [ "$rspdef" -ne 1 ]; then
    echo -e "fault_rsp_is_INITIALISED_from_REG_RSP\t0\t(its initialiser does not mention REG_RSP — a constant would pass every other row)"
    fails=1
else
    echo -e "fault_rsp_is_INITIALISED_from_REG_RSP\t1\t(initialiser references REG_RSP)"
fi
rspx=$(grep -c 'uc_mcontext.gregs\[REG_RSP\]' "$HND")
rspu=$(grep -c 'format_frame_word(retbuf, sizeof retbuf, fault_rsp' "$HND")
if [ "$rspx" -ge 1 ] && [ "$rspu" -eq 3 ]; then
    echo -e "the_handler_extracts_rsp_AND_PASSES_IT\t1\t(defined $rspx, used $rspu/3)"
else
    echo -e "the_handler_extracts_rsp_AND_PASSES_IT\t0\t(defined $rspx want 1, used $rspu want 3)"; fails=1
fi
fsites=$(grep -c 'format_frame_word(retbuf, sizeof retbuf' "$HND")
if [ "$fsites" -eq 3 ]; then
    echo -e "all_handler_log_sites_emit_the_frame_word\t1\t($fsites/3)"
else
    echo -e "all_handler_log_sites_emit_the_frame_word\t0\t($fsites/3)"; fails=1
fi
tsites=$(grep -c 'fmt_thread(thrbuf, sizeof thrbuf)' "$HND")
if [ "$tsites" -eq 3 ]; then
    echo -e "handler_log_sites_report_the_thread\t1\t($tsites/3)"
else
    echo -e "handler_log_sites_report_the_thread\t0\t($tsites/3)"; fails=1
fi
# Every bridge-created thread must register, or a real bridge thread reports
# UNREGISTERED and an absent entry stops meaning "not a bridge thread".
# ck-tdco: PER-ROLE and comment-stripped. The grep this replaces counted raw
# source lines, so commenting a registration out left it green, and a prose
# comment mentioning register_thread( could substitute for a deleted one.
if ! python3 bridge/test/registration_census.py; then
    census_st=$?
    [ "$census_st" -eq 3 ] && { echo "NOT EVALUABLE: registration census could not run"; exit 3; }
    fails=1
fi
if [ "$sites" -eq 3 ]; then
    echo -e "handler_log_sites_use_the_shared_formatter\t1\t($sites/3)"
else
    echo -e "handler_log_sites_use_the_shared_formatter\t0\t($sites/3)"; fails=1
fi
# And the hoist itself: the extraction must sit ABOVE the protected-region
# branch, or the forwarded path — the one that kills the process — is blind
# again. Compare line numbers rather than trusting the diff that put it there.
ext=$(grep -n 'gregs\[REG_RIP\]' "$HND" | head -1 | cut -d: -f1)
brc=$(grep -n 'if (g_in_protected_region) {' "$HND" | head -1 | cut -d: -f1)
if [ -n "$ext" ] && [ -n "$brc" ] && [ "$ext" -lt "$brc" ]; then
    echo -e "rip_extraction_is_above_the_protected_branch\t1\t(L$ext < L$brc)"
else
    echo -e "rip_extraction_is_above_the_protected_branch\t0\t(L${ext:-?} vs L${brc:-?})"; fails=1
fi

echo
if [ "$probe_st" -ne 0 ] || [ "$fails" -ne 0 ]; then
    echo "FAIL (probe exit=$probe_st, §C fails=$fails)"; exit 1
fi
echo "PASS"; exit 0
