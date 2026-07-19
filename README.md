# Atelier Arland Menu Fix

A minimal `d3d11.dll` proxy that removes the severe English menu hitches in Atelier Rorona DX, Atelier Totori DX, and Atelier Meruru DX.

The games repeatedly validate the same immutable `.PSSG` resource paths while building menus. Rorona and Meruru perform a metadata lookup and filename-case directory scan on every successful lookup; Totori repeatedly performs the metadata lookup. This proxy caches only successful validation results, keyed by the complete path. Failed checks, non-PSSG paths, and actual archive reads are unchanged.

## Install

Build or download `d3d11.dll`, place it beside the game's English executable, and use a native DLL override under Wine/Proton:

```text
WINEDLLOVERRIDES="d3d11=n,b" %command%
```

The fix is enabled by default. Set `ATFIX_NO_PSSG_PATH_CACHE=1` (or `ATFIX_PSSG_PATH_CACHE=0`) to disable it.

Only the exact tested 64-bit English builds are modified:

| Game | Executable SHA-256 | Hook RVA |
|---|---|---:|
| Rorona | `2afd19db0cef3e3f0888fb62e02c9ca5929264ff5ee8c780af06213642988276` | `0x12cc70` |
| Totori | `38c41df799b207786a11c08d6bf83cec8ac10414757f935311549f74474bfd90` | `0x18b140` |
| Meruru | `d69cad45700457128cc8805ea3cf80dfaea0e155e6dfd2d1123277f4ebd7c19b` | `0x1533c0` |

The executable name, `.text` size, and function prologue must all match. Unknown builds are passed through without modification.

## Build

On Windows, install Visual Studio 2022 with the Desktop development with C++ workload, Python, Meson, and Ninja. Then use an x64 Native Tools Developer PowerShell:

```powershell
meson setup build --buildtype release
meson compile -C build
```

On Fedora/Linux with MinGW, Meson, and Ninja:

```sh
meson setup build --cross-file build-win64.txt --buildtype release
ninja -C build
```

The output is `build/d3d11.dll`. GitHub Actions builds the same DLL and uploads it as a workflow artifact.
