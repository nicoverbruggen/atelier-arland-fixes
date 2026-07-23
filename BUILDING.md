# Building

The mod builds with Meson and Ninja on Windows (MSVC) or Linux (MinGW cross
build). The outputs are `build64/d3d11.dll` (the game DLL) and
`build32/msimg32.dll` (the settings-launcher DLL).

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

GitHub Actions produces both DLLs as workflow artifacts and tagged release
assets.
