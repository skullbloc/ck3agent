#!/usr/bin/env bash
# Put the runtime dependency wheels for every supported Python (3.11-3.14) next
# to the ck3agent wheel, so the release tarball installs offline.
#
#   installer/bundle_wheels.sh DEST_DIR path/to/ck3agent-*.whl
#
# release.yml and installer/test_install.sh both call this: the smoke test
# installs exactly the bundle the release ships. $PYTHON overrides `python`.
set -euo pipefail
[ $# -eq 2 ] || { echo "usage: $0 DEST_DIR WHEEL" >&2; exit 2; }
dest="$1" wheel="$2"
mkdir -p "$dest"
for pv in 3.11 3.12 3.13 3.14; do
  "${PYTHON:-python}" -m pip download -q -d "$dest" --only-binary=:all: --python-version "$pv" "$wheel"
done
