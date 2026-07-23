# Atelier Arland Fixes

This mod significantly improves performance in the Steam releases of **Atelier Rorona DX, Atelier Totori DX, and Atelier Meruru DX**, in every supported language. It removes severe menu hitches, reduces costly D3D11 synchronization stalls, prevents text corruption caused by the synchronization optimization, and adds game-side 2560×1440 and 3840×2160 rendering support.

The mod ships with a 64-bit `d3d11.dll` for the games and a 32-bit `msimg32.dll` for their shared settings launcher. The game DLL combines the synchronization fix required by the Arland ports with the Arland-specific menu fixes discovered during this project. The launcher DLL always exposes 1920×1080, 2560×1440, and 3840×2160 in both windowed and fullscreen mode despite DPI, desktop-resolution, and display-mode filtering. 

> [!TIP]
> No separate `atelier-sync-fix` or `dinput8.dll` is required. For newer Atelier games, use the upstream [atelier-sync-fix](https://github.com/doitsujin/atelier-sync-fix) or an appropriate maintained fork instead. This project deliberately contains only Arland-specific code.

## How it works

Everything above works out of the box with default settings by simply dropping `d3d11.dll` into your game folders. Optional, off-by-default enhancements (higher-resolution shadows, restored cut-in shadows, cut-in brightness) are documented in [ADVANCED.md](ADVANCED.md).

The mod is intended for the Steam versions of the games. See [TECHNICAL.md](TECHNICAL.md) for implementation details and tested executable fingerprints.

## What is included

The tables below tracks which enhancements have been validated in each game; the
details are in the sections that follow. A ✓ marks a feature confirmed working in
that game.

### Bug fixes and basic enhancements

These are on by default.

| Fix                                          | Rorona | Totori | Meruru |
| -------------------------------------------- | :----: | :----: | :----: |
| Much faster menus (removed stutter)          | ✓      | ✓      | ✓      |
| Frame sync fix                               | ✓      | ✓      | ✓      |
| Text-corruption fix                          | ✓      | ✓      | ✓      |
| Higher resolution rendering                  | ✓      | ✓      | ✓      |
| Restored battle shadows while fighting       | ✓      | ⏳     | —      |
| Conversation slowdown fix                    | —      | —      | ✓      |

✓ fixed, enabled by default · ⏳ planned · — not needed (no defect in that game)

### Advanced graphics tweaks and restored features

These are optional improvements that are opt-in and documented in [ADVANCED.md](ADVANCED.md).

| Enhancement                                  | Rorona | Totori | Meruru |
| -------------------------------------------- | :----: | :----: | :----: |
| MSAA                                         | ✓      | ✓      | ✓      |
| Shadow multiplier (+)                        | ✓      | ✓      | ✓      |
| Cut-in shadows (*)                           | ✓      | TODO   | ✓      |
| Cut-in brightness adjustment                 | ✓      | TODO   | ✓      |

(+) Tends to cause crashes, probably worth keeping disabled.<br/>
(*) Some visual glitches may occur. See TODO for investigation into this issue, which is likely related to positioning of characters.

## Installation on Windows

1. Open the game's installation directory from Steam by selecting **Manage → Browse local files**.
2. Copy `d3d11.dll` and `msimg32.dll` into that directory, beside the game executables and `ArlandDXEnv.exe`.
3. Launch the game normally through Steam.

Remove older copies of this mod's `dinput8.dll` and `winmm.dll`, and do not install another `d3d11.dll` wrapper alongside it. The game loads `d3d11.dll`; the settings launcher loads `msimg32.dll` and makes 1920×1080, 2560×1440, and 3840×2160 available.

All performance and text-correctness fixes are enabled automatically. No configuration is required.

### Wine and Proton

The mod works under Wine and Proton; the required launch options are documented in [ADVANCED.md](ADVANCED.md).

### Advanced options

On first launch, the mod creates `arland-fix.ini` beside the DLLs with everything optional disabled. MSAA, higher-resolution shadow maps, battle shadows, restored cut-in shadows, cut-in brightness, a direct resolution override, a suggested best-experience configuration, and troubleshooting are documented in [ADVANCED.md](ADVANCED.md). The drop-in installation never enables the optional features.

## Build

Build instructions for Windows and Linux are in [BUILDING.md](BUILDING.md).

## Credits

Philip Rebohle created the original [`atelier-sync-fix`](https://github.com/doitsujin/atelier-sync-fix) CPU shadow-copy implementation. TellowKrinkle's [`atelier-sync-fix` fork](https://github.com/TellowKrinkle/atelier-sync-fix) later added Map/Unmap shadow coherence for Ayesha and the old-Arland render-target and viewport/scissor correction ported here; this project replaces the fork's single-map/immediate-upload implementation with per-resource tracking and deferred uploads suitable for the Arland workload. Yuri Hime's [Atelier Graphics Tweak](https://steamcommunity.com/app/1152300/discussions/0/3345546664208090238/) and the earlier [Rorona community investigation](https://steamcommunity.com/app/936160/discussions/0/1742227264210806751/?ctp=2) identified the broader font-atlas transfer problem; AGT used an experimental upload-suppression approach that is not included here. The `.PSSG` validation cache and bounded atlas-read snapshot caches come from the Arland menu-hitch investigation led by Nico, the author of this repository. [MinHook](https://github.com/TsudaKageyu/minhook) is by Tsuda Kageyu and contributors.

## License

See `LICENSE` for the MIT and zlib license terms that apply to the respective source files.
