#!/usr/bin/env bash
# Build on Windows. Run in Git Bash / MSYS2 from a Native Tools Developer prompt
# (MSVC on PATH). Builds natively with meson into build_<arch>/.
#
# One developer prompt targets one architecture, so run this from an x64 prompt
# (produces d3d11.dll and arland-fix-launcher.exe) and again from an x86 prompt
# (produces msimg32.dll); CI does both across two jobs. Optional first argument:
# the meson build type (default: release).
set -euo pipefail
cd "$(dirname "$0")/.."

buildtype="${1:-release}"
arch="${VSCMD_ARG_TGT_ARCH:-x64}"
builddir="build_${arch}"

meson setup "$builddir" --buildtype "$buildtype" --reconfigure >/dev/null 2>&1 \
  || meson setup "$builddir" --buildtype "$buildtype"
meson compile -C "$builddir"

# The compile above builds every target for this architecture; this only
# confirms each one landed, so a silently dropped target cannot pass for a
# good build. The GUI is 64-bit only.
if [[ "$arch" == "x64" ]]; then
  expected=("d3d11.dll" "arland-fix-launcher.exe")
else
  expected=("msimg32.dll")
fi
status=0
for artifact in "${expected[@]}"; do
  if [[ -f "$builddir/$artifact" ]]; then
    echo "  ok      $builddir/$artifact"
  else
    echo "  MISSING $builddir/$artifact" >&2
    status=1
  fi
done

if [[ $status -eq 0 ]]; then
  if [[ "$arch" == "x64" ]]; then
    python scripts/check_exports.py "$builddir/d3d11.dll"
  else
    python scripts/check_exports.py "$builddir/msimg32.dll"
  fi
fi

echo "Built ${arch} into ${builddir}/."
exit "$status"
