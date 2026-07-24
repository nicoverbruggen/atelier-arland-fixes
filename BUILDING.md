# Building

The mod builds with Meson and Ninja on Windows (MSVC) or Linux (MinGW cross
build). The outputs are `build64/d3d11.dll` (the game DLL) and
`build32/msimg32.dll` (the settings-launcher DLL).

Two convenience scripts wrap the steps below. `scripts/build_linux.sh` cross-
compiles both DLLs with MinGW inside the build container (the day-to-day Linux
flow; set `$ATFIX_CONTAINER` to use a different container name). `scripts/build.sh`
builds natively on Windows from a Native Tools prompt. Both take an optional build
type (default `release`).

## Windows

Install Visual Studio 2022 with the Desktop development with C++ workload,
Python, Meson, and Ninja. Build the game DLL from an x64 Native Tools
Developer PowerShell:

```powershell
meson setup build64 --buildtype release
meson compile -C build64
```

Then build the launcher DLL from an x86 Native Tools Developer PowerShell:

```powershell
meson setup build32 --buildtype release
meson compile -C build32
```

## Linux

On Fedora or another Linux distribution with MinGW, Meson, and Ninja:

```sh
meson setup build64 --cross-file build-win64.txt --buildtype release
ninja -C build64
meson setup build32 --cross-file build-win32.txt --buildtype release
ninja -C build32
```

## Continuous integration

GitHub Actions builds both DLLs with MSVC (one job per architecture) and
publishes them as workflow artifacts and tagged release assets. MSVC output is
used for releases because it is less likely to trip antivirus ML heuristics than
the MinGW cross-build.
