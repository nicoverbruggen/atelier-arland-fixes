# Atelier Arland Fixes

This mod significantly improves performance in the Steam releases of **Atelier Rorona DX, Atelier Totori DX, and Atelier Meruru DX**, in every supported language. It removes severe menu hitches, reduces costly D3D11 synchronization stalls, prevents text corruption caused by the synchronization optimization, and adds game-side 2560×1440 and 3840×2160 rendering support.

The mod ships with a 64-bit `d3d11.dll` for the games, a 64-bit `arland-fix-launcher.exe`, and a 32-bit `msimg32.dll` for the two front-ends the games share. The game DLL combines the synchronization fix required by the Arland ports with the Arland-specific menu fixes discovered during this project. `arland-fix-launcher.exe` puts every setting in one window and starts the game from it. `msimg32.dll` is loaded by both of Koei Tecmo's front-ends: starting the game opens the launcher above instead, and their own settings editor gains 1920×1080, 2560×1440 and 3840×2160 in both windowed and fullscreen mode despite DPI, desktop-resolution, and display-mode filtering. Both of the original tools remain reachable from the launcher.

> [!TIP]
> No separate `atelier-sync-fix` or `dinput8.dll` is required. For newer Atelier games, use the upstream [atelier-sync-fix](https://github.com/doitsujin/atelier-sync-fix) or an appropriate maintained fork instead. This project deliberately contains only Arland-specific code.

## How it works

Everything above works out of the box with default settings by simply dropping `d3d11.dll` into your game folders. Anisotropic filtering is on by default too, since it costs nothing per frame. Optional, off-by-default enhancements (supersampling, MSAA, higher-resolution shadows, borderless windowed mode) are documented in [ADVANCED.md](ADVANCED.md).

The mod is intended for the Steam versions of the games. See [TECHNICAL.md](TECHNICAL.md) for implementation details and tested executable fingerprints.

## What is included

The tables below tracks which enhancements have been validated in each game; the details are in the sections that follow. A ✓ marks a feature confirmed working in that game.

### Bug fixes and basic enhancements

These are on by default.

| Fix                                          | Rorona | Totori | Meruru |
| -------------------------------------------- | :----: | :----: | :----: |
| Much faster menus (removed stutter)          | ✓      | ✓      | ✓      |
| Frame sync fix                               | ✓      | ✓      | ✓      |
| Text-corruption fix                          | ✓      | ✓      | ✓      |
| Higher resolution rendering                  | ✓      | ✓      | ✓      |
| High-resolution UI text                      | ✓      | ✓      | ✓      |
| SMAA anti-aliasing                            | ✓      | ✓      | ✓      |
| Correct behaviour at high refresh rates       | ✓      | ✓      | ✓      |
| Restored battle shadows while fighting       | ✓      | —      | —      |
| Conversation slowdown fix                    | —      | —      | ✓      |

✓ fixed, enabled by default · — not needed (no defect in that game)

**High refresh rates work properly.** The games were written around 60 fps and misbehave above it: on a 120 Hz or 144 Hz display the field-map character buzzes vertically whenever it stands still on a step or ledge, which is visible in the world and can make an interaction prompt flicker, and past a few hundred frames per second the character stops being able to walk at all. Both come from the same place, an assumption that a frame is always 1/60 of a second, and the mod corrects it rather than working around it. There is no frame-rate limit and nothing about how the game presents frames is changed, so the game simply runs at your display's refresh rate. Under Proton a limit set by Steam or the compositor is respected as usual. Nothing to configure.

SMAA anti-aliasing and the high-resolution UI text are on by default; both can be turned off in `arland-fix.ini`. The UI text is re-rendered from a bundled scalable font; this applies to the English executables only, and the Japanese and Chinese builds keep their original font untouched.

The battle cut-in shadow and brightness restorations are off by default; enable them in `arland-fix.ini` if you prefer restored shadows and full-brightness close-ups over the vanilla darkened, shadowless ones.

### Advanced graphics tweaks

These are optional improvements that are off by default and documented in [ADVANCED.md](ADVANCED.md).

| Enhancement                                  | Rorona | Totori | Meruru |
| -------------------------------------------- | :----: | :----: | :----: |
| Supersampling (internal render resolution)   | ✓      | ✓      | ✓      |
| MSAA                                         | ✓      | ✓      | ✓      |
| Anisotropic filtering                        | ✓      | ✓      | ✓      |
| Shadow multiplier                            | ✓      | ✓      | ✓      |
| Borderless windowed mode                     | ✓      | ✓      | ✓      |
| Restored battle cut-in shadows               | ✓      | ✓      | ✓      |
| Cut-in scene kept at full brightness         | ✓      | ✓      | ✓      |

Rare crashes have been observed during long sessions with the advanced graphics tweaks enabled at their highest settings; no individual feature has been confirmed as the cause. If you hit instability, lower or disable them — crashes append a report to `arland-fix.log` that helps pinpoint the cause.

## Installation on Windows

1. Open the game's installation directory from Steam by selecting **Manage → Browse local files**.
2. Copy `d3d11.dll`, `arland-fix-launcher.exe`, `msimg32.dll` and `arland-fix.ini` into that directory, beside the game executables and `ArlandDXEnv.exe`. If you are updating an existing install, keep the `arland-fix.ini` you already have — the bundled one is only the defaults.
3. Launch the game normally through Steam.

All performance and text-correctness fixes are enabled automatically. No configuration is required.

### Wine and Proton

The mod works under Wine and Proton; the required launch options are documented in [ADVANCED.md](ADVANCED.md).

When the multilingual build runs in Japanese on a non-UTF-8 system locale (common under Wine and Proton), its title bar would otherwise show mojibake, and the engine's ANSI window cannot display Japanese on such a locale at all. In that case the mod substitutes a readable romanized title, for example `Rorona no Atelier ~Arland no Renkinjutsushi~ DX`. The Chinese and English builds, and correctly configured Japanese or UTF-8 locales, are unaffected.

### Advanced options

The release archive includes an `arland-fix.ini` listing every option with its default and a short explanation; if none is present the mod creates a minimal one on first launch. The costly options are disabled by default; the ones that are free (SMAA, anisotropic filtering, the high-resolution UI text) are on. MSAA, supersampling, higher-resolution shadow maps, battle shadows, restored cut-in shadows, cut-in brightness, the resolution settings, a suggested best-experience configuration, and troubleshooting are documented in [ADVANCED.md](ADVANCED.md). The drop-in installation never enables the optional features.

## Build

Build instructions for Windows and Linux are in [BUILDING.md](BUILDING.md).

## Credits

Philip Rebohle created the original [`atelier-sync-fix`](https://github.com/doitsujin/atelier-sync-fix) CPU shadow-copy implementation. TellowKrinkle's [`atelier-sync-fix` fork](https://github.com/TellowKrinkle/atelier-sync-fix) later added Map/Unmap shadow coherence for Ayesha and the old-Arland render-target and viewport/scissor correction ported here; this project replaces the fork's single-map/immediate-upload implementation with per-resource tracking and deferred uploads suitable for the Arland workload. Yuri Hime's [Atelier Graphics Tweak](https://steamcommunity.com/app/1152300/discussions/0/3345546664208090238/) and the earlier [Rorona community investigation](https://steamcommunity.com/app/936160/discussions/0/1742227264210806751/?ctp=2) identified the broader font-atlas transfer problem; AGT used an experimental upload-suppression approach that is not included here.

The bundled SMAA anti-aliasing is by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, and Diego Gutierrez ([SMAA](https://github.com/iryoku/smaa), MIT), vendored unchanged; AGT shipped the same SMAA for these games. The high-resolution UI text is rasterized with [stb_truetype](https://github.com/nothings/stb) by Sean Barrett (public domain). One replacement typeface is embedded per game: [National Park](https://nationalparktypeface.com/) SemiBold (Rorona) and [Nunito](https://fonts.google.com/specimen/Nunito) Regular (Totori), both under the SIL Open Font License, and Cosmetica Medium (Meruru), an emboldened [MgOpen](https://www.ellak.gr/fonts/mgopen/index.html) Cosmetica under the MgOpen licence and renamed as that licence requires. Copyright notices and the full licence texts are in [licenses/](licenses/), and ship with each release. [MinHook](https://github.com/TsudaKageyu/minhook) is by Tsuda Kageyu and contributors.

See [TECHNICAL.md](TECHNICAL.md) for the full implementation details and provenance.

The menu-hitch research that made this project possible was mine, and I (Nico) did the reverse engineering and integration that combine all of the above into a single mod, using large language models from OpenAI and Anthropic throughout to analyze the games, develop the fixes, and bundle the improvements together.

## License

See `LICENSE` for the MIT and zlib license terms that apply to the respective source files.
