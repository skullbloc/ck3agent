#!/usr/bin/env bash
# Container smoke test for install.sh / uninstall.sh.
#
#   installer/test_install.sh [image | image:mode ...]
#
# Builds the wheel and a RELEASE-SHAPED bundle (dependency wheels from
# installer/bundle_wheels.sh, the same script release.yml runs), then per case
# (image + mode: pipx, nopipx, novenv) runs, each on a FRESH user with no pipx state:
#   A. normal network: install, reinstall (the .mcp.json command survives it), failed
#      upgrade keeps the old install, uninstall
#   B. dead package index: the bundle alone must install
#   C. (pipx cases) pipx<->venv switching leaves no orphan and setup repairs .mcp.json;
#      uninstall works with pipx broken
# Plus: root refusal message, and the missing-venv hint (followed, then rerun).
# Needs docker or podman (CONTAINER_RT overrides), python3 with `build`, network.
set -euo pipefail

dead_index="http://127.0.0.1:9/simple"

if [ "${1:-}" = "--inside" ]; then
  # ---- runs as root inside the container; $2 = mode, $3 = expected install method ----
  mode="$2" how="$3"
  [ -r /etc/os-release ] && . /etc/os-release
  fail() { echo "FAIL: $*" >&2; exit 1; }
  quiet() { local o; o="$("$@" 2>&1)" || { echo "$o" >&2; return 1; }; }
  case "$ID-${VERSION_ID:-rolling}-$mode" in
    ubuntu-22.04-pipx)    quiet apt-get update && quiet apt-get install -y python3.11 python3.11-venv pipx ;;
    ubuntu-22.04-novenv)  quiet apt-get update && quiet apt-get install -y python3.11 ;;
    ubuntu-*-nopipx)      quiet apt-get update && quiet apt-get install -y python3 python3-venv ;;
    ubuntu-*-pipx)        quiet apt-get update && quiet apt-get install -y python3 python3-venv pipx ;;
    debian-*-nopipx)      quiet apt-get update && quiet apt-get install -y python3 ;;
    debian-*-pipx)        quiet apt-get update && quiet apt-get install -y python3 pipx ;;
    fedora-*-pipx)        quiet dnf install -y python3 pipx util-linux ;;
    fedora-*-nopipx)      quiet dnf install -y python3 util-linux ;;
    fedora-*-novenv)      quiet dnf install -y python3 util-linux && quiet rpm -e --nodeps python-pip-wheel ;;
    arch-*-pipx)          quiet pacman -Syu --noconfirm python python-pipx ;;
    arch-*-nopipx)        quiet pacman -Syu --noconfirm python ;;
    *) echo "unsupported case: $ID $mode" >&2; exit 2 ;;
  esac

  cp -r /rel /opt/rel && chmod -R a+rX /opt/rel
  as() { local u="$1"; shift; runuser -u "$u" -- env HOME="/home/$u" PATH="/home/$u/.local/bin:$PATH" "$@"; }
  try() { rc=0; out="$("$@" 2>&1)" || rc=$?; }       # sets $rc and $out
  newuser() { useradd -m "$1"; home="/home/$1"; data="$home/.local/share/ck3agent"; }
  ver="$(cat /opt/rel/VERSION)"
  leftovers() { local l; l="$(find "/home/$1" -iname '*ck3agent*' 2>/dev/null)"; [ -z "$l" ] || { echo "$l" >&2; fail "$2: files left after uninstall"; }; }
  version_ok() { as "$1" ck3agent --version | grep -q "ck3agent $ver" || fail "$2: ck3agent --version"; }
  method_of() { # which install method serves ~/.local/bin/ck3agent
    case "$(readlink -f "/home/$1/.local/bin/ck3agent")" in
      */pipx/venvs/*) echo pipx ;; */ck3agent/venv*) echo venv ;; *) echo other ;;
    esac
  }
  # Merge the ck3 server into $2 the way `ck3agent setup` does (merge_mcp, no --force,
  # exe_argv), with the installed interpreter; print the command it holds.
  mcp_cmd() {
    local py   # the venv's python, beside the console script (pipx or ours)
    py="$(dirname "$(readlink -f "/home/$1/.local/bin/ck3agent")")/python"
    as "$1" "$py" - "$2" <<'PY'
import json, sys
from pathlib import Path
from ck3agent import cli
p = Path(sys.argv[1])
d = json.loads(p.read_text()) if p.exists() else {}
cli.merge_mcp(d, cli.exe_argv(), False)
p.write_text(json.dumps(d))
print(d["mcpServers"]["ck3"]["command"])
PY
  }

  # root is refused, with the reason
  try bash /opt/rel/install.sh --no-setup
  [ "$rc" = 1 ] || fail "root run: want exit 1, got $rc"
  case "$out" in *"do not run as root"*) ;; *) echo "$out" >&2; fail "root run: refused for another reason" ;; esac

  # ---- A: fresh user, normal network, release bundle ----
  newuser ta
  if [ "$mode" = novenv ] || { [ "$ID" = debian ] && [ "$mode" = nopipx ]; }; then
    # python without its venv package: refuse early, name the package that fixes it, copy nothing
    try as ta bash /opt/rel/install.sh --no-setup
    [ "$rc" = 1 ] || fail "missing venv: want exit 1, got $rc"
    if [ "$ID" = fedora ]; then
      pkg="$(printf '%s\n' "$out" | sed -n 's/.*dnf install \([A-Za-z0-9._-]*\).*/\1/p')"
    else
      pkg="$(printf '%s\n' "$out" | sed -n 's/.*apt install \(python[0-9.]*-venv\).*/\1/p')"
    fi
    [ -n "$pkg" ] || { echo "$out" >&2; fail "missing venv: no package hint"; }
    [ ! -e "$data" ] || fail "missing venv: files were copied"
    if [ "$ID" = fedora ]; then quiet dnf install -y "$pkg"; else quiet apt-get install -y "$pkg"; fi  # follow the hint ...
    try as ta bash /opt/rel/install.sh --no-setup        # ... and the rerun must work
    [ "$rc" = 0 ] || { echo "$out" >&2; fail "rerun after following the hint ($pkg) still fails"; }
    as ta bash /opt/rel/uninstall.sh >/dev/null
    leftovers ta "after hint"
  fi
  for round in 1 2; do   # second round: reinstall over an existing install
    try as ta bash /opt/rel/install.sh --no-setup
    [ "$rc" = 0 ] || { echo "$out" >&2; fail "A: install.sh (round $round)"; }
    case "$out" in *"($how)"*) ;; *) echo "$out" >&2; fail "A: expected install method $how" ;; esac
  done
  version_ok ta A
  [ -x "$data/lib/libck3_bridge.so" ] || fail "A: bridge library missing"
  # the command `ck3agent setup` writes into .mcp.json must survive a reinstall
  play=/home/ta/play
  as ta mkdir -p "$play"
  c1="$(mcp_cmd ta "$play/.mcp.json")"
  [ -x "$c1" ] || fail "A: .mcp.json command $c1 does not exist"
  as ta bash /opt/rel/install.sh --no-setup >/dev/null || fail "A: reinstall for .mcp.json"
  [ -x "$c1" ] || fail "A: a reinstall deleted the .mcp.json command $c1"
  c2="$(mcp_cmd ta "$play/.mcp.json")"
  [ "$c2" = "$c1" ] || fail "A: setup after a reinstall changed the command: $c1 -> $c2"

  # failed upgrade of a venv install (no dependency wheels, dead index): the old one keeps working
  as ta bash /opt/rel/install.sh --no-setup --no-pipx >/dev/null || fail "A: venv install"
  cp -r /rel /opt/relbad
  find /opt/relbad/wheels -name '*.whl' ! -name 'ck3agent-*' -delete
  chmod -R a+rX /opt/relbad
  try as ta env PIP_INDEX_URL="$dead_index" bash /opt/relbad/install.sh --no-setup --no-pipx
  [ "$rc" != 0 ] || fail "A: upgrade with no reachable dependencies succeeded"
  version_ok ta "A: after failed upgrade"
  [ "$(find "$data" -maxdepth 1 -name 'venv.*' | wc -l)" = 1 ] || { ls -la "$data" >&2; fail "A: failed upgrade left extra venvs"; }
  if ! { [ -L "$data/venv" ] && [ -x "$data/venv/bin/ck3agent" ]; }; then ls -la "$data" >&2; fail "A: DATA/venv is not a link to the live venv"; fi
  c3="$(mcp_cmd ta "$play/.mcp.json")"           # in pipx cases --no-pipx moved it: setup repairs
  [ -x "$c3" ] || fail "A: .mcp.json command $c3 missing after a failed upgrade"
  # python newer than the bundle (no cpXY wheels for it): fall back to the index, do not die
  cp -r /rel /opt/relgap && find /opt/relgap/wheels -name '*-cp3*' -delete && chmod -R a+rX /opt/relgap
  try as ta bash /opt/relgap/install.sh --no-setup --no-pipx
  [ "$rc" = 0 ] || { echo "$out" >&2; fail "A: bundle gap: no fallback to the index"; }
  case "$out" in *"trying the package index"*) ;; *) echo "$out" >&2; fail "A: bundle gap: index fallback was not exercised" ;; esac
  version_ok ta "A: after bundle-gap install"
  # restore a good install, then uninstall
  as ta bash /opt/rel/install.sh --no-setup >/dev/null
  as ta bash /opt/rel/uninstall.sh >/dev/null || fail "A: uninstall.sh"
  leftovers ta A

  # ---- B: fresh user, dead package index: the bundle alone must install ----
  newuser tb
  try as tb env PIP_INDEX_URL="$dead_index" bash /opt/rel/install.sh --no-setup
  [ "$rc" = 0 ] || { echo "$out" >&2; fail "B: offline install from the release bundle"; }
  version_ok tb B
  echo "   offline install used: $(method_of tb)"
  if [ "$how" = pipx ] && [ "$(method_of tb)" != pipx ]; then
    # pipx could not install offline: the fallback must say so, not switch silently
    case "$out" in *"pipx install failed; using a private venv"*) ;; *) echo "$out" >&2; fail "B: silent fallback from pipx" ;; esac
  fi
  as tb bash /opt/rel/uninstall.sh >/dev/null || fail "B: uninstall.sh"
  leftovers tb B

  # ---- C: switching methods leaves nothing behind; uninstall works with pipx broken ----
  if [ "$mode" = pipx ]; then
    newuser tc
    play=/home/tc/play
    as tc mkdir -p "$play"
    as tc bash /opt/rel/install.sh --no-setup --no-pipx >/dev/null || fail "C: venv install"
    [ "$(method_of tc)" = venv ] || fail "C: --no-pipx did not give a venv"
    mcp_cmd tc "$play/.mcp.json" >/dev/null
    as tc bash /opt/rel/install.sh --no-setup >/dev/null || fail "C: venv -> pipx"
    [ "$(method_of tc)" = pipx ] || fail "C: venv -> pipx: command not served by pipx"
    ls -d "$data"/venv* >/dev/null 2>&1 && fail "C: old venv left after switching to pipx"
    c="$(mcp_cmd tc "$play/.mcp.json")"           # setup replaces the entry whose command is gone
    [ -x "$c" ] || fail "C: venv -> pipx: setup kept a missing .mcp.json command $c"
    as tc bash /opt/rel/install.sh --no-setup --no-pipx >/dev/null || fail "C: pipx -> venv"
    [ "$(method_of tc)" = venv ] || fail "C: pipx -> venv: command not served by the venv"
    find "/home/tc" -path '*pipx/venvs/ck3agent' | grep -q . && fail "C: pipx venv orphaned after switching to venv"
    c="$(mcp_cmd tc "$play/.mcp.json")"
    [ -x "$c" ] || fail "C: pipx -> venv: setup kept a missing .mcp.json command $c"
    as tc rm -rf "$play"
    as tc bash /opt/rel/install.sh --no-setup >/dev/null || fail "C: venv -> pipx (2)"
    mkdir -p /opt/stub && printf '#!/bin/sh\nexit 1\n' > /opt/stub/pipx && chmod a+rx /opt/stub /opt/stub/pipx
    as tc env PATH="/opt/stub:/home/tc/.local/bin:$PATH" bash /opt/rel/uninstall.sh >/dev/null || fail "C: uninstall with broken pipx"
    leftovers tc C
  fi
  echo "ok: $ID ${VERSION_ID:-} $mode ($how)"
  exit 0
fi

# ---- host side ----
src="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
rt="${CONTAINER_RT:-$(command -v docker || command -v podman || true)}"
[ -n "$rt" ] || { echo "need docker or podman" >&2; exit 2; }
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

rel="$work/rel"
mkdir -p "$rel/lib" "$rel/share" "$rel/wheels"
python3 -m build --wheel --outdir "$work/dist" "$src" >/dev/null 2>&1 || { echo "wheel build failed" >&2; exit 2; }
PYTHON=python3 "$src/installer/bundle_wheels.sh" "$rel/wheels" "$work"/dist/ck3agent-*.whl || { echo "bundling wheels failed" >&2; exit 2; }
printf 'not a real library\n' > "$rel/lib/libck3_bridge.so"
printf 'dummy\n' > "$rel/share/trigger_subclasses.csv"
cp "$src/VERSION" "$src/installer/install.sh" "$src/installer/uninstall.sh" "$rel/"
cp "$src/installer/test_install.sh" "$rel/test_install.sh"
chmod -R a+rX "$rel"

# image|mode|expected method. Images are pinned to a tag (CI must not drift).
arch=archlinux:base-20261011.0.609700
default=(ubuntu:22.04\|pipx\|pipx ubuntu:22.04\|novenv\|venv ubuntu:24.04\|nopipx\|venv ubuntu:24.04\|pipx\|pipx
         debian:12\|nopipx\|venv debian:13\|pipx\|pipx fedora:44\|pipx\|pipx fedora:44\|nopipx\|venv
         fedora:44\|novenv\|venv
         "$arch|pipx|pipx" "$arch|nopipx|venv")
cases=()
if [ $# -eq 0 ]; then cases=("${default[@]}"); fi
for a in "$@"; do
  hit=0
  for d in "${default[@]}"; do
    image="${d%%|*}"
    name="${d%|*}"; name="${image}:${name#*|}"
    if [ "$image" = "$a" ] || [ "$name" = "$a" ] || [[ "$image" == "$a:"* ]]; then cases+=("$d"); hit=1; fi
  done
  [ "$hit" = 1 ] || { echo "unknown image: $a" >&2; exit 2; }
done

status=0
for c in "${cases[@]}"; do
  IFS='|' read -r image mode how <<<"$c"
  echo "== $image $mode"
  "$rt" run --rm -v "$rel:/rel:ro,z" "$image" bash /rel/test_install.sh --inside "$mode" "$how" || status=1
done
exit "$status"
