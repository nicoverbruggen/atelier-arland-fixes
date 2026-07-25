#!/usr/bin/env bash
# Day-to-day local build. Cross-compiles every Windows target with MinGW inside
# the build container, producing:
#   build64/d3d11.dll          (64-bit game DLL)
#   build64/arland-fix-launcher.exe  (64-bit settings GUI)
#   build32/msimg32.dll        (32-bit settings-launcher proxy)
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

# Every target is built by the meson compile above; this only confirms each one
# actually landed, so a silently dropped target cannot pass for a good build.
echo
status=0
for artifact in build64/d3d11.dll build64/arland-fix-launcher.exe build32/msimg32.dll; do
  if [[ -f "$repo/$artifact" ]]; then
    echo "  ok      $artifact"
  else
    echo "  MISSING $artifact" >&2
    status=1
  fi
done
exit "$status"
