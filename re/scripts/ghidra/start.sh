#!/usr/bin/env bash
# Start the resident pyghidra query server (ghserver.py): opens one Ghidra
# project ONCE, read-only, and serves decompile/func_at/xrefs/callees/by_name/
# symsearch/findstr/findptr/read on 127.0.0.1:$GHIDRA_MCP_PORT (default 18741).
# Client: gh.py beside this script (or the staged copy, below).
#   python3 re/scripts/ghidra/gh.py '{"cmd":"ping"}'                         # readiness + function count
#   python3 re/scripts/ghidra/gh.py '{"cmd":"func_at","va":"0x062b8cf0"}'    # IMAGE VA = offset + image base
#
# Configuration is environment; a file sourced first fills what is unset:
#   CK3_GHIDRA_ENV          file to source (default: host.env beside this script, if present)
#   GHIDRA_INSTALL_DIR      Ghidra install root (required)
#   CK3_GHIDRA_PROJECT_DIR  directory holding the .gpr (required)
#   CK3_GHIDRA_PROJECT_NAME project name (default: ck3)
#   GHIDRA_PY               python with pyghidra installed (default: python3)
#   GHIDRA_MCP_PORT         TCP port on 127.0.0.1 (default: 18741)
#   CK3_GHIDRA_STAGE_DIR    if set, copy gh.py + ghserver.py there and run the copies
#   CK3_GHIDRA_LOG          server log (default: $XDG_STATE_HOME/ck3agent/ghserver.log)
#
# One server per project, ever: Ghidra projects are single-writer. Do not run
# analyzeHeadless or another pyghidra against the project while it is up, and
# never delete .lock files. Launch detached (this script does: setsid nohup ...
# & disown); a job-controlled background task that gets stopped takes the JVM
# with it. Opening a multi-GB project takes minutes; poll with ping.

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ENV_FILE="${CK3_GHIDRA_ENV:-$HERE/host.env}"
if [ -f "$ENV_FILE" ]; then
  # shellcheck source=/dev/null
  . "$ENV_FILE"
fi

: "${GHIDRA_INSTALL_DIR:?set GHIDRA_INSTALL_DIR to the Ghidra install root}"
: "${CK3_GHIDRA_PROJECT_DIR:?set CK3_GHIDRA_PROJECT_DIR to the directory holding the .gpr}"
export GHIDRA_INSTALL_DIR CK3_GHIDRA_PROJECT_DIR
export CK3_GHIDRA_PROJECT_NAME="${CK3_GHIDRA_PROJECT_NAME:-ck3}"
export GHIDRA_MCP_PORT="${GHIDRA_MCP_PORT:-18741}"
PY="${GHIDRA_PY:-python3}"
LOG="${CK3_GHIDRA_LOG:-${XDG_STATE_HOME:-$HOME/.local/state}/ck3agent/ghserver.log}"
mkdir -p "$(dirname "$LOG")"

# Already up? Don't start a second instance (Ghidra project lock).
if python3 "$HERE/gh.py" '{"cmd":"ping"}' >/dev/null 2>&1; then
  echo "Ghidra server already up on :$GHIDRA_MCP_PORT" >&2
  exit 0
fi

RUN_DIR="$HERE"
if [ -n "${CK3_GHIDRA_STAGE_DIR:-}" ]; then
  # Refresh the staged copies from the durable ones.
  cp -f "$HERE/gh.py" "$CK3_GHIDRA_STAGE_DIR/gh.py" 2>/dev/null || true
  cp -f "$HERE/ghserver.py" "$CK3_GHIDRA_STAGE_DIR/ghserver.py" 2>/dev/null || true
  RUN_DIR="$CK3_GHIDRA_STAGE_DIR"
fi

# Detached launch: survives the shell/task that started it.
cd "$RUN_DIR"
setsid nohup "$PY" "$RUN_DIR/ghserver.py" </dev/null >"$LOG" 2>&1 &
disown || true

echo "Launching resident pyghidra server (PID $!, log $LOG); opening the project may take minutes." >&2
echo "Poll: python3 $RUN_DIR/gh.py '{\"cmd\":\"ping\"}'" >&2
