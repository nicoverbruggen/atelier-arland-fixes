#!/usr/bin/env bash
# Day-to-day local build. Cross-compiles both Windows DLLs with MinGW inside the
# build container, producing:
#   build64/d3d11.dll     (64-bit game DLL)
#   build32/msimg32.dll   (32-bit settings-launcher proxy)
#
# The container (default "atfix-build", override with $ATFIX_CONTAINER) provides
# the MinGW-w64 toolchain, meson and ninja. Optional first argument: the meson
# build type (default: release).
#
# CI builds the released binaries with MSVC instead (see .github/workflows/build.yml).
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
container="${ATFIX_CONTAINER:-atfix-build}"
buildtype="${1:-release}"

echo "Building in container '$container' ($buildtype) — $repo"
podman exec -i -w "$repo" "$container" bash -s "$buildtype" <<'EOSH'
set -e
buildtype="$1"
for pair in "build64 build-win64.txt" "build32 build-win32.txt"; do
  set -- $pair
  echo "== $1 =="
  meson setup "$1" --cross-file "$2" --buildtype "$buildtype" --reconfigure >/dev/null 2>&1 \
    || meson setup "$1" --cross-file "$2" --buildtype "$buildtype"
  meson compile -C "$1"
done
EOSH

echo
echo "Done:  build64/d3d11.dll  build32/msimg32.dll"
