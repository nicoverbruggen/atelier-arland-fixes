# Atelier Arland Fixes

This mod significantly improves performance and adds various improvements to the Steam releases of **Atelier Rorona DX, Atelier Totori DX, and Atelier Meruru DX**, in every supported language.

> [!TIP]
> No separate `atelier-sync-fix` or `dinput8.dll` is required. For newer Atelier games (Mysterious games and newer), use the upstream [atelier-sync-fix](https://github.com/doitsujin/atelier-sync-fix) or an appropriate maintained fork instead. This project deliberately contains only Arland-specific code.

## How it works

The mod is intended for the Steam versions of the games. See [TECHNICAL.md](TECHNICAL.md) for implementation details and tested executable fingerprints.

## What is included

### Launcher

The **included launcher** replaces the standard window for the Arland games and gives you more control over your game experience. It also lets you tweak various **graphics settings** that are added by this mod.

### List of fixes and improvements

| Fix                                          | Rorona | Totori | Meruru |
| -------------------------------------------- | :----: | :----: | :----: |
| Much faster menus (removed stutter)          | ✓      | ✓      | ✓      |
| Frame sync fix                               | ✓      | ✓      | ✓      |
| Text-corruption fix                          | ✓      | ✓      | ✓      |
| Higher resolution rendering                  | ✓      | ✓      | ✓      |
| High-resolution UI text                      | ✓      | ✓      | ✓      |
| SMAA anti-aliasing                           | ✓      | ✓      | ✓      |
| Correct behaviour at high refresh rates      | ✓      | ✓      | ✓      |
| Borderless windowed at your desktop size     | ✓      | ✓      | ✓      |
| Correct world-map analog cursor speed        | —      | ✓      | ✓      |
| Restored ordinary-battle shadows             | ✓      | —      | —      |
| Instant save and load menus                  | ✓      | ✓      | ✓      |
| Optional startup logo and intro-movie skip   | ✓      | ✓      | ✓      |
| Various game-specific bug fixes              | ✓      | ✓      | ✓      |
| Local crash logging                          | ✓      | ✓      | ✓      |

If the game crashes, the mod appends a report to `arland-fix.log` that helps pinpoint the cause. Include that file and your settings when reporting the problem, since these logs are never sent anywhere.

### List of graphics enhancements

| Enhancement                                  | Rorona | Totori | Meruru |
| -------------------------------------------- | :----: | :----: | :----: |
| Supersampling (internal render resolution)   | ✓      | ✓      | ✓      |
| Anisotropic filtering                        | ✓      | ✓      | ✓      |
| Shadow multiplier                            | ✓      | ✓      | ✓      |
| Battle cut-in shadows and brightness         | ✓      | ✓      | ✓      |

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

All performance and text-correctness fixes are enabled automatically. No additional configuration is required, but it's recommended that you adjust the graphics settings according to your hardware configuration. The launcher explains what each one costs.

Use it on its own. It is meant to be the only graphics mod in the folder: copy the four files in as listed above and nothing else. Do not rename `d3d11.dll` to anything else, and do not place a second copy of it under another name. Other tools that install themselves as `d3d11.dll` cannot be used at the same time, because only one of them can have that name.

### Wine and Proton

Copy the files in exactly as above, then add this to the game's Steam Launch Options (Properties → General → Launch Options):

```text
WINEDLLOVERRIDES="d3d11,msimg32=n,b" %command%
```

Without it Wine loads its own `d3d11` and ignores the mod's, so the files sit in the folder doing nothing and the game looks entirely unmodified. 

If nothing seems to have changed after installing, check this first. `arland-fix.log` appearing in the game directory is the sign the mod loaded.

### Advanced options

The release archive includes an `arland-fix.ini` listing every option with its default; if none is present the mod creates a minimal one on first launch. You can customize settings by using the mod's launcher.

## Build

Build instructions for Windows and Linux are in [BUILDING.md](BUILDING.md).

## Credits

> [!NOTE]
> The menu-hitch research that made this project possible was mine, and I did the reverse engineering and integration that combine all of the above plus more into a single mod, using large language models from OpenAI and Anthropic throughout to analyze the games, develop the fixes, and bundle the improvements together. I believe that LLMs were used responsibly in this project.

Philip Rebohle created the original [`atelier-sync-fix`](https://github.com/doitsujin/atelier-sync-fix) CPU shadow-copy implementation. TellowKrinkle's [`atelier-sync-fix` fork](https://github.com/TellowKrinkle/atelier-sync-fix) later added Map/Unmap shadow coherence for Ayesha and the old-Arland render-target and viewport/scissor correction ported here; this project replaces the fork's single-map/immediate-upload implementation with per-resource tracking and deferred uploads suitable for the Arland workload. Yuri Hime's [Atelier Graphics Tweak](https://steamcommunity.com/app/1152300/discussions/0/3345546664208090238/) and the earlier [Rorona community investigation](https://steamcommunity.com/app/936160/discussions/0/1742227264210806751/?ctp=2) identified the broader font-atlas transfer problem; AGT used an experimental upload-suppression approach that is not included here.

The bundled SMAA anti-aliasing is by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, and Diego Gutierrez ([SMAA](https://github.com/iryoku/smaa), MIT), vendored unchanged; AGT shipped the same SMAA for these games. The high-resolution UI text is rasterized with [stb_truetype](https://github.com/nothings/stb) by Sean Barrett (public domain). One replacement typeface is embedded per game: [National Park](https://nationalparktypeface.com/) SemiBold (Rorona) and [Nunito](https://fonts.google.com/specimen/Nunito) Regular (Totori), both under the SIL Open Font License, and Cosmetica Medium (Meruru), an emboldened [MgOpen](https://www.ellak.gr/fonts/mgopen/index.html) Cosmetica under the MgOpen licence and renamed as that licence requires. The handful of symbols none of the three carries come from Arland Fallback, a small face of 67 symbols whose outlines are taken from [Inter](https://rsms.me/inter/) and [Source Sans 3](https://github.com/adobe-fonts/source-sans), both under the SIL Open Font License. Copyright notices and the full licence texts are in [licenses/](licenses/), and ship with each release. [MinHook](https://github.com/TsudaKageyu/minhook) is by Tsuda Kageyu and contributors.

See [TECHNICAL.md](TECHNICAL.md) for the full implementation details and the evidence behind them.

## License

See `LICENSE` for the MIT and zlib license terms that apply to the respective source files.
