# Advanced configuration

The mod works as a drop-in with no configuration: faster menus, the synchronization and text-corruption fixes, high-resolution rendering support, and Atelier Rorona DX's restored battle shadows are all active by default. This document covers the optional graphics tweaks, the on-by-default features you may want to adjust, how to combine them for the best experience, and the diagnostic switches. For installation and the default feature set, see [README.md](README.md).

All settings live in `arland-fix.ini` beside the DLLs (created on first launch). Close the game before editing the file. Everything in this document can also be set from `arland-fix-launcher.exe`, which is usually easier.

## The launcher

`arland-fix-launcher.exe` holds every setting the mod has in one window, writes both `arland-fix.ini` and the game's own `ArlandDX_Settings.ini`, and starts the game. It configures whichever game folder it is run from, so it needs no configuration of its own.

Starting the game normally opens it. Both of the games' own front-ends load `msimg32.dll`, so the mod's copy recognises Koei Tecmo's launcher among them and opens this one in its place before the original appears. Pressing Play in Steam therefore lands you here, and pressing Play with mod continues into the game as usual. This has been tested through Steam under Proton: the Steam session, the overlay and playtime tracking all behave as they would without the mod.

If `arland-fix-launcher.exe` is not installed, the original launcher opens exactly as before, so an incomplete install cannot leave the game unstartable.

Koei Tecmo's own launcher and settings editor are still available, from the buttons on the launcher's Display tab. Those open the real ones: the substitution does not apply to a launcher opened this way, so they cannot loop back. The settings editor also still gains the extra resolutions described under [Resolution](#resolution).

To bypass the substitution outside the launcher, set `ARLAND_NO_REDIRECT=1` in the environment. The original launcher then opens untouched.

### Skipping the launcher

Once everything is configured the way you want it, the launcher is a window you press one button in. Setting

```ini
[Launcher]
SkipLauncher=true
```

in `arland-fix.ini` makes Play in Steam go straight into the game, with the settings already in the file. The checkbox for it sits beside **Play with mod** in the launcher, so ticking it and starting the game is one gesture.

Nothing else changes. The game is started exactly as the launcher starts it, from the same process Steam launched and with the same environment, so the Steam session, the overlay, playtime tracking and Steam Input behave as they do without this set. The language you chose still decides which of the game's two executables runs. What you give up is the chance to change a setting on the way in: the launcher no longer opens by itself, so run `arland-fix-launcher.exe` from the game folder when you want to adjust something or turn this back off.

If no game executable is found beside the launcher, the setting is ignored and Koei Tecmo's own launcher opens as it otherwise would.

## Wine and Proton

Copy both DLLs into the game directory as described in the README, then add this to the game's Steam Launch Options:

```text
WINEDLLOVERRIDES="d3d11,msimg32=n,b" %command%
```

The same `arland-fix.ini` file used on Windows configures the mod under Wine or Proton. Runtime messages are written to `arland-fix.log` in the game directory.

## Resolution

The launcher DLL always exposes 1920×1080, 2560×1440, and 3840×2160 in both launcher states even when DPI or display-mode enumeration would normally hide them. Keeping 1920×1080 visible is intentional for Steam Deck and other lower-resolution handhelds, where a high-DPI desktop can otherwise prevent the launcher from exposing 1080p; it also supports higher-resolution rendering for downsampling and normal docked use.

Independently of the launcher, `arland-fix.ini` sets two resolutions:

```ini
[Rendering]
DisplayWidth=2560
DisplayHeight=1440
RenderWidth=3840
RenderHeight=2160
```

`DisplayWidth`/`DisplayHeight` size the window and what reaches the screen — use your monitor's resolution. Leave both blank to keep whatever the launcher selected. Setting them above your monitor's resolution gains nothing (the extra pixels are scaled away again), so a larger value is clamped to the display and noted in the log; rendering higher than the screen is what `RenderWidth`/ `RenderHeight` is for.

`RenderWidth`/`RenderHeight` size everything the game actually draws. Leave both blank and the game renders at the display resolution, exactly as before. Setting them higher turns on supersampling, below.

Each pair is all-or-nothing: if either half is blank or out of range, that pair is ignored.

### Supersampling

When the render resolution is larger than the display resolution, the whole frame — scene *and* UI — is rendered at that larger size and downscaled once, just before it reaches the screen.

Unlike MSAA and SMAA, which smooth edges after the fact, this resolves detail that is genuinely finer than a display pixel, and menus and text sharpen along with the scene. The whole pipeline follows the render resolution, so the background blur and depth-of-field stay internally consistent.

It is the most expensive option in this file, and cost scales with pixels drawn: 3840×2160 into a 2560×1440 window is 2.25× the pixels of 1440p, and 4× if the window is 1080p. Exact integer ratios (a 1080p window rendering at 4K) resolve with a true box filter and are the sharpest; other ratios use a bilinear resolve. Because supersampling already anti-aliases geometry, there is little reason to combine it with MSAA — that multiplies the cost of an already larger render target — so prefer raising the render resolution over stacking the two.

The render resolution is capped at **7680×4320** (8K), and a larger value is reduced to it with the ratio kept and a line in the log. The launcher does not offer multipliers that would exceed the cap; where one is configured that no longer fits, it drops to the largest that does rather than switching supersampling off.

The cap is about diminishing returns rather than a hard limit. These games' assets date from 2010, and the sub-pixel detail supersampling recovers is exhausted well before 8K, while the cost keeps scaling with the pixels drawn: a 4K display at 4× is 15360×8640, over half a gigabyte for a single render target, and the games allocate many.

### Deprecated: `Width` / `Height`

`Width`/`Height` are the old names for the display resolution and are **deprecated**. They are still honoured when `DisplayWidth`/`DisplayHeight` are blank, so existing configurations keep working, but new ones should use `DisplayWidth`/`DisplayHeight`. If both pairs are set, the `Display` pair wins.

## Borderless windowed mode

The games offer only a plain window with a title bar, or exclusive fullscreen. Exclusive fullscreen takes control of the display, which makes alt-tabbing slow and interacts badly with compositors and multi-monitor setups under Wine and Proton.

```ini
[Rendering]
Borderless=true
```

The game then runs as a window with its decorations removed, sized to fill the monitor it is on — it looks like fullscreen, alt-tabs instantly, and leaves the display mode alone. The game's own `FullScreen` setting in `ArlandDX_Settings.ini` is ignored while this is on — leave it at whatever it is. The mod forces the swap chain windowed, takes alt-enter away from DXGI, and refuses display-mode changes, so nothing can hand the screen back to exclusive fullscreen behind it. Off by default. `ARLAND_BORDERLESS` overrides the INI for a session.

Use it with `DisplayWidth`/`DisplayHeight` set to your monitor's resolution. The window is sized to the monitor either way, so a smaller backbuffer is simply scaled up to fill it.

## SMAA anti-aliasing

SMAA (a post-process anti-aliasing pass) is **on by default**. It smooths edges across the whole scene, including visible edges inside textures that geometry-only MSAA cannot touch, at a low, constant cost. It is applied before the UI is drawn so the HUD and text stay crisp in all three games, whether or not optional MSAA and supersampling are enabled. SMAA and MSAA complement each other rather than replacing each other — MSAA only smooths the outlines of 3D shapes, while SMAA also catches edges inside textures, on cut-out foliage and hair, and in specular highlights — so turning one on never turns the other off. Turn it off with:

```ini
[Rendering]
SMAA=false
```

`ARLAND_SMAA` overrides the INI for a session. SMAA and MSAA can be combined (SMAA cleans up what MSAA's geometry-only multisampling leaves), or SMAA can be used alone as a much cheaper alternative to MSAA.

## UI font

The games draw all UI text from a low-resolution pre-baked bitmap font. `Font` in the `[Rendering]` section chooses how it is rendered; it is **`replaced` by default**:

```ini
[Rendering]
Font=replaced
```

- **`replaced`** (default) — re-render each string from a scalable font embedded in the DLL, at full resolution, keeping the game's own layout. Glyphs the font lacks — such as the custom controller-button icons — fall back to `upscaled` automatically, so nothing is left pixelated or missing.
- **`upscaled`** — keep the original baked glyphs but smooth them (the exact original look, just sharper).
- **`original`** (or `off`) — the untouched original bitmap font.

`replaced` mode uses a different bundled font per game, each embedded in the DLL so nothing extra is installed:

- **Atelier Rorona** — National Park SemiBold
- **Atelier Totori** — Nunito Regular
- **Atelier Meruru** — Cosmetica Medium (a bolder MgOpen Cosmetica)

To supply your own replacement font instead, drop a `arland-hires-font.ttf` next to the DLL; it overrides the bundled font for whichever game is running. `ARLAND_UIFONT` overrides the mode for a session (`ARLAND_UIFONT=upscaled`, etc.), and `ARLAND_HIRES_SCALE` / `ARLAND_HIRES_VOFF` nudge the replacement font's size and vertical position for quick tuning. English versions only — the Japanese and Chinese builds are unaffected. Each bundled font's license is in `licenses/`.

## MSAA

Multisample anti-aliasing is off by default. SMAA, below, is the anti-aliasing the mod ships with, and it costs far less. `MSAA` in the `[Rendering]` section requests a sample count of `2`, `4`, or `8`:

```ini
[Rendering]
MSAA=4
```

If the GPU or selected format does not support the requested count, the mod falls back to a lower supported count. Use `0` or `1`, or remove the setting entirely, to disable MSAA. Higher sample counts increase GPU and video-memory cost.

## Anisotropic filtering

The games sample their textures with plain linear filtering, so surfaces seen at a shallow angle — floors, walls, the ground stretching into the distance — look blurry. `AnisotropicFiltering` upgrades that to anisotropic filtering, which keeps those oblique surfaces sharp:

```ini
[Rendering]
AnisotropicFiltering=16
```

Accepted values are `2`, `4`, `8`, and `16` (the maximum-anisotropy level); `0`, `1`, or any other number leaves the game's original filtering in place. The mod upgrades the game's texture samplers at creation, so all world and character textures benefit with no per-frame cost; shadow-comparison samplers are left untouched. On by default at `8`, which is also what you get if the setting is removed entirely, so use `1` to turn it off. `ARLAND_ANISO` overrides the INI.

## Shadow resolution multiplier

The games render shadows into a 1024×1024 shadow map, so shadow edges can look blocky, most noticeably in Atelier Meruru DX. `ShadowMultiplier` in the `[Rendering]` section renders shadows at a higher internal resolution for crisper edges:

```ini
[Rendering]
ShadowMultiplier=2
```

Accepted values are `1` (the default, unchanged 1024×1024), `2`, `4`, and `8`, which render the shadow map at 2048, 4096, and 8192 respectively; any other value falls back to `1`. The mod allocates its own higher-resolution shadow maps and redirects the shadow pipeline onto them, leaving the game's own 1024×1024 textures untouched so the engine's size and memory assumptions stay valid. At `1` the mod does not touch the shadow pipeline at all.

Higher multipliers increase GPU and video-memory cost substantially: `8` is the heaviest setting and its stability under long play sessions is still being validated — prefer `2` or `4`.

## Battle shadows

Atelier Rorona DX omitted all character and enemy shadows during ordinary battle; the mod restores them, enabled by default. `BattleShadows` in the `[Battle]` section toggles the restoration (Meruru already casts these natively, and the toggle does not affect Atelier Totori DX):

```ini
[Battle]
BattleShadows=true
```

## Battle cut-in shadows and brightness

During the battle action cut-ins (the close-up attack cameras), the games show no ground shadows on any platform and dim the scene. The mod can restore the shadows and keep the close-up at full brightness in all three games. Both are **off by default**; enable them from the `[Battle]` section:

```ini
[Battle]
BattleCutInShadows=true
BattleCutInDimming=false
```

`BattleCutInShadows` (default `false`) restores the ground shadows during cut-ins; set it to `true` to enable them.

`BattleCutInDimming` (default `true`) keeps the original close-up dimming; set it to `false` to hold the cut-in at full brightness. The two options are independent and apply to all three games, in both the English and the multilingual builds.

## Suggested "best experience" configuration

On a GPU with headroom, this is the configuration the optional features were designed for:

```ini
[Rendering]
Font=replaced
SMAA=true
MSAA=4
DisplayWidth=
DisplayHeight=
RenderWidth=
RenderHeight=
ShadowMultiplier=2
AnisotropicFiltering=16

[Battle]
BattleShadows=true
BattleCutInShadows=true
BattleCutInDimming=false
```

Supersampling is left blank here because it costs more than everything else in this file combined. On hardware with real headroom it is the single biggest image-quality win available: set `DisplayWidth`/`DisplayHeight` to your monitor and `RenderWidth`/`RenderHeight` one step higher, and drop `MSAA` back to `1` rather than paying for both.

Raise `MSAA` to `8` and `ShadowMultiplier` to `4` on strong hardware. (The high-resolution UI font, SMAA, and the restored fighting battle shadows (`BattleShadows`) are on by default and listed here only for completeness. The cut-in restorations (`BattleCutInShadows` and `BattleCutInDimming`) are off by default; they cost little, so they are worth enabling.)

## Diagnostic switches

These are environment variables, not INI keys: they are for narrowing down a problem for a bug report. Most only add logging, but a few named below deliberately switch a fix back off so its effect can be compared against unmodified behaviour, and those do change how the game plays for that launch.

| Variable | Effect |
| --- | --- |
| `ARLAND_VERBOSE_LOG=1` | Extra logging, same as `[Diagnostics] VerboseLogging`. Adds implementation details such as hook addresses, resource redirections and battle-object state, and turns on the observing diagnostics below (the frame-time line, menu statistics, scene tracking and the memory probe), so a detailed log can be requested without anyone editing several environment variables. The heavier traces stay opt-in individually, and `ARLAND_MENU_TRANSITION_TRACE` is deliberately never included because it changes the code path it reports on. |
| `ARLAND_MENU_STATS=1` | Per-drain menu timings and cache hit rates. |
| `ARLAND_FIELD_TRACE=1` | Logs the field-map character's state around each loss of footing. |
| `ARLAND_PRESENT_INTERVAL=0` | Forces vsync off (`1`, `2`, `3` present every Nth refresh). Useful with an external frame limiter. Unset, the game's own vsync is left alone; `0` will tear in exclusive fullscreen. |
| `ARLAND_NO_REDIRECT=1` | Opens Koei Tecmo's own launcher instead of the mod's. |
| `ARLAND_PERF_LOG=1` | On with verbose logging; set explicitly to `0` to suppress it or `1` without it. Writes a `PERF` line to `arland-fix.log` every ten seconds: average frame rate, average frame time, and the worst single frame in that window. For comparing one setting against another, which an average alone cannot do: a steady 60 with a 90 ms hitch reads very differently from a steady 60. |
| `ARLAND_DISABLE=1` | Stands the whole mod down for one launch: `d3d11.dll` still loads and still forwards Direct3D, but installs no hooks and changes nothing, so the game runs as it shipped. This is what the launcher's **Play without the mod** button passes to the game, and it is the quickest way to tell a problem apart from a problem the mod is causing without moving files out of the game folder. |
| `ARLAND_FIELD_ENGINE_FIX=0` | Restores the game's own minimum-movement distance, reinstating the high-refresh field-map problems. For comparing against unmodified behaviour. |
| `ARLAND_FIELD_STABILIZER=0` | Stops holding the character still at rest, leaving a small residual movement at high frame rates. Ignored unless the option above is also on. |
| `ARLAND_CUTIN_ACTOR_CLEAR=0` | Stops clearing a mid-cut-in battler's shadow when its fade begins, so it fades out over its full quarter second with the shadow at full strength until the end. Only has an effect while the cut-in shadow restoration is enabled. For comparing against unmodified behaviour. |
| `ARLAND_RESOLUTION_TRACE=1` | Traces the resolution override's effect on render targets. |
| `ARLAND_PRESENT_TRACE=1` | Reports how the finished frame reaches the screen. Needs a display resolution set. |

Most INI options also have an `ARLAND_`-prefixed override for a single session; those are noted alongside each option above.

## Logs and crash reports

Runtime messages are written to `arland-fix.log` in the game directory; the previous session's log is kept as `arland-fix.log.old`. A normal log starts with the mod version, recognized game build, complete configuration and concise `FIXES` lines showing what installed successfully. Warnings and failures are included, with failures from hot rendering paths reported once unless verbose logging is enabled; raw hook addresses, repeated resource operations and battle-object telemetry also require verbose logging. If the game crashes, the mod appends a `CRASH` post-mortem to the log — the exception, the faulting address as module+offset, registers, and a stack scan — before the process exits. Include both files when reporting a problem.

## Debug views

Turning on verbose logging adds a **Debug** tab to the launcher, holding a single **Debug view** dropdown. These are diagnostics rather than quality settings: each one replaces what is drawn so a particular part of the mod can be checked by eye, which is why only one runs at a time. Off in a normal install.

- **Wireframe** — draws 3D geometry as outlines. The HUD, menus and movies are left alone, since those are flat quads drawn with depth testing off. Shows model detail and where level-of-detail models swap as the camera moves.
- **SMAA edge detection** — outlines what the antialiasing pass found, in red and green over a dimmed scene. The HUD stays untouched, because the pass runs before the UI is drawn — which also makes this the quickest way to confirm the pre-UI injection is working.
- **SMAA blend weights** — the following pass. Worth a look when the edges look right but the result does not.
- **Highlight scene target** — tints the surface the antialiasing pass picked, at the moment it picks it. Green over the world but not the HUD means it found the right surface at the right point in the frame; green over the HUD as well means it ran too late.

In the INI the setting is `[Debug] View`, one of `off`, `wireframe`, `smaa-edges`, `smaa-weights` or `scene-target`. `ARLAND_DEBUG_VIEW` overrides it, and the older `ARLAND_SMAA_DEBUG=1|2` still selects the two SMAA views.
