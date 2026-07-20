# Atelier Arland Fixes

This mod significantly improves performance in the English Steam releases of **Atelier Rorona DX, Atelier Totori DX, and Atelier Meruru DX**. It removes severe menu hitches, reduces costly D3D11 synchronization stalls, prevents text corruption caused by the synchronization optimization, and adds game-side 2560×1440 and 3840×2160 rendering support.

The mod ships with a 64-bit `d3d11.dll` for the games and a 32-bit `msimg32.dll` for their shared settings launcher. The game DLL combines the synchronization fix required by the Arland ports with the Arland-specific menu fixes discovered during this project. The launcher DLL always exposes 1920×1080, 2560×1440, and 3840×2160 in both windowed and fullscreen mode despite DPI, desktop-resolution, and display-mode filtering. No separate `atelier-sync-fix` or `dinput8.dll` is required.

For newer Atelier games, use the upstream [atelier-sync-fix](https://github.com/doitsujin/atelier-sync-fix) or an appropriate maintained fork instead. This project deliberately contains only Arland-specific code.

## Benefits

- Much faster opening of text-heavy menus, including the worst-affected Status, Quest, Container, Basket, Assignment, and crafting screens.
- Fewer pauses caused by the games repeatedly waiting for graphics work to finish.
- Correct text rendering without the corruption produced by older synchronization fixes in these games.
- Correct direct rendering at 2560×1440 and 3840×2160 instead of leaving internal render targets and raster state at 1920×1080.
- One installation covering the relevant performance fixes for the complete Arland trilogy.

The mod is intended for the Steam versions of the games. See [TECHNICAL.md](TECHNICAL.md) for implementation details and tested executable fingerprints.

## Installation on Windows

1. Open the game's installation directory from Steam by selecting **Manage → Browse local files**.
2. Copy `d3d11.dll` and `msimg32.dll` into that directory, beside the English game executable and `ArlandDXEnv.exe`.
3. Launch the game normally through Steam.

Remove older copies of this mod's `dinput8.dll` and `winmm.dll`, and do not install another `d3d11.dll` wrapper alongside it. The game loads `d3d11.dll`; the settings launcher loads `msimg32.dll` and makes 1920×1080, 2560×1440, and 3840×2160 available.

All performance and text-correctness fixes are enabled automatically. No configuration is required.

### Optional MSAA on Windows

Multisample anti-aliasing is disabled by default. Use `2`, `4`, or `8` to request that sample count. If the GPU or selected format does not support the requested count, the mod falls back to a lower supported count. Use `0` or `1`, or remove the setting entirely, to disable MSAA. Higher sample counts increase GPU and video-memory cost.

On first launch, the mod creates `arland-fix.ini` beside the DLLs with MSAA disabled. To enable it, close the game and change the file to:

```ini
[Rendering]
MSAA=4
Width=
Height=
```

## Advanced use

### Wine and Proton

Copy both DLLs into the game directory as described above, then add this to the game's Steam Launch Options:

```text
WINEDLLOVERRIDES="d3d11,msimg32=n,b" %command%
```

The same `arland-fix.ini` file used on Windows configures MSAA under Wine or Proton. Alternatively, set MSAA directly in the launch options:

```text
ARLAND_MSAA=4 WINEDLLOVERRIDES="d3d11,msimg32=n,b" %command%
```

`ARLAND_MSAA` takes precedence over `arland-fix.ini` when both are present. Runtime messages are written to `arland-fix.log` in the game directory.

### Direct resolution override

The launcher DLL always exposes 1920×1080, 2560×1440, and 3840×2160 in both launcher states even when DPI or display-mode enumeration would normally hide them. Keeping 1920×1080 visible is intentional for Steam Deck and other lower-resolution handhelds, where a high-DPI desktop can otherwise prevent the launcher from exposing 1080p; it also supports higher-resolution rendering for downsampling and normal docked use. As a launcher-independent fallback, `arland-fix.ini` can also override the resolution used by the game. Both values must be present; blank values leave the launcher's selection unchanged:

```ini
[Rendering]
MSAA=1
Width=3840
Height=2160
```

### Troubleshooting switches

The fixes are enabled by default. `ARLAND_MENU_FIX=0` disables the executable-specific menu hooks while retaining D3D11 forwarding and synchronization. `ARLAND_ATLAS_CACHE=0` disables atlas-read caching. Rorona also accepts `ARLAND_FRAME_ATLAS_CACHE=0` to restrict snapshots to the older queue-scoped behavior. These switches are intended for diagnosis and A/B testing, not normal installation.

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

The outputs are `build64/d3d11.dll` and `build32/msimg32.dll`. GitHub Actions produces both DLLs as workflow artifacts and tagged release assets.

## Known limitations

Blurred 3D backgrounds used during dialogue scenes cover the full output at higher resolutions. Rorona's additional fixed-size snapshot and composite assumptions are corrected without affecting portraits or other cutscene layers. Totori and Meruru use different blur paths that already follow the resized targets. Optional MSAA has been validated at 2560×1440 in all three games; leave it disabled if you encounter rendering problems on other hardware or drivers.

## Credits

Philip Rebohle created the original [`atelier-sync-fix`](https://github.com/doitsujin/atelier-sync-fix) CPU shadow-copy implementation. TellowKrinkle's [`atelier-sync-fix` fork](https://github.com/TellowKrinkle/atelier-sync-fix) later added Map/Unmap shadow coherence for Ayesha and the old-Arland render-target and viewport/scissor correction ported here; this project replaces the fork's single-map/immediate-upload implementation with per-resource tracking and deferred uploads suitable for the Arland workload. The `.PSSG` validation and per-menu atlas-read fixes come from the Arland menu-hitch investigation led by Nico, the author of this repository. [MinHook](https://github.com/TsudaKageyu/minhook) is by Tsuda Kageyu and contributors.

## License

See `LICENSE` for the MIT and zlib license terms that apply to the respective source files.
