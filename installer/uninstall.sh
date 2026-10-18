#!/usr/bin/env bash
# Remove the ck3agent package and data. Keeps ~/.config/ck3agent and your play
# directory unless --purge is given.
set -euo pipefail
data="${XDG_DATA_HOME:-$HOME/.local/share}/ck3agent"
conf="${XDG_CONFIG_HOME:-$HOME/.config}/ck3agent"
if command -v pipx >/dev/null 2>&1 && pipx list 2>/dev/null | grep -q 'package ck3agent'; then
  pipx uninstall ck3agent
else
  python3 -m pip uninstall -y ck3agent || true
fi
rm -rf "$data"
echo "removed $data"
if [ "${1:-}" = "--purge" ]; then rm -rf "$conf"; echo "removed $conf"; fi
