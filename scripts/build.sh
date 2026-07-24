#!/usr/bin/env bash
# Build on Windows. Run in Git Bash / MSYS2 from a Native Tools Developer prompt
# (MSVC on PATH). Builds natively with meson into build_<arch>/.
#
# One developer prompt targets one architecture, so run this from an x64 prompt
# (produces d3d11.dll) and again from an x86 prompt (produces msimg32.dll); CI
# does both across two jobs. Optional first argument: the meson build type
# (default: release).
set -euo pipefail
cd "$(dirname "$0")/.."

buildtype="${1:-release}"
arch="${VSCMD_ARG_TGT_ARCH:-x64}"
builddir="build_${arch}"

meson setup "$builddir" --buildtype "$buildtype" --reconfigure >/dev/null 2>&1 \
  || meson setup "$builddir" --buildtype "$buildtype"
meson compile -C "$builddir"

echo "Built ${arch} into ${builddir}/."
