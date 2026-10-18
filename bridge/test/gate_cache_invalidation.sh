#!/usr/bin/env bash
# gate_cache_invalidation.sh — ck-hxh7 regression gate.
#
# The bug: clear_name_cache() was DEFINED in name_tables.cpp and DECLARED in
# name_tables.h and called from NOWHERE. A grep across bridge/src and
# bridge/include returned only the definition and the declaration. Because of
# that the bridge served the previous world's character names for the life of
# the process after any save reload.
#
# This gate makes that state unreachable again: every world-scoped cache clear
# listed below must have at least one REAL reference — a call or a registration
# with the world-reset registry — that is not its own definition, not its
# declaration, and not a comment. And the detector itself must be wired into
# on_tick, or the registry would just be a bug one level up.
#
# Usage: gate_cache_invalidation.sh <bridge-source-dir>
set -uo pipefail

BRIDGE_DIR="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
SRC="$BRIDGE_DIR/src"
INC="$BRIDGE_DIR/include"

fail=0
note() { printf '  %s\n' "$1"; }
bad()  { printf 'GATE FAIL: %s\n' "$1" >&2; fail=1; }

# Every world-scoped clear function that must stay wired.
CLEARS=(
    clear_name_cache
    clear_all_sent_events
    clear_war_score_cache
    reset_war_monitor_state
    reset_war_history                             # ck-sd4d's ended-war ledger
    quarantine_personality_locks_on_world_reset   # ck-9fc3's write hazard
    world_identity_on_world_reset                 # ck-9fc3's save identity
)

# Print "file:line: code" for every CODE occurrence of $1 — line comments are
# stripped first so a prose mention never satisfies the gate (that mistake is
# how this bug hid in plain sight).
scan_code() {
    local fn="$1"
    find "$SRC" "$INC" -type f \( -name '*.cpp' -o -name '*.h' \) -print0 \
    | while IFS= read -r -d '' f; do
        awk -v fn="$fn" -v file="$f" '
            {
                line = $0
                sub(/\/\/.*/, "", line)                      # drop line comments
                if (line ~ ("(^|[^A-Za-z0-9_])" fn "([^A-Za-z0-9_]|$)"))
                    print file ":" NR ": " line
            }
        ' "$f"
    done
}

echo "ck-hxh7 gate: world-scoped cache invalidation"

for fn in "${CLEARS[@]}"; do
    all=$(scan_code "$fn")
    # Exclude the definition ("[static] <type> <fn>(" opening a body) and any
    # pure declaration (a prototype line ending in ');').
    real=$(printf '%s\n' "$all" \
        | grep -v -E "(static +)?(void|bool|int) +${fn} *\(" \
        | grep -v -E "${fn} *\([^)]*\) *;[[:space:]]*$" \
        | grep -v '^[[:space:]]*$' || true)

    if [ -z "$real" ]; then
        bad "$fn has no caller or registration — a world reload will not clear its cache."
        printf '%s\n' "$all" | sed 's/^/    /' >&2
    else
        n=$(printf '%s\n' "$real" | grep -c . || true)
        note "ok: $fn — $n real reference(s)"
        printf '%s\n' "$real" | sed 's/^/       /'
    fi
done

if ! grep -q 'world_epoch_on_tick()' "$SRC/bridge_init.cpp"; then
    bad "world_epoch_on_tick() is not called from bridge_init.cpp on_tick — nothing detects a reload."
else
    note "ok: world_epoch_on_tick() is wired into on_tick"
fi

if ! grep -q 'src/world_epoch.cpp' "$BRIDGE_DIR/CMakeLists.txt"; then
    bad "src/world_epoch.cpp is not in the ck3_bridge source list."
else
    note "ok: world_epoch.cpp is in the library build"
fi

if ! grep -q 'src/personality_world_reset.cpp' "$BRIDGE_DIR/CMakeLists.txt"; then
    bad "src/personality_world_reset.cpp is not in the ck3_bridge source list — the personality lock quarantine would never register."
else
    note "ok: personality_world_reset.cpp is in the library build"
fi

if ! grep -q 'src/world_identity.cpp' "$BRIDGE_DIR/CMakeLists.txt"; then
    bad "src/world_identity.cpp is not in the ck3_bridge source list — nothing would decide whether quarantined steering may come back."
else
    note "ok: world_identity.cpp is in the library build"
fi

if ! grep -q 'src/personality_persist.cpp' "$BRIDGE_DIR/CMakeLists.txt"; then
    bad "src/personality_persist.cpp is not in the ck3_bridge source list — the lock file would lose its identity header."
else
    note "ok: personality_persist.cpp is in the library build"
fi

# ck-9fc3: the restore side must stay behind the identity gate. The gate proper
# (exactly one caller, on the preprocessed TU) lives in the acceptance; here we
# only assert that the identity-free loader ck-9fc3 flagged has not come back.
if grep -rnE '(^|[^A-Za-z0-9_])load_personality_locks *\(' "$SRC" "$INC" \
       | grep -v 'load_personality_locks_from_disk' | grep -v '//' | grep -q .; then
    bad "load_personality_locks() is back — an identity-free reader of /tmp/ck3-personality-locks.json carries one world's steering into the next (ck-9fc3)."
else
    note "ok: no identity-free personality-lock loader"
fi

# This gate is the cheap always-on tripwire. The full claims — that a reload
# actually empties every one of these, and that steering comes back ONLY on the
# same save — are the two acceptances.
note "full acceptance: python3 scripts/acceptance_world_reset.py"
note "                python3 scripts/acceptance_save_identity.py"

if [ "$fail" -ne 0 ]; then
    echo "ck-hxh7 gate FAILED" >&2
    exit 1
fi
echo "ck-hxh7 gate PASSED"
