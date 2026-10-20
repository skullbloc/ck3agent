#!/usr/bin/env bash
# Build the stub proxy version.dll (x86_64 PE) from version_proxy.c.
#
#   ./build.sh [OUTDIR]            # default OUTDIR: ./out (never commit it)
#   CC=x86_64-w64-mingw32-clang ./build.sh
#
# CC defaults to x86_64-w64-mingw32-gcc (Fedora: dnf install mingw64-gcc);
# llvm-mingw's x86_64-w64-mingw32-clang works too. The .def is derived from
# version_exports.h, so the exported names are exactly the generated list.
# After the build the export table of the result is compared with that list;
# a mismatch fails the build.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${1:-$here/out}
CC=${CC:-x86_64-w64-mingw32-gcc}
mkdir -p "$out"

def=$out/version_proxy.def
{
  echo "LIBRARY version.dll"
  echo "EXPORTS"
  sed -nE 's/^X\(([A-Za-z0-9_]+), *[0-9]+\).*/  \1=proxy_\1/p' "$here/../../windows/version_exports.h"
} > "$def"
want=$(grep -c '=proxy_' "$def")
[ "$want" -gt 0 ] || { echo "build.sh: no exports parsed from version_exports.h" >&2; exit 1; }

"$CC" -shared -O2 -Wall -Wextra -o "$out/version.dll" \
  "$here/version_proxy.c" "$def" -static-libgcc -Wl,--kill-at

# Check the result's export table against the list (fail open: any parse
# trouble is a failure, never a pass).
got=$(python3 -I "$here/../../windows/gen_exports.py" "$out/version.dll" | grep -c '^X(' || true)
if [ "$got" != "$want" ]; then
  echo "build.sh: version.dll exports $got names, list has $want" >&2
  exit 1
fi
echo "built $out/version.dll ($want exports, $("$CC" --version | head -1))"
sha256sum "$out/version.dll"
