# Atelier Arland Fixes

This mod significantly improves performance in the Steam releases of **Atelier Rorona DX, Atelier Totori DX, and Atelier Meruru DX**, in every supported language. It removes severe menu hitches, reduces costly D3D11 synchronization stalls, prevents text corruption caused by the synchronization optimization, and adds game-side 2560×1440 and 3840×2160 rendering support.

> [!IMPORTANT]
> **Public testing release:** This mod has received substantial testing across all three games and is already suitable for most players who want a better Arland DX experience, but it has not yet reached 1.0. Broader testing, especially on Windows and a wider range of hardware, is still needed. Please [report any problems](https://github.com/nicoverbruggen/atelier-arland-fixes/discussions/new?category=bug-reports) and include `arland-fix.log` and your settings.

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
| Correct world-map analog cursor speed         | —      | ✓      | ✓      |
| Restored battle shadows while fighting       | ✓      | —      | —      |
| Conversation slowdown fix                    | —      | —      | ✓      |

✓ fixed, enabled by default · — not needed or not applicable

**High refresh rates work properly.** The games were written around 60 fps and misbehave above it: on a 120 Hz or 144 Hz display the field-map character buzzes vertically whenever it stands still on a step or ledge, which is visible in the world and can make an interaction prompt flicker, and past a few hundred frames per second the character stops being able to walk at all. Totori and Meruru also move their freely controlled world-map cursor once per rendered frame, making the analog stick increasingly fast above 60 fps. The mod corrects these timing assumptions rather than working around them. Rorona's world map uses a separate discrete location selector; direct measurement found its selection cadence unchanged between 144 and 60 fps, so it needs no corresponding correction. There is no frame-rate limit and nothing about how the game presents frames is changed, so the game simply runs at your display's refresh rate. Under Proton a limit set by Steam or the compositor is respected as usual. Nothing to configure.

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

If the game crashes, the mod appends a report to `arland-fix.log` that helps pinpoint the cause. Include that file and your settings when reporting the problem.

## Safety

I have done my best to make the mod as safe as possible. Here's what you should know:

### Policy #1: Keep the original game files untouched

Like other DLL-based game fixes, this mod is loaded by the game and changes how parts of it work while it is running. It does not permanently patch the games: the changes disappear when you close the game, and the original executables and game assets are never edited.

### Policy #2: Safety checks and easy removal

In practical terms:

- The mod checks that it recognizes the exact game version before applying a fix. If something does not match what it expects, that fix is skipped.
- It does not read or write your save files, collect usage data, update itself or connect to the internet in the background.
- The only files it normally creates or updates are its settings and diagnostic logs. Changing options in the settings launcher also updates the game's own settings file.
- **Play without the mod** starts the game with the fixes disabled. Removing or renaming the mod's DLL files returns the game to normal the next time it starts.

Like any DLL mod, this is executable code that runs with the same access as the game. Download it only from this repository's [official releases](https://github.com/nicoverbruggen/atelier-arland-fixes/releases), or build it from source. The exact technical safeguards are documented in [TECHNICAL.md](TECHNICAL.md#runtime-memory-manipulation).

### Policy #3: Public source and build process

The complete source code and the steps GitHub uses to build each release are public. You can also make your own build by following [BUILDING.md](BUILDING.md). 

> [!TIP]
> If you do not read code yourself, an LLM (like ChatGPT or Claude) can help review the repository for obvious red flags and explain whether it appears to do what this page claims. That can be a useful second opinion, but it is not a guarantee and cannot prove that a downloaded file was built from the published source. You can ask it to inspect the built files, however this may use up a lot of tokens.

## Installation on Windows

1. Open the game's installation directory from Steam by selecting **Manage → Browse local files**.
2. Copy `d3d11.dll`, `arland-fix-launcher.exe`, `msimg32.dll` and `arland-fix.ini` into that directory, beside the game executables and `ArlandDXEnv.exe`. If you are updating an existing install, keep the `arland-fix.ini` you already have — the bundled one is only the defaults.
3. Launch the game normally through Steam.

All performance and text-correctness fixes are enabled automatically. No configuration is required.

### Wine and Proton

Copy the files in exactly as above, then add this to the game's Steam Launch Options (Properties → General → Launch Options):

```text
WINEDLLOVERRIDES="d3d11,msimg32=n,b" %command%
```

Without it Wine loads its own `d3d11` and ignores the mod's, so the files sit in the folder doing nothing and the game looks entirely unmodified. If nothing seems to have changed after installing, check this first. `arland-fix.log` appearing in the game directory is the sign the mod loaded.

When the multilingual build runs in Japanese on a non-UTF-8 system locale (common under Wine and Proton), its title bar would otherwise show mojibake, and the engine's ANSI window cannot display Japanese on such a locale at all. In that case the mod substitutes a readable romanized title, for example `Rorona no Atelier ~Arland no Renkinjutsushi~ DX`. The Chinese and English builds, and correctly configured Japanese or UTF-8 locales, are unaffected.

### Advanced options

The release archive includes an `arland-fix.ini` listing every option with its default and a short explanation; if none is present the mod creates a minimal one on first launch. The costly options are disabled by default; the ones that are free (SMAA, anisotropic filtering, the high-resolution UI text) are on. MSAA, supersampling, higher-resolution shadow maps, battle shadows, restored cut-in shadows, cut-in brightness, the resolution settings, a suggested best-experience configuration, and troubleshooting are documented in [ADVANCED.md](ADVANCED.md). The drop-in installation never enables the optional features.

## Build

Build instructions for Windows and Linux are in [BUILDING.md](BUILDING.md).

## Credits

Philip Rebohle created the original [`atelier-sync-fix`](https://github.com/doitsujin/atelier-sync-fix) CPU shadow-copy implementation. TellowKrinkle's [`atelier-sync-fix` fork](https://github.com/TellowKrinkle/atelier-sync-fix) later added Map/Unmap shadow coherence for Ayesha and the old-Arland render-target and viewport/scissor correction ported here; this project replaces the fork's single-map/immediate-upload implementation with per-resource tracking and deferred uploads suitable for the Arland workload. Yuri Hime's [Atelier Graphics Tweak](https://steamcommunity.com/app/1152300/discussions/0/3345546664208090238/) and the earlier [Rorona community investigation](https://steamcommunity.com/app/936160/discussions/0/1742227264210806751/?ctp=2) identified the broader font-atlas transfer problem; AGT used an experimental upload-suppression approach that is not included here.

The bundled SMAA anti-aliasing is by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, and Diego Gutierrez ([SMAA](https://github.com/iryoku/smaa), MIT), vendored unchanged; AGT shipped the same SMAA for these games. The high-resolution UI text is rasterized with [stb_truetype](https://github.com/nothings/stb) by Sean Barrett (public domain). One replacement typeface is embedded per game: [National Park](https://nationalparktypeface.com/) SemiBold (Rorona) and [Nunito](https://fonts.google.com/specimen/Nunito) Regular (Totori), both under the SIL Open Font License, and Cosmetica Medium (Meruru), an emboldened [MgOpen](https://www.ellak.gr/fonts/mgopen/index.html) Cosmetica under the MgOpen licence and renamed as that licence requires. Copyright notices and the full licence texts are in [licenses/](licenses/), and ship with each release. [MinHook](https://github.com/TsudaKageyu/minhook) is by Tsuda Kageyu and contributors.

See [TECHNICAL.md](TECHNICAL.md) for the full implementation details and provenance.

The menu-hitch research that made this project possible was mine, and I (Nico) did the reverse engineering and integration that combine all of the above plus more into a single mod, using large language models from OpenAI and Anthropic throughout to analyze the games, develop the fixes, and bundle the improvements together.

## License

See `LICENSE` for the MIT and zlib license terms that apply to the respective source files.
