# Atelier Arland Fixes

This mod significantly improves performance in the Steam releases of **Atelier Rorona DX, Atelier Totori DX, and Atelier Meruru DX**, in every supported language. It removes severe menu hitches, reduces costly D3D11 synchronization stalls, prevents text corruption caused by the synchronization optimization, and adds game-side 2560×1440 and 3840×2160 rendering support.

The mod ships with a 64-bit `d3d11.dll` for the games and a 32-bit `msimg32.dll` for their shared settings launcher. The game DLL combines the synchronization fix required by the Arland ports with the Arland-specific menu fixes discovered during this project. The launcher DLL always exposes 1920×1080, 2560×1440, and 3840×2160 in both windowed and fullscreen mode despite DPI, desktop-resolution, and display-mode filtering. 

> [!TIP]
> No separate `atelier-sync-fix` or `dinput8.dll` is required. For newer Atelier games, use the upstream [atelier-sync-fix](https://github.com/doitsujin/atelier-sync-fix) or an appropriate maintained fork instead. This project deliberately contains only Arland-specific code.

## How it works

Everything above works out of the box with default settings by simply dropping `d3d11.dll` into your game folders. Optional, off-by-default enhancements (MSAA, higher-resolution shadows) are documented in [ADVANCED.md](ADVANCED.md).

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
| SMAA anti-aliasing                            | ✓      | ✓      | ✓      |
| Restored battle shadows while fighting       | ✓      | ⏳     | —      |
| Restored battle cut-in shadows               | ✓      | ✓      | ✓      |
| Cut-in scene kept at full brightness         | ✓      | ✓      | ✓      |
| Conversation slowdown fix                    | —      | —      | ✓      |

✓ fixed, enabled by default · ⏳ planned · — not needed (no defect in that game)

SMAA anti-aliasing is on by default and smooths edges across the whole scene at low cost; it can be turned off in `arland-fix.ini`. The cut-in shadow and brightness restorations can also be turned off there if you prefer the vanilla darkened, shadowless close-ups; see [ADVANCED.md](ADVANCED.md).

### Advanced graphics tweaks

These are optional improvements that are off by default and documented in [ADVANCED.md](ADVANCED.md).

| Enhancement                                  | Rorona | Totori | Meruru |
| -------------------------------------------- | :----: | :----: | :----: |
| MSAA                                         | ✓      | ✓      | ✓      |
| Shadow multiplier                            | ✓      | ✓      | ✓      |
| Anisotropic filtering                        | ✓      | ✓      | ✓      |

Rare crashes have been observed during long sessions with the advanced graphics tweaks enabled at their highest settings; no individual feature has been confirmed as the cause. If you hit instability, lower or disable them — crashes append a report to `arland-fix.log` that helps pinpoint the cause.

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

Philip Rebohle created the original [`atelier-sync-fix`](https://github.com/doitsujin/atelier-sync-fix) CPU shadow-copy implementation. TellowKrinkle's [`atelier-sync-fix` fork](https://github.com/TellowKrinkle/atelier-sync-fix) later added Map/Unmap shadow coherence for Ayesha and the old-Arland render-target and viewport/scissor correction ported here; this project replaces the fork's single-map/immediate-upload implementation with per-resource tracking and deferred uploads suitable for the Arland workload. Yuri Hime's [Atelier Graphics Tweak](https://steamcommunity.com/app/1152300/discussions/0/3345546664208090238/) and the earlier [Rorona community investigation](https://steamcommunity.com/app/936160/discussions/0/1742227264210806751/?ctp=2) identified the broader font-atlas transfer problem; AGT used an experimental upload-suppression approach that is not included here.

The bundled SMAA anti-aliasing is by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, and Diego Gutierrez ([SMAA](https://github.com/iryoku/smaa), MIT), vendored unchanged; AGT shipped the same SMAA for these games. The high-resolution UI text is rasterized with [stb_truetype](https://github.com/nothings/stb) by Sean Barrett (public domain), and its bundled replacement typeface is [Cuprum](https://fonts.google.com/specimen/Cuprum) (SIL Open Font License). [MinHook](https://github.com/TsudaKageyu/minhook) is by Tsuda Kageyu and contributors.

See [TECHNICAL.md](TECHNICAL.md) for the full implementation details and provenance.

The menu-hitch research that made this project possible was mine, and I (Nico) did the reverse engineering and integration that combine all of the above into a single mod, using large language models from OpenAI and Anthropic throughout to analyze the games, develop the fixes, and bundle the improvements together.

## License

See `LICENSE` for the MIT and zlib license terms that apply to the respective source files.
