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

# Start the container if it is not already running. It is a build environment,
# not a service, so it is normal for it to be stopped between sessions -- and
# "can only create exec sessions on running containers" is a poor way to find
# that out mid-build.
if ! podman container exists "$container"; then
    echo "Container '$container' does not exist. See BUILDING.md for how to create it." >&2
    exit 1
fi
if [[ "$(podman inspect -f '{{.State.Running}}' "$container" 2>/dev/null)" != "true" ]]; then
    echo "Starting container '$container'"
    podman start "$container" >/dev/null
    # podman start returns before the container is ready to take exec sessions.
    for _ in $(seq 1 20); do
        podman exec "$container" true 2>/dev/null && break
        sleep 0.5
    done
fi

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
[[ $status -eq 0 ]] || exit "$status"

python3 "$repo/scripts/check_exports.py" \
  "$repo/build64/d3d11.dll" "$repo/build32/msimg32.dll"

# Package the same archive the release workflow does, so what gets tested by
# hand locally has the same shape as what users download: same file names, same
# layout, documentation and licences in the same subfolder. Only the version in
# the name differs, since there is no tag to take it from.
#
# Kept out of the repo by /out/ in .gitignore.
if ! command -v zip >/dev/null 2>&1; then
  echo
  echo "zip is not installed; skipping the archive" >&2
  exit 0
fi

out="$repo/out"
stage="$out/stage"
rm -rf "$stage"
mkdir -p "$stage/arland-fix"

# Shipped under its final name so the archive extracts straight into the game
# directory with nothing to rename.
cp "$repo/build64/d3d11.dll" "$repo/build64/arland-fix-launcher.exe" \
   "$repo/build32/msimg32.dll" "$stage/"
cp "$repo/default.ini" "$stage/arland-fix.ini"
cp "$repo/README.md" "$repo/CHANGELOG.md" "$repo/ADVANCED.md" "$repo/LICENSE" \
   "$stage/arland-fix/"
cp -r "$repo/licenses" "$stage/arland-fix/LICENSES"
# Named for what they cover: LICENSE points at them and there is no vendor/
# tree in the archive.
cp "$repo/vendor/minhook/LICENSE.txt" "$stage/arland-fix/LICENSES/MinHook.txt"
cp "$repo/vendor/smaa/LICENSE.txt" "$stage/arland-fix/LICENSES/SMAA.txt"

build_version="$(python3 "$repo/scripts/read_version.py" "$repo/VERSION")"
archive="$out/arland-fix-$build_version.zip"
rm -f "$archive"
( cd "$stage" && zip -qr "$archive" \
    d3d11.dll msimg32.dll arland-fix.ini arland-fix-launcher.exe arland-fix )
rm -rf "$stage"

echo
echo "  packaged out/$(basename "$archive")"
