# Atelier Arland Fixes

This mod significantly improves performance in the English Steam releases of Atelier Rorona DX, Atelier Totori DX, and Atelier Meruru DX. It removes severe menu hitches, reduces costly D3D11 synchronization stalls, and prevents the text corruption caused by earlier synchronization fixes.

The mod ships as one `d3d11.dll` combining the synchronization fix required by the Arland ports with the Arland-specific menu fixes discovered during this project. No separate `atelier-sync-fix` or `dinput8.dll` is required.

For newer Atelier games, use the upstream [atelier-sync-fix](https://github.com/doitsujin/atelier-sync-fix) or an appropriate maintained fork instead. This project deliberately contains only Arland-specific code.

## Benefits

- Much faster opening of text-heavy menus, including the worst-affected Status, Quest, Container, Basket, and crafting screens.
- Fewer pauses caused by the games repeatedly waiting for graphics work to finish.
- Correct text rendering without the corruption produced by older synchronization fixes in these games.
- One installation covering the relevant performance fixes for the complete Arland trilogy.

The mod is intended for the Steam versions of the games. See [TECHNICAL.md](TECHNICAL.md) for implementation details and tested executable fingerprints.

## Install

Place `d3d11.dll` beside the game's English executable. Remove older copies of this mod's `dinput8.dll` and do not install another `d3d11.dll` wrapper alongside it.

On Wine or Proton, use:

```text
WINEDLLOVERRIDES="d3d11=n,b" %command%
```

The fixes are enabled by default. `ARLAND_MENU_FIX=0` disables the executable-specific menu hooks while retaining D3D11 forwarding and synchronization. `ARLAND_ATLAS_CACHE=0` disables the queue-scoped atlas-read cache.

## Build

On Windows, install Visual Studio 2022 with the Desktop development with C++ workload, Python, Meson, and Ninja. From an x64 Native Tools Developer PowerShell:

```powershell
meson setup build --buildtype release
meson compile -C build
```

On Fedora or another Linux distribution with MinGW, Meson, and Ninja:

```sh
meson setup build --cross-file build-win64.txt --buildtype release
ninja -C build
```

The output is `build/d3d11.dll`. GitHub Actions produces the same DLL as a workflow artifact.

## Credits

Philip Rebohle created the original [`atelier-sync-fix`](https://github.com/doitsujin/atelier-sync-fix) CPU shadow-copy implementation. TellowKrinkle's [`atelier-sync-fix` fork](https://github.com/TellowKrinkle/atelier-sync-fix) later added Map/Unmap shadow coherence for Ayesha; this project replaces its single-map/immediate-upload implementation with per-resource tracking and deferred uploads suitable for the Arland workload. The `.PSSG` validation and per-menu atlas-read fixes come from the Arland menu-hitch investigation led by Nico, the author of this repository. [MinHook](https://github.com/TsudaKageyu/minhook) is by Tsuda Kageyu and contributors.

## License

See `LICENSE` for the MIT and zlib license terms that apply to the respective source files.
