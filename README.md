# Atelier Arland Fixes

This mod significantly improves performance in the English Steam releases of Atelier Rorona DX, Atelier Totori DX, and Atelier Meruru DX. It removes severe menu hitches, reduces costly D3D11 synchronization stalls, prevents the text corruption caused by earlier synchronization fixes, and adds proper 2560×1440 and 3840×2160 rendering support.

The mod ships as a 64-bit `d3d11.dll` for the games and a 32-bit `winmm.dll` for their shared settings launcher. The game DLL combines the synchronization fix required by the Arland ports with the Arland-specific menu fixes discovered during this project. The launcher DLL exposes all supported resolutions without modifying Koei Tecmo's executable. No separate `atelier-sync-fix` or `dinput8.dll` is required.

For newer Atelier games, use the upstream [atelier-sync-fix](https://github.com/doitsujin/atelier-sync-fix) or an appropriate maintained fork instead. This project deliberately contains only Arland-specific code.

## Benefits

- Much faster opening of text-heavy menus, including the worst-affected Status, Quest, Container, Basket, and crafting screens.
- Fewer pauses caused by the games repeatedly waiting for graphics work to finish.
- Correct text rendering without the corruption produced by older synchronization fixes in these games.
- Correct direct rendering at 2560×1440 and 3840×2160 instead of leaving internal render targets and raster state at 1920×1080.
- One installation covering the relevant performance fixes for the complete Arland trilogy.

The mod is intended for the Steam versions of the games. See [TECHNICAL.md](TECHNICAL.md) for implementation details and tested executable fingerprints.

## Install

Place both `d3d11.dll` and `winmm.dll` in the game directory beside the English executable and `ArlandDXEnv.exe`. Remove older copies of this mod's `dinput8.dll` and do not install another `d3d11.dll` wrapper alongside it. The 64-bit game loads only `d3d11.dll`; the 32-bit launcher loads only `winmm.dll`.

On Wine or Proton, use:

```text
WINEDLLOVERRIDES="d3d11,winmm=n,b" %command%
```

The fixes are enabled by default. `ARLAND_MENU_FIX=0` disables the executable-specific menu hooks while retaining D3D11 forwarding and synchronization. `ARLAND_ATLAS_CACHE=0` disables the queue-scoped atlas-read cache.

## Build

On Windows, install Visual Studio 2022 with the Desktop development with C++ workload, Python, Meson, and Ninja. Build the game DLL from an x64 Native Tools Developer PowerShell:

```powershell
meson setup build64 --buildtype release
meson compile -C build64
```

Then build the launcher DLL from an x86 Native Tools Developer PowerShell:

```powershell
meson setup build32 --buildtype release
meson compile -C build32
```

On Fedora or another Linux distribution with MinGW, Meson, and Ninja:

```sh
meson setup build64 --cross-file build-win64.txt --buildtype release
ninja -C build64
meson setup build32 --cross-file build-win32.txt --buildtype release
ninja -C build32
```

The outputs are `build64/d3d11.dll` and `build32/winmm.dll`. GitHub Actions produces both as workflow artifacts and release assets.

## Credits

Philip Rebohle created the original [`atelier-sync-fix`](https://github.com/doitsujin/atelier-sync-fix) CPU shadow-copy implementation. TellowKrinkle's [`atelier-sync-fix` fork](https://github.com/TellowKrinkle/atelier-sync-fix) later added Map/Unmap shadow coherence for Ayesha and the old-Arland render-target and viewport/scissor correction ported here; this project replaces the fork's single-map/immediate-upload implementation with per-resource tracking and deferred uploads suitable for the Arland workload. The `.PSSG` validation and per-menu atlas-read fixes come from the Arland menu-hitch investigation led by Nico, the author of this repository. The launcher DLL is original work based on that project's Arland launcher analysis. [MinHook](https://github.com/TsudaKageyu/minhook) is by Tsuda Kageyu and contributors.

## License

See `LICENSE` for the MIT and zlib license terms that apply to the respective source files.
