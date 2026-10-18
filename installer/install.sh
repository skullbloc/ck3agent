#!/usr/bin/env bash
# ck3agent Linux installer. Run from the unpacked release directory.
#
#   ./install.sh [--allow-root] [--no-setup] [--no-pipx] [ck3agent setup options...]
#
# Installs the ck3agent command (pipx if present, else a private venv in the
# data dir) and the bridge library under ${XDG_DATA_HOME:-~/.local/share}/ck3agent,
# then runs `ck3agent setup` with any remaining options. --no-setup skips it,
# --no-pipx forces the private venv.
set -Eeuo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
data="${XDG_DATA_HOME:-$HOME/.local/share}/ck3agent"
bin="$HOME/.local/bin"
allow_root=0 no_setup=0 use_pipx=1
while [ $# -gt 0 ]; do
  case "$1" in
    --allow-root) allow_root=1 ;;
    --no-setup) no_setup=1 ;;
    --no-pipx) use_pipx=0 ;;
    *) break ;;
  esac
  shift
done

stage="checking the system" done_so_far="nothing" newvenv=""
cleanup() {
  [ -z "$newvenv" ] || rm -rf "$newvenv"
  rm -f "$data/.venv.$$" "$bin/.ck3agent.$$" "$data/lib/.libck3_bridge.so.$$" "$data/share/.trigger_subclasses.csv.$$"
}
trap cleanup EXIT
trap 'echo "install failed while $stage (installed so far: $done_so_far)." >&2
      echo "fix the error above and rerun ./install.sh; ./uninstall.sh removes a partial install." >&2' ERR
die() { echo "$*" >&2; exit 1; }

[ -r "$here/VERSION" ] || die "VERSION not found in $here; run install.sh from the unpacked release directory"
ver="$(cat "$here/VERSION")"
echo "ck3agent $ver installer"
case "$(uname -s)-$(uname -m)" in
  Linux-x86_64) ;;
  *) die "Linux x86_64 only (this is $(uname -s)-$(uname -m))" ;;
esac
if [ "$(id -u)" = 0 ] && [ "$allow_root" = 0 ]; then
  die "do not run as root; run as the user who plays (--allow-root overrides)"
fi

# The release bundles wheels for Python 3.11-3.14: prefer those, then any newer python3.
py=""
for c in python3.14 python3.13 python3.12 python3.11 python3; do
  if command -v "$c" >/dev/null 2>&1 && "$c" -c 'import sys; sys.exit(sys.version_info < (3, 11))'; then
    py="$(command -v "$c")"; break
  fi
done
[ -n "$py" ] || die "need Python 3.11 or newer on PATH"
pyver="$("$py" -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
echo "python: $("$py" --version) ($py)"

# Some distros ship python without venv/ensurepip. Find out before copying anything.
probe="$(mktemp -d)"
if ! "$py" -m venv "$probe/v" >/dev/null 2>&1; then
  rm -rf "$probe"
  # shellcheck disable=SC1091
  id="$( . /etc/os-release 2>/dev/null; echo "${ID:-} ${ID_LIKE:-}" )"
  case " $id " in
    *" debian "*|*" ubuntu "*) hint="sudo apt install python$pyver-venv" ;;
    *" fedora "*|*" rhel "*)   hint="sudo dnf install python-pip-wheel" ;;
    *" arch "*)                hint="sudo pacman -S python   (reinstall: its venv module ships in that package)" ;;
    *)                         hint="install your distro's venv/pip package for python $pyver" ;;
  esac
  die "$py cannot create a venv. Run: $hint"
fi
rm -rf "$probe"

wheel_files=("$here/wheels/ck3agent-$ver-"*.whl)
if ! { [ ${#wheel_files[@]} -eq 1 ] && [ -f "${wheel_files[0]}" ]; }; then die "expected one wheels/ck3agent-$ver-*.whl in $here"; fi
wheel="${wheel_files[0]}"
# With dependency wheels bundled, try them alone first (works offline). Scoped to
# the package install: a global PIP_NO_INDEX would also break pipx's own
# bootstrap of pip. A python newer than the bundle falls back to the index.
bundled=0
[ "$(find "$here/wheels" -name '*.whl' | wc -l)" -gt 1 ] && bundled=1

# Drop a stale copy of the other install method (uninstall.sh --only-pipx|--only-venv).
drop() { bash "$here/uninstall.sh" "$1" >/dev/null 2>&1 || true; }

# Copy to a temporary name, then rename over the old one: a failure leaves the old
# library in place, and a running game keeps the file it loaded.
copy_bridge() {
  stage="copying the bridge library"
  mkdir -p "$data/lib" "$data/share"
  install -m 0755 "$here/lib/libck3_bridge.so" "$data/lib/.libck3_bridge.so.$$"
  install -m 0644 "$here/share/trigger_subclasses.csv" "$data/share/.trigger_subclasses.csv.$$"
  mv -f "$data/lib/.libck3_bridge.so.$$" "$data/lib/libck3_bridge.so"
  mv -f "$data/share/.trigger_subclasses.csv.$$" "$data/share/trigger_subclasses.csv"
}

exe="" how=""
stage="installing the python package"
if [ "$use_pipx" = 1 ] && command -v pipx >/dev/null 2>&1; then
  pip_args=()
  [ "$bundled" = 1 ] && pip_args=(--pip-args "--no-index --find-links \"$here/wheels\"")
  if out="$(pipx install --force --python "$py" "${pip_args[@]}" "$wheel" 2>&1)"; then
    how="pipx"
    exe="${PIPX_BIN_DIR:-$HOME/.local/bin}/ck3agent"
    drop --only-venv
    copy_bridge
  else
    echo "pipx install failed; using a private venv instead. pipx said:" >&2
    printf '%s\n' "$out" | tail -n 6 >&2
  fi
fi
if [ -z "$how" ]; then
  mkdir -p "$data" "$bin"
  # Build beside the live venv and swap: a failed upgrade leaves the old one working.
  # DATA/venv is a link to the current venv.<version>.<random>, and the paths `ck3agent
  # setup` writes into .mcp.json and the hook go through it, so they outlive the swap.
  newvenv=$(mktemp -d "$data/venv.$ver.XXXXXX")   # a fresh name: never the live venv's
  "$py" -m venv "$newvenv"
  done_so_far="new venv at $newvenv (previous install untouched)"
  pipcmd=("$newvenv/bin/python" -m pip install --quiet --disable-pip-version-check)
  if [ "$bundled" = 1 ]; then
    "${pipcmd[@]}" --no-index --find-links "$here/wheels" "$wheel" ||
      { echo "bundled wheels do not cover python $pyver; trying the package index" >&2
        "${pipcmd[@]}" --find-links "$here/wheels" "$wheel"; }
  else
    "${pipcmd[@]}" "$wheel"
  fi
  "$newvenv/bin/ck3agent" --version >/dev/null
  copy_bridge
  stage="switching to the new venv"
  drop --only-pipx
  # An older install (rc1, or by hand) has a real directory here: move it aside.
  if [ -d "$data/venv" ] && [ ! -L "$data/venv" ]; then mv -T "$data/venv" "$data/venv.prev.$$"; fi
  livevenv="$newvenv"
  ln -s "${newvenv##*/}" "$data/.venv.$$"
  mv -Tf "$data/.venv.$$" "$data/venv"
  newvenv=""                                   # live now: the cleanup trap must keep it
  ln -s "$data/venv/bin/ck3agent" "$bin/.ck3agent.$$"
  mv -Tf "$bin/.ck3agent.$$" "$bin/ck3agent"
  for old in "$data"/venv.*; do
    if [ -e "$old" ] && [ "$old" != "$livevenv" ]; then rm -rf "$old"; fi
  done
  how="venv"
  exe="$bin/ck3agent"
fi
done_so_far="python package ($how), bridge in $data/lib"

stage="verifying the install"
got="$("$exe" --version)" || die "$exe does not run"
case "$got" in
  *"$ver"*) echo "$got" ;;
  *) die "$exe reports '$got', expected $ver (an older ck3agent shadows it?)" ;;
esac
case ":$PATH:" in
  *":$(dirname "$exe"):"*) ;;
  *) echo "add $(dirname "$exe") to PATH: export PATH=\"$(dirname "$exe"):\$PATH\"" ;;
esac
echo "installed: ck3agent $ver ($how), bridge $data/lib/libck3_bridge.so"

if [ "$no_setup" = 0 ]; then
  exec "$exe" setup "$@"
fi
cat <<'MSG'
next:
  1. ck3agent setup
  2. ck3agent launch --continue
  3. cd ~/ck3agent-play && claude
MSG
