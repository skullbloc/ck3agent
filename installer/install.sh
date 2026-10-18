#!/usr/bin/env bash
# ck3agent Linux installer — run from the unpacked release directory.
#
#   ./install.sh [--no-setup] [setup options...]
#
# Puts the bridge library and data under ${XDG_DATA_HOME:-~/.local/share}/ck3agent,
# installs the ck3agent Python package for this user (pipx if present, else pip --user),
# then runs `ck3agent setup` (which finds your CK3 install and creates a play directory).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
data="${XDG_DATA_HOME:-$HOME/.local/share}/ck3agent"
ver="$(cat "$here/VERSION")"
no_setup=0
if [ "${1:-}" = "--no-setup" ]; then no_setup=1; shift; fi

echo "ck3agent $ver — Linux installer"
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) ;;
  *) echo "this release is for Linux x86_64 only (got $(uname -s)-$(uname -m))"; exit 1 ;;
esac

py=""
for c in python3.13 python3.12 python3.11 python3; do
  if command -v "$c" >/dev/null 2>&1 && "$c" -c 'import sys; sys.exit(0 if sys.version_info >= (3,11) else 1)'; then py="$c"; break; fi
done
if [ -z "$py" ]; then echo "need Python 3.11 or newer on PATH"; exit 1; fi
echo "python: $($py --version) ($py)"

mkdir -p "$data/lib" "$data/share"
install -m 0755 "$here/lib/libck3_bridge.so" "$data/lib/libck3_bridge.so"
install -m 0644 "$here/share/trigger_subclasses.csv" "$data/share/trigger_subclasses.csv"
echo "bridge:  $data/lib/libck3_bridge.so"

wheel="$(ls "$here"/wheels/ck3agent-*.whl | head -1)"
if command -v pipx >/dev/null 2>&1; then
  pipx install --force "$wheel" >/dev/null
  echo "package: installed with pipx"
else
  "$py" -m pip install --user --upgrade --quiet "$wheel"
  echo "package: installed with pip --user"
fi

if ! command -v ck3agent >/dev/null 2>&1; then
  bin="$HOME/.local/bin"
  echo
  echo "NOTE: $bin is not on your PATH. Add it (e.g. in ~/.bashrc):"
  echo "    export PATH=\"\$HOME/.local/bin:\$PATH\""
  export PATH="$bin:$PATH"
fi
ck3agent version

if [ "$no_setup" = 0 ]; then
  echo
  ck3agent setup "$@"
fi
