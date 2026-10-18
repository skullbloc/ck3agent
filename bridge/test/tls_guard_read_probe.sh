#!/usr/bin/env bash
# ck-62eo acceptance. Host-only: no game, no bridge, no Ghidra.
# Exit: 0 pass / 1 FAIL / 3 NOT EVALUABLE.
#
#   §A  DRIVEN — the claim the fix RESTS on, against the shipped bridge.h:
#       safe_read cannot tell a failed read from a real zero, safe_read_bytes
#       can. Zero is a meaningful value at the guard site, so that difference
#       is the whole reason the simpler fix was rejected.
#   §B  READ   — that the SITE uses the right primitive, and that no raw
#       dereference of the derived address survives. ai_eval_hook.cpp cannot
#       link without the engine, so this is grepped, and is labelled as read
#       rather than presented as if §A covered it.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 3

SRC=bridge/test/tls_guard_read_probe.cpp
HDR=bridge/include/bridge.h
SITE=bridge/src/ai_eval_hook.cpp
for f in "$SRC" "$HDR" "$SITE"; do
    [ -f "$f" ] || { echo "NOT EVALUABLE: missing $f"; exit 3; }
done
command -v g++ >/dev/null || { echo "NOT EVALUABLE: no g++"; exit 3; }

BIN=$(mktemp -d) || exit 3
trap 'rm -rf -- "${BIN:?}"' EXIT

echo "── §A: the read primitives (DRIVEN) ──────────────────────────────"
g++ -std=c++17 -O1 -Wall -o "$BIN/p" "$SRC" || { echo "NOT EVALUABLE: probe did not build"; exit 3; }
"$BIN/p"; probe_st=$?

echo
echo "── §B: the guard site (a READ) ───────────────────────────────────"
fails=0
chk() {  # name, actual, expected, detail
    if [ "$2" = "$3" ]; then echo -e "$1\t1\t$4"; else echo -e "$1\t0\t$4"; fails=1; fi
}

# The ONLY surviving *guard_byte dereference must be the guarded store. Every
# read now goes through safe_read_bytes into guard_value.
derefs=$(grep -c '\*guard_byte' "$SITE")
chk "only_one_guard_byte_dereference_remains" "$derefs" "1" "($derefs found, want 1: the guarded store)"

# That one deref must sit inside a protected region. Check the nearest preceding
# sigsetjmp is closer than the nearest preceding is-not-protected marker.
dl=$(grep -n '\*guard_byte' "$SITE" | head -1 | cut -d: -f1)
sj=$(awk -v d="$dl" 'NR<d && /sigsetjmp\(g_recovery_point/ {n=NR} END{print n+0}' "$SITE")
pr=$(awk -v d="$dl" 'NR<d && /g_in_protected_region = true/ {n=NR} END{print n+0}' "$SITE")
if [ "$sj" -gt 0 ] && [ "$pr" -gt 0 ] && [ $((dl - sj)) -lt 6 ]; then
    echo -e "the_surviving_deref_is_inside_a_sigsetjmp\t1\t(store L$dl, sigsetjmp L$sj)"
else
    echo -e "the_surviving_deref_is_inside_a_sigsetjmp\t0\t(store L$dl, nearest sigsetjmp L$sj)"; fails=1
fi

# The read must use safe_read_bytes — and must NOT use safe_read<uint8_t>, which
# would return 0 on failure and silently mean "construct the context".
srb=$(grep -c "safe_read_bytes(reinterpret_cast<uintptr_t>(guard_byte)" "$SITE")
chk "the_guard_read_uses_safe_read_bytes" "$srb" "1" "($srb)"
wrong=$(grep -c 'safe_read<uint8_t>(reinterpret_cast<uintptr_t>(guard_byte)' "$SITE")
chk "the_guard_read_does_NOT_use_safe_read" "$wrong" "0" "($wrong — it would alias failure to 'construct')"

# An unreadable guard must REFUSE, not default either way.
# ONE PER CALL SITE. My first cut asserted exactly 1 and went red when fixing
# the SECOND site made it 2 — a row failing against correct code because its
# expected value was written before the duplicate was known. The property is
# "every site that reads the guard refuses when it cannot", so the count tracks
# the number of call sites.
ref=$(grep -c 'tls_guard_unreadable' "$SITE")
chk "every_call_site_refuses_an_unreadable_guard" "$ref" "2" "($ref, want 2: one per site)"
refw=$(grep -c 'tls_guard_unwritable' "$SITE")
chk "every_call_site_refuses_an_unwritable_guard" "$refw" "2" "($refw, want 2)"

# The refusal must say WHICH offsets it was running on: discovery fails as the
# normal case, so "discovered" vs "HARDCODED_FALLBACK" is the first thing a
# reader of such a refusal needs.
rd=$(grep -c 'read_tls_guard(guard_byte)' "$SITE")
chk "both_call_sites_use_the_shared_read_helper" "$rd" "2" "($rd, want 2: dispatch + trigger_query)"
wr=$(grep -c 'set_tls_guard(guard_byte)' "$SITE")
chk "both_call_sites_use_the_shared_write_helper" "$wr" "2" "($wr, want 2)"

prov=$(grep -c 'g_tls_offsets_discovered' "$SITE")
if [ "$prov" -ge 4 ]; then
    echo -e "offset_provenance_is_recorded_and_reported\t1\t($prov references)"
else
    echo -e "offset_provenance_is_recorded_and_reported\t0\t($prov, want >=4)"; fails=1
fi

echo
if [ "$probe_st" -ne 0 ] || [ "$fails" -ne 0 ]; then
    echo "FAIL (probe exit=$probe_st, §B fails=$fails)"; exit 1
fi
echo "PASS"; exit 0
