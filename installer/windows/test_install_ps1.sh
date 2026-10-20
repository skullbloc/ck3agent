#!/usr/bin/env bash
# Exercise install.ps1 / uninstall.ps1 under PowerShell on Linux (P7 prep; the real
# Windows run is P7). Needs pwsh (PWSH=<path>), a ck3agent wheel source tree (a
# release export: SRC=<dir with pyproject.toml>), the bridge DLL (DLL=<version.dll>)
# and a ck3.exe of the pinned build (EXE=<ck3.exe>). The Windows-only parts
# (registry, Steam libraries) are bypassed with CK3AGENT_GAME_DIR; the rest runs
# as written: SHA256SUMS, the venv from bundled wheels, the identity check
# through ck3agent._pe, the DLL placement and its refusals, .mcp.json, uninstall.
set -Eeuo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${PWSH:?PWSH=<pwsh>}" "${SRC:?SRC=<export tree>}" "${DLL:?DLL=<version.dll>}" "${EXE:?EXE=<pinned ck3.exe>}"
pin="${PIN:-$here/../../../bridge/src/os/windows/pinned_identity.h}"
t="$(mktemp -d)"; trap 'rm -rf "$t"' EXIT
fail=0; ok() { echo "ok:   $*"; }; bad() { echo "FAIL: $*"; fail=$((fail+1)); }

python3 -m pip wheel --quiet --wheel-dir "$t/wheels" "$SRC" >/dev/null   # with its dependencies, as bundle_wheels.sh
python3 "$here/package_windows.py" --dll "$DLL" --wheels "$t/wheels" --version 0.0.0-test \
  --pinned "$pin" --out "$t/r.zip" >/dev/null
(cd "$t" && python3 -m zipfile -e r.zip unz)
z="$t/unz/ck3agent-0.0.0-test-windows-x86_64"

game="$t/game"; mkdir -p "$game/binaries"; ln -s "$(readlink -f "$EXE")" "$game/binaries/ck3.exe"
export CK3AGENT_GAME_DIR="$game" CK3AGENT_DATA_DIR="$t/data" CK3AGENT_PLAY_DIR="$t/play"
run() { "$PWSH" -NoProfile -NonInteractive -File "$@" >"$t/out.txt" 2>&1; }

if run "$z/install.ps1"; then ok "install on the pinned build"; else bad "install on the pinned build"; cat "$t/out.txt"; fi
cmp -s "$game/binaries/version.dll" "$DLL" && ok "version.dll placed beside ck3.exe" || bad "version.dll not placed"
python3 - "$t/play/.mcp.json" <<'PY' && ok ".mcp.json names an existing ck3agent command" || bad ".mcp.json"
import json, os, sys
e = json.load(open(sys.argv[1]))["mcpServers"]["ck3"]
sys.exit(0 if os.path.exists(e["command"]) and e["args"] == ["serve"] else 1)
PY
grep -q '"identity_match": true' "$t/data/install.json" && ok "identity matched pin.json" || bad "identity match not recorded"
[ "$(head -c3 "$t/play/.mcp.json" | od -An -tx1 | tr -d ' ')" != "efbbbf" ] && ok ".mcp.json has no byte-order mark" || bad ".mcp.json starts with a BOM"

if run "$z/install.ps1"; then ok "reinstall over its own DLL"; else bad "reinstall over its own DLL"; fi

other="$t/game2"; mkdir -p "$other/binaries"; cp "$DLL" "$other/binaries/ck3.exe"
if CK3AGENT_GAME_DIR="$other" run "$z/install.ps1"; then bad "another build accepted"; else
  grep -q "not ck3.exe 1.18.3.1\|is not ck3.exe" "$t/out.txt" && ok "another build refused" || bad "refusal message"; fi
[ ! -e "$other/binaries/version.dll" ] && ok "nothing placed beside the other build" || bad "a DLL was placed on refusal"

third="$t/game3"; mkdir -p "$third/binaries"; ln -s "$(readlink -f "$EXE")" "$third/binaries/ck3.exe"
echo foreign > "$third/binaries/version.dll"
if CK3AGENT_GAME_DIR="$third" run "$z/install.ps1"; then bad "foreign version.dll overwritten"; else
  [ "$(cat "$third/binaries/version.dll")" = foreign ] && ok "a foreign version.dll is left alone" || bad "foreign dll changed"; fi

echo tamper >> "$z/README.txt"
if run "$z/install.ps1"; then bad "a tampered zip installed"; else
  grep -q "does not match SHA256SUMS" "$t/out.txt" && ok "a tampered file is refused (SHA256SUMS)" || bad "tamper message"; fi

if run "$z/uninstall.ps1"; then ok "uninstall"; else bad "uninstall"; cat "$t/out.txt"; fi
[ ! -e "$game/binaries/version.dll" ] && ok "version.dll removed" || bad "version.dll left"
[ ! -e "$t/data" ] && ok "data dir removed" || bad "data dir left"
python3 -c "import json,sys; sys.exit('ck3' in json.load(open('$t/play/.mcp.json'))['mcpServers'])" \
  && ok "ck3 entry removed from .mcp.json" || bad "ck3 entry left"
echo "$fail failures"; exit $((fail > 0))
