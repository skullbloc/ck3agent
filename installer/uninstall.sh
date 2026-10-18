#!/usr/bin/env bash
# Remove the ck3agent package, venv and data dir.
#
#   ./uninstall.sh [--purge]
#
# Config and your play directory stay unless --purge is given. --purge also
# removes the config dir and asks before removing the play directory.
# (install.sh calls --only-pipx / --only-venv to drop the other method's copy.)
set -euo pipefail
data="${XDG_DATA_HOME:-$HOME/.local/share}/ck3agent"
conf="${XDG_CONFIG_HOME:-$HOME/.config}/ck3agent"
bin="$HOME/.local/bin"
purge=0 mode=all
case "${1:-}" in
  --purge) purge=1 ;;
  --only-pipx) mode=pipx ;;
  --only-venv) mode=venv ;;
esac

remove_pipx_copy() {
  if command -v pipx >/dev/null 2>&1 && pipx list 2>/dev/null | grep -q 'package ck3agent'; then
    if pipx uninstall ck3agent >/dev/null 2>&1; then echo "removed pipx package ck3agent"; fi
  fi
  # pipx may be gone or broken since it installed this: remove its venv and links ourselves
  local d l
  for d in "${PIPX_HOME:-$HOME/.local/share/pipx}" "${XDG_DATA_HOME:-$HOME/.local/share}/pipx" "$HOME/.local/pipx"; do
    [ -d "$d/venvs/ck3agent" ] || continue
    for l in "${PIPX_BIN_DIR:-$bin}/ck3agent" "$bin/ck3agent"; do
      if [ -L "$l" ] && [[ "$(readlink "$l")" == "$d/venvs/ck3agent/"* ]]; then rm -f "$l"; fi
    done
    rm -rf "$d/venvs/ck3agent"
    echo "removed $d/venvs/ck3agent"
  done
}
remove_venv_copy() {
  local l="$bin/ck3agent" v
  if [ -L "$l" ] && [[ "$(readlink "$l")" == "$data"/venv* ]]; then rm -f "$l"; fi
  for v in "$data"/venv "$data"/venv.*; do   # DATA/venv is a link to a venv.<version>.<random>
    if [ -e "$v" ] || [ -L "$v" ]; then rm -rf "$v"; fi
  done
}

if [ "$mode" = pipx ]; then remove_pipx_copy; exit 0; fi
if [ "$mode" = venv ]; then remove_venv_copy; exit 0; fi

play=""
if [ "$purge" = 1 ] && [ -f "$conf/config.toml" ]; then
  play="$(sed -n 's/^play_dir = "\(.*\)"$/\1/p' "$conf/config.toml")"
fi

remove_pipx_copy
remove_venv_copy
rm -rf "$data"
echo "removed $data"

if [ "$purge" = 1 ]; then
  rm -rf "$conf"
  echo "removed $conf"
  if [ -n "$play" ] && [ -f "$play/.mcp.json" ]; then
    if [ -t 0 ]; then
      read -r -p "remove play directory $play? [y/N] " ans
      if [ "$ans" = y ] || [ "$ans" = Y ]; then rm -rf "$play"; echo "removed $play"; fi
    else
      echo "kept $play (not interactive)"
    fi
  fi
fi
echo "if you set a Steam launch option for the bridge, remove it (Steam > CK3 > Properties > Launch Options)"
