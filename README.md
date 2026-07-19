# Atelier Arland Menu Fix

A minimal `dinput8.dll` proxy that removes the severe English menu hitches in Atelier Rorona DX, Atelier Totori DX, and Atelier Meruru DX. It is designed to coexist with TellowKrinkle's [`atelier-sophie-20231022` atelier-sync-fix release](https://github.com/TellowKrinkle/atelier-sync-fix/releases/tag/atelier-sophie-20231022), which remains installed as `d3d11.dll` while this fix is installed as `dinput8.dll`.

A unified version that integrates both mods into a single `d3d11.dll` is also in development.

The games repeatedly validate the same immutable `.PSSG` resource paths while building menus. Rorona and Meruru perform a metadata lookup and filename-case directory scan on every successful lookup; Totori repeatedly performs the metadata lookup. This proxy caches only successful validation results, keyed by the complete path. Failed checks, non-PSSG paths, and actual archive reads are unchanged.

## Install

Place TellowKrinkle's `d3d11.dll` and this project's `dinput8.dll` beside the game's English executable, then enable both native DLL overrides under Wine/Proton:

```text
WINEDLLOVERRIDES="d3d11=n,b;dinput8=n,b" %command%
```

The fix is enabled by default. Set `ARLAND_MENU_FIX=0` to disable it. This setting is intentionally separate from `atelier-sync-fix` configuration so each DLL can be controlled independently.

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

The output is `build/dinput8.dll`. GitHub Actions builds the same DLL and uploads it as a workflow artifact.
