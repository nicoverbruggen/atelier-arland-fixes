# Advanced configuration

The mod works as a drop-in with no configuration, and every setting it has lives in `arland-fix-launcher.exe`, which opens when you start the game. That is the place to change things: it writes both `arland-fix.ini` and the game's own `ArlandDX_Settings.ini`, explains each option beside it, and ships five quality presets.

This document covers what the launcher does not: editing `arland-fix.ini` by hand, the environment variables, the log files, and the debug views. For installation, the default feature set, and the Wine/Proton launch options, see [README.md](README.md).

## Skipping the launcher

Once everything is configured the way you want it, the launcher is a window you press one button in. Setting

```ini
[Launcher]
SkipLauncher=true
```

makes Play in Steam go straight into the game with the settings already in the file. The checkbox for it sits beside **Play with mod** in the launcher.

Nothing else changes. The game is started exactly as the launcher starts it, from the same process Steam launched and with the same environment, so the Steam session, the overlay, playtime tracking and Steam Input behave as they do without this set, and the language you chose still decides which of the game's two executables runs. What you give up is the chance to change a setting on the way in: run `arland-fix-launcher.exe` from the game folder when you want to adjust something or turn this back off. If no game executable is found beside the launcher the setting is ignored, and Koei Tecmo's own launcher opens as it otherwise would.

`ARLAND_NO_REDIRECT=1` in the environment opens Koei Tecmo's launcher instead of the mod's, for one launch. The launcher's own buttons for the original tools use this, which is why they cannot loop back.

## Editing `arland-fix.ini` by hand

The file sits beside the DLLs and is created on first launch. Close the game before editing it. Every key here has an `ARLAND_`-prefixed environment variable that overrides it for a single session.

| `[Rendering]` | Values | Default | Override |
| --- | --- | --- | --- |
| `DisplayWidth` / `DisplayHeight` | Window and present size. Blank uses your desktop resolution. `DisplayWidth=game` keeps whatever the game's own settings selected. | blank | |
| `RenderWidth` / `RenderHeight` | Internal render size. Larger than the display size is supersampling. Blank matches the display size. | blank | |
| `Borderless` | `true` / `false` | `true` | `ARLAND_BORDERLESS` |
| `SMAA` | `true` / `false` | `true` | `ARLAND_SMAA` |
| `MSAA` | `1` (off), `2`, `4`, `8` | `1` | `ARLAND_MSAA` |
| `Font` | `replaced`, `upscaled`, `original` | `replaced` | `ARLAND_UIFONT` |
| `ShadowMultiplier` | `1` (off), `2`, `4`, `8` | `1` | `ARLAND_SHADOW_MULTIPLIER` |
| `AnisotropicFiltering` | `1` (off), `2`, `4`, `8`, `16` | `16` | `ARLAND_ANISO` |

| `[Battle]` | Values | Default |
| --- | --- | --- |
| `BattleShadows` | Restores Rorona's missing ordinary-battle shadows. Meruru and Totori cast them natively and are unaffected. | `true` |
| `BattleCutInShadows` | `true` restores ground shadows during the close-up attack cameras. | `true` |
| `BattleCutInDimming` | `true` keeps the original close-up dimming; `false` holds it at full brightness. | `false` |

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
| `SkipIntroMovie` | `true` skips the opening movie that plays after the logos. The ending movies are unaffected. | `false` |

`[Launcher] SkipLauncher` is above; `[Diagnostics] VerboseLogging` is under [Logs](#logs-and-crash-reports); `[Debug] View` is under [Debug views](#debug-views).

Each resolution pair is all-or-nothing: if either half is blank or out of range, that pair is ignored. A display resolution larger than your monitor is clamped to the monitor and noted in the log, since the extra pixels would only be scaled away again.

## Things the launcher cannot tell you

**Supersampling is the expensive one.** Cost scales with pixels drawn: 3840×2160 into a 2560×1440 window is 2.25× the pixels of 1440p, and 4× if the window is 1080p. It resolves detail genuinely finer than a display pixel, and sharpens menus and text along with the scene, which no amount of anti-aliasing can do. Exact integer ratios are the sharpest. Because it already anti-aliases geometry, drop `MSAA` back to `1` rather than paying for both. The render resolution is capped at 7680×4320, with the ratio kept and a line in the log; past 8K these 2010-era assets have no sub-pixel detail left to recover.

**`ShadowMultiplier=8` is not recommended.** Its cost in video memory is substantial and its stability over long sessions is still being validated. Prefer `2` or `4`.

**Borderless wants a matching display resolution.** The window is sized to the monitor either way, so a smaller backbuffer is simply scaled up to fill it. The defaults already line up: borderless is on and a blank display resolution is your desktop resolution, so the backbuffer matches the window and nothing is scaled. Setting a display resolution below your monitor's is what reintroduces the upscale.

**`Font=replaced` is English-only.** The Japanese and Chinese builds are unaffected in every mode. A different font is bundled per game (National Park SemiBold for Rorona, Nunito Regular for Totori, Cosmetica Medium for Meruru), each embedded in the DLL; their licenses are in `licenses/`. Dropping a `arland-hires-font.ttf` beside the DLL overrides whichever one would be used. `ARLAND_HIRES_SCALE` and `ARLAND_HIRES_VOFF` nudge the replacement font's size and vertical position for quick tuning.

**`SkipLogos` does not shorten loading.** The logos play on a separate thread while the game loads, so they are covering work rather than delaying it. Skipping them replaces the logos with a black screen for however long that work actually takes; on a fast disk you get most of the time back, and on a slow one you may get very little. The setting also suppresses the replay that the title screen plays after it has been left idle.

**SMAA and MSAA are not alternatives.** MSAA only smooths the outlines of 3D shapes; SMAA also catches edges inside textures, on cut-out foliage and hair, and in specular highlights. Turning one on never turns the other off, and SMAA alone is far cheaper than MSAA.

## Diagnostic switches

These are environment variables, not INI keys: they are for narrowing down a problem for a bug report. Most only add logging, but the ones marked *(A/B)* switch a fix back off so its effect can be compared against unmodified behaviour, and those do change how the game plays for that launch.

| Variable | Effect |
| --- | --- |
| `ARLAND_DISABLE=1` | Stands the whole mod down for one launch: `d3d11.dll` still loads and still forwards Direct3D, but installs no hooks and changes nothing, so the game runs as it shipped. This is what the launcher's **Play without the mod** button passes to the game, and it is the quickest way to tell a problem apart from a problem the mod is causing without moving files out of the game folder. |
| `ARLAND_VERBOSE_LOG=1` | Extra logging, same as `[Diagnostics] VerboseLogging`. Adds implementation details such as hook addresses, resource redirections and battle-object state, and turns on the observing diagnostics below (the frame-time line, menu statistics, scene tracking and the memory probe), so a detailed log can be requested without anyone editing several environment variables. The heavier traces stay opt-in individually, and `ARLAND_MENU_TRANSITION_TRACE` is never included because it changes the code path it reports on. |
| `ARLAND_PERF_LOG=1` | On with verbose logging; set explicitly to `0` to suppress it or `1` without it. Writes a `PERF` line to `arland-fix.log` every ten seconds: average frame rate, average frame time, and the worst single frame in that window. For comparing one setting against another, which an average alone cannot do: a steady 60 with a 90 ms hitch reads very differently from a steady 60. |
| `ARLAND_MENU_STATS=1` | Per-drain menu timings and cache hit rates. Follows verbose logging unless set explicitly. |
| `ARLAND_FIELD_TRACE=1` | Logs the field-map character's state around each loss of footing. |
| `ARLAND_RESOLUTION_TRACE=1` | Traces the resolution override's effect on render targets. |
| `ARLAND_PRESENT_TRACE=1` | Reports how the finished frame reaches the screen. Needs a display resolution set. |
| `ARLAND_ITEM_PROBE=1` | Logs every malformed item index Atelier Totori DX's guards reject, with the whole record and the caller, not just the first. |
| `ARLAND_PRESENT_INTERVAL=0` | Forces vsync off (`1`, `2`, `3` present every Nth refresh). Useful with an external frame limiter. Unset, the game's own vsync is left alone; `0` will tear in exclusive fullscreen. |
| `ARLAND_NO_REDIRECT=1` | Opens Koei Tecmo's own launcher instead of the mod's. |
| `ARLAND_CRASH_LOG=0` | Stands the crash post-mortem down. It is otherwise always written. |
| `ARLAND_FIELD_ENGINE_FIX=0` | *(A/B)* Restores the game's own minimum-movement distance, reinstating the high-refresh field-map problems. |
| `ARLAND_FIELD_STABILIZER=0` | *(A/B)* Stops holding the character still at rest, leaving a small residual movement at high frame rates. Ignored unless the option above is also on. |
| `ARLAND_CUTIN_ACTOR_CLEAR=0` | *(A/B)* Stops clearing a mid-cut-in battler's shadow when its fade begins, so it fades out over its full quarter second with the shadow at full strength until the end. Only has an effect while the cut-in shadow restoration is enabled. |
| `ARLAND_ITEM_GUARD=0` | *(A/B)* Restores Totori's own unbounded item-effect lookups. |
| `ARLAND_ITEM_SANITIZE=0` | *(A/B)* Disables the repair of damaged Totori save data on load and save. |
| `ARLAND_BUC_TEXT_CACHE=0` | *(A/B)* Disables the conversation-scoped text cache in Atelier Meruru DX. |

## Logs and crash reports

Runtime messages are written to `arland-fix.log` in the game directory; the previous session's log is kept as `arland-fix.log.old`. A normal log starts with the mod version, recognized game build, complete configuration and concise `FIXES` lines showing what installed successfully. Warnings and failures are included, with failures from hot rendering paths reported once unless verbose logging is enabled; raw hook addresses, repeated resource operations and battle-object telemetry also require verbose logging.

If the game crashes, the mod appends a `CRASH` post-mortem to the log — the exception, the faulting address as module+offset, registers, and a stack scan — before the process exits. Include both files when reporting a problem.

## Debug views

Turning on verbose logging adds a **Debug** tab to the launcher, holding a single **Debug view** dropdown. These are diagnostics, not quality settings: each one replaces what is drawn so a particular part of the mod can be checked by eye, which is why only one runs at a time. Off in a normal install.

- **Wireframe** — draws 3D geometry as outlines. The HUD, menus and movies are left alone, since those are flat quads drawn with depth testing off. Shows model detail and where level-of-detail models swap as the camera moves.
- **SMAA edge detection** — outlines what the antialiasing pass found, in red and green over a dimmed scene. The HUD stays untouched, because the pass runs before the UI is drawn — which also makes this the quickest way to confirm the pre-UI injection is working.
- **SMAA blend weights** — the following pass. Worth a look when the edges look right but the result does not.
- **Highlight scene target** — tints the surface the antialiasing pass picked, at the moment it picks it. Green over the world but not the HUD means it found the right surface at the right point in the frame; green over the HUD as well means it ran too late.

In the INI the setting is `[Debug] View`, one of `off`, `wireframe`, `smaa-edges`, `smaa-weights` or `scene-target`. `ARLAND_DEBUG_VIEW` overrides it, and the older `ARLAND_SMAA_DEBUG=1|2` still selects the two SMAA views.
