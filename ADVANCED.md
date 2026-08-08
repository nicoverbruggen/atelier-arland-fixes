# Advanced configuration

The mod works as a drop-in with no configuration, and most of its settings live in `arland-fix-launcher.exe`, which opens when you start the game. A few are file-only and are marked as such below. That is the place to change things: it writes both `arland-fix.ini` and the game's own `ArlandDX_Settings.ini`, and explains each option beside it.

This document is the reference for `arland-fix.ini`: every key, what it accepts, and what it defaults to. For installation and the default feature set see [README.md](README.md); for the environment switches, the log files and the debug views see [TECHNICAL.md](TECHNICAL.md#diagnostics).

## Skipping the launcher

Once everything is configured the way you want it, the launcher is a window you press one button in. Setting

```ini
[Launcher]
SkipLauncher=true
```

makes Play in Steam go straight into the game with the settings already in the file. It sits on the launcher's **General** tab, under its own **Launcher** heading.

Nothing else changes. The game is started exactly as the launcher starts it, from the same process Steam launched and with the same environment, so the Steam session, the overlay, playtime tracking and Steam Input behave as they do without this set, and the language you chose still decides which of the game's two executables runs. What you give up is the chance to change a setting on the way in: run `arland-fix-launcher.exe` from the game folder when you want to adjust something or turn this back off. If no game executable is found beside the launcher the setting is ignored, and Koei Tecmo's own launcher opens as it otherwise would.

The launcher's own buttons for the original Koei Tecmo tools open them without looping back through the mod's launcher.

## Editing `arland-fix.ini` by hand

The file sits beside the DLLs and is created on first launch. Close the game before editing it. Most keys have an `ARLAND_`-prefixed environment variable that overrides them for a single session. Those, the diagnostic switches, the log files and the debug views are all in [TECHNICAL.md](TECHNICAL.md#diagnostics).

| `[Rendering]` | Values | Default |
| --- | --- | --- |
| `DisplayWidth` / `DisplayHeight` | Window and present size. Blank uses your desktop resolution. | blank |
| `RenderWidth` / `RenderHeight` | Internal render size. Larger than the display size is supersampling. Blank matches the display size. | blank |
| `Borderless` | `true` / `false` | `true` |
| `SMAA` | `true` / `false` | `true` |
| `Font` | `replaced`, `upscaled`, `original` | `replaced` |
| `ShadowMultiplier` | `1` (off), `2`, `4`, `8` | `2` |
| `AnisotropicFiltering` | `1` (off), `2`, `4`, `8`, `16` | `16` |

The launcher has no control for `AnisotropicFiltering` and rewrites it to `16` every time you save, so a hand-set lower value survives only until the next save from the launcher. The same applies to a hand-edited cut-in combination the launcher's two states cannot express: it is read as Enhanced and written back that way. Two more rewrites affect the resolution keys. `RenderWidth`/`RenderHeight` are shown as a multiplier of the display size, snapped to the nearest of 1.25, 1.5, 2, 3 and 4, and saving writes the pair back as that multiple, so a hand-set 2560x1440 over a 1080p display comes back as 2400x1350. And a `DisplayWidth` larger than your monitor is shown as Auto, so saving replaces it with your desktop resolution.

| `[Battle]` | Values | Default |
| --- | --- | --- |
| `BattleCutInShadows` | `true` restores ground shadows during the close-up attack cameras. | `false` |
| `BattleCutInDimming` | `true` keeps the original close-up dimming; `false` holds it at full brightness. | `true` |

Both cut-in keys are off as shipped, which leaves the close-ups as the game renders them. They store one choice between them and the launcher writes them together, so set them from the "Attack cut-ins" list rather than by hand. Setting them by hand is honoured, with one exception: on Atelier Totori DX, `BattleCutInShadows` on its own does nothing, because that game's cut-in shadows are carried by the same value the brightness hold writes. Use Enhanced there, or the pair `BattleCutInShadows=true` and `BattleCutInDimming=false`. Rorona's missing ordinary-battle shadows are restored without a setting, because that is a fix rather than a preference; Meruru and Totori cast them natively and are unaffected.

| `[Field]` | Values | Default |
| --- | --- | --- |
| `MonsterSnapFix` | Spreads a chasing monster's end-of-segment movement over time instead of applying it in a single frame, so it cannot cross your staff's reach between two frames and take the encounter with it. All three games. | `true` |
| `CharacterPullFix` | Stops the engine's character separation pulling two characters together when they are already further apart than their combined size. Applies to every character, including your own party, in all three games. The correction only ever removes that pull; it never alters how characters are pushed apart. | `true` |

| `[Menus]` | Values | Default |
| --- | --- | --- |
| `FastSaveMenu` | Removes the hardcoded waits in front of the save data slots view, and the one in front of leaving it, so it opens as soon as the game has built it. The waits pace nothing: none of them polls a file, a load or whether anything is ready. All three games. | `true` |

| `[Startup]` | Values | Default |
| --- | --- | --- |
| `SkipLogos` | `true` skips the publisher and developer logos while the game boots. | `false` |
| `SkipIntroMovie` | `true` skips the first movie of the run, which is the opening that plays after the logos. Movies you ask for yourself are unaffected: the endings play, and so does anything you replay from the in-game Movies gallery. | `false` |

`[Launcher] SkipLauncher` is above. `[Diagnostics] VerboseLogging` and `[Debug] View` are diagnostics and live in [TECHNICAL.md](TECHNICAL.md#diagnostics).

Each resolution pair is all-or-nothing: if either half is blank or out of range, that pair is ignored. A display resolution larger than your monitor is clamped to the monitor and noted in the log, since the extra pixels would only be scaled away again.

## Things the launcher cannot tell you

**Supersampling is the expensive one.** Cost scales with pixels drawn: 3840×2160 into a 2560×1440 window is 2.25× the pixels of 1440p, and 4× if the window is 1080p. It resolves detail genuinely finer than a display pixel, and sharpens menus and text along with the scene, which no amount of anti-aliasing can do. Exact integer ratios are the sharpest. The render resolution is capped at 7680×4320, with the ratio kept and a line in the log; past 8K these 2010-era assets have no sub-pixel detail left to recover.

**`ShadowMultiplier` ships at `2`.** The games draw every shadow in a scene into one 1024×1024 map, which is blocky at any resolution this mod renders at, so `2` is the default rather than something to opt into. It costs little video memory. `4` is sharper again and worth trying. **`8` is not recommended:** an 8192 map holds 64 times the pixels of the game's own 1024 one, and four times a 4096 one. Setting `1` restores the game's own 1024 map and turns the whole mechanism off.

**Borderless wants a matching display resolution.** The window is sized to the monitor either way, so a smaller backbuffer is simply scaled up to fill it. The defaults already line up: borderless is on and a blank display resolution is your desktop resolution, so the backbuffer matches the window and nothing is scaled. Setting a display resolution below your monitor's is what reintroduces the upscale.

**`Font=replaced` is English-only.** The Japanese and Chinese builds are unaffected in every mode. A different font is bundled per game (National Park SemiBold for Rorona, Nunito Regular for Totori, Cosmetica Medium for Meruru), each embedded in the DLL; their licenses are in `licenses/`. Dropping a `arland-hires-font.ttf` beside the DLL overrides whichever one would be used. Symbols none of them carries (`⇔` in Totori's travel routes, `♪` and `△` in Meruru's tips) are drawn from a bundled fallback face, which applies to your own font too.

**`SkipLogos` does not shorten loading.** The logos play on a separate thread while the game loads, so they are covering work rather than delaying it. Skipping them replaces the logos with a black screen for however long that work actually takes; on a fast disk you get most of the time back, and on a slow one you may get very little. The setting also suppresses the replay that the title screen plays after it has been left idle.
