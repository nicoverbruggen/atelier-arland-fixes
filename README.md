# Atelier Arland Fixes

This mod significantly improves performance and adds various improvements to the Steam releases of **Atelier Rorona DX, Atelier Totori DX, and Atelier Meruru DX**, in every supported language.

For the Dusk trilogy games, please see [this repository instead](https://github.com/nicoverbruggen/atelier-dusk-fixes).

> [!TIP]
> No separate `atelier-sync-fix` or `dinput8.dll` is required. For newer Atelier games (Mysterious games and newer), use the upstream [atelier-sync-fix](https://github.com/doitsujin/atelier-sync-fix) or an appropriate maintained fork instead. This project deliberately contains only Arland-specific code.

## How it works

The mod is intended for the Steam versions of the games.

## What is included

### Launcher

The **included launcher** replaces the standard window for the Arland games and gives you more control over your game experience. It also lets you tweak various **graphics settings** that are added by this mod.

### List of fixes and improvements

These are enabled by default, as they are crucial fixes. Some items do not apply to all games, as they differ slightly in terms of engine and functionality.

| Fix                                        | Rorona | Totori | Meruru |
|--------------------------------------------|:------:|:------:|:------:|
| Much faster menus (removed stutter)        |   ✓    |   ✓    |   ✓    |
| Frame sync fix                             |   ✓    |   ✓    |   ✓    |
| Text-corruption fix                        |   ✓    |   ✓    |   ✓    |
| Higher resolution rendering                |   ✓    |   ✓    |   ✓    |
| High-resolution UI text (English builds)   |   ✓    |   ✓    |   ✓    |
| Correct behaviour at high refresh rates    |   ✓    |   ✓    |   ✓    |
| Correct world-map analog cursor speed      |   —    |   ✓    |   ✓    |
| Restored ordinary-battle shadows           |   ✓    |   —    |   —    |
| Instant save and load menus                |   ✓    |   ✓    |   ✓    |
| Correct picture shape on non-16:9 displays |   ✓    |   ✓    |   ✓    |
| Fixed stutter with no controller connected |   ✓    |   ✓    |   ✓    |
| Optional startup logo and intro-movie skip |   ✓    |   ✓    |   ✓    |
| Various game-specific bug fixes            |   ✓    |   ✓    |   ✓    |
| Local crash logging                        |   ✓    |   ✓    |   ✓    |

If the game crashes, the mod appends a report to `arland-fix.log` that helps pinpoint the cause. Include that file and your settings when reporting the problem, since these logs are never sent anywhere.

### List of graphics enhancements

These things were not part of the original games, but were added with the mod. Each one can be turned on or off in the launcher. Edge smoothing and the shadow enhancements are on to begin with; supersampling and sharpening are optional.

| Enhancement                                | Rorona | Totori | Meruru |
|--------------------------------------------|:------:|:------:|:------:|
| Edge smoothing (SMAA)                      |   ✓    |   ✓    |   ✓    |
| Supersampling (internal render resolution) |   ✓    |   ✓    |   ✓    |
| Sharpening                                 |   ✓    |   ✓    |   ✓    |
| Shadow enhancements                        |   ✓    |   ✓    |   ✓    |

## Installation on Windows

> [!IMPORTANT]
> This mod is a replacement for `atelier-sync-fix` and Atelier Graphics Tweak (`AGT`), so remove those mods first if you have them installed.

1. Open the game's installation directory from Steam by selecting **Manage → Browse local files**.
2. Copy the contents of the latest release (`d3d11.dll`, `arland-fix-launcher.exe`, `msimg32.dll` and `arland-fix.ini`) into that directory, beside the game's own executables. If you are updating an existing install, keep the `arland-fix.ini` you already have, since the bundled one is only the defaults.
3. Launch the game normally through Steam. The mod's launcher should open now instead of the original one.

## Installation on Linux (Proton)

The mod works correctly under Proton, and the games keep using Steam as usual. There is one extra step: Wine ships its own `d3d11` and `msimg32`, and it prefers them over the files in the game folder, so you have to tell it not to.

In **Properties → General → Launch Options**, add:

```text
WINEDLLOVERRIDES="d3d11,msimg32=n,b" %command%
```

Keep `%command%` at the end. After this, the game should use the new launcher that the mod provides.

## Safety

I have done my best to make the mod as safe as possible. Here's what you should know:

### Policy #1: Keep the original game files untouched

Like other DLL-based game fixes, this mod is loaded by the game and changes how parts of it work while it is running. It does not permanently patch the games: the changes disappear when you close the game, and the original executables and game assets are never edited.

### Policy #2: Safety checks and easy removal

In practical terms:

- Address-based and Direct3D fixes require an exact executable name and build fingerprint. If either does not match, those fixes are skipped and Direct3D is only forwarded. Two startup-window corrections must be hooked before the normal fingerprint gate runs; they change a call only when the game module, executable/window name or class/background brush matches their narrow runtime checks, and otherwise pass it through unchanged.
- It does not read or write your save files, collect usage data, update itself or connect to the internet in the background.
- The only files it normally creates or updates are its settings and diagnostic logs. Changing options in the settings launcher also updates the game's own settings file.
- **Play without the mod** starts the game with the fixes disabled. Removing or renaming the mod's DLL files returns the game to normal the next time it starts.

Like any DLL mod, this is executable code that runs with the same access as the game. Download it only from this repository's [official releases](https://github.com/nicoverbruggen/atelier-arland-fixes/releases), or build it from source.

### Policy #3: Public source and build process

The complete source code and the steps GitHub uses to build each release are public. You can also make your own build: the repository builds with Meson and a MinGW cross toolchain, and `scripts/build_linux.sh` runs the whole thing.

> [!TIP]
> If you do not read code yourself, an LLM (like ChatGPT or Claude) can help review the repository for obvious red flags and explain whether it appears to do what this page claims. That can be a useful second opinion, but it is not a guarantee and cannot prove that a downloaded file was built from the published source. You can ask it to inspect the built files, however this may use up a lot of tokens.

## Configuration

Use the launcher. It writes the game's own configuration file and the mod's `arland-fix.ini`, and only shows options the game it sits next to supports. `arland-fix.ini` ships with every default filled in, so it also serves as the list of what can be set.

## Credits

> [!NOTE]
> The menu-hitch research that made this project possible was mine, and I did the reverse engineering and integration that combine all of the above plus more into a single mod, using large language models from OpenAI and Anthropic throughout to analyze the games, develop the fixes, and bundle the improvements together. I believe that LLMs were used responsibly in this project.

This mod was inspired by, and consulted, prior work by:

- Philip Rebohle's [`atelier-sync-fix`](https://github.com/doitsujin/atelier-sync-fix)
- TellowKrinkle's [`atelier-sync-fix` fork](https://github.com/TellowKrinkle/atelier-sync-fix)
- Yuri Hime's [Atelier Graphics Tweak](https://steamcommunity.com/app/1152300/discussions/0/3345546664208090238/)
- The [Rorona community investigation](https://steamcommunity.com/app/936160/discussions/0/1742227264210806751/?ctp=2) into the font-atlas transfer problem

The bundled SMAA anti-aliasing is by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, and Diego Gutierrez ([SMAA](https://github.com/iryoku/smaa), MIT), vendored unchanged; AGT shipped the same SMAA for these games. The high-resolution UI text is rasterized with [stb_truetype](https://github.com/nothings/stb) by Sean Barrett (public domain). One replacement typeface is embedded per game: [National Park](https://nationalparktypeface.com/) SemiBold (Rorona) and [Nunito](https://fonts.google.com/specimen/Nunito) Regular (Totori), both under the SIL Open Font License, and Cosmetica Medium (Meruru), an emboldened [MgOpen](https://www.ellak.gr/fonts/mgopen/index.html) Cosmetica under the MgOpen licence and renamed as that licence requires. The handful of symbols none of the three carries come from Arland Fallback, a small face of 67 symbols whose outlines are taken from [Inter](https://rsms.me/inter/) and [Source Sans 3](https://github.com/adobe-fonts/source-sans), both under the SIL Open Font License. Copyright notices and the full licence texts are in [licenses/](licenses/), and ship with each release. [MinHook](https://github.com/TsudaKageyu/minhook) is by Tsuda Kageyu and contributors.

## License

See `LICENSE` for the MIT and zlib license terms that apply to the respective source files.
