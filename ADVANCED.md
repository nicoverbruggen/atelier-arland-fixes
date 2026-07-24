# Advanced configuration

The mod works as a drop-in with no configuration: faster menus, the synchronization and text-corruption fixes, high-resolution rendering support, and Atelier Rorona DX's restored battle shadows are all active by default. This document covers the optional graphics tweaks, the on-by-default features you may want to adjust, how to combine them for the best experience, and the diagnostic switches. For installation and the default feature set, see [README.md](README.md).

All settings live in `arland-fix.ini` beside the DLLs (created on first launch). Close the game before editing the file.

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

Unlike MSAA and SMAA, which smooth edges after the fact, this resolves detail that is genuinely finer than a display pixel: the thin alpha-tested costume trim no post-process can fully recover comes back, and menus and text sharpen along with the scene. The whole pipeline follows the render resolution, so the background blur and depth-of-field stay internally consistent.

It is the most expensive option in this file, and cost scales with pixels drawn: 3840×2160 into a 2560×1440 window is 2.25× the pixels of 1440p, and 4× if the window is 1080p. Exact integer ratios (a 1080p window rendering at 4K) resolve with a true box filter and are the sharpest; other ratios use a bilinear resolve. Because supersampling already anti-aliases geometry, there is little reason to combine it with MSAA — that multiplies the cost of an already larger render target — so prefer raising the render resolution over stacking the two.

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

## Frame rate cap

Above roughly 115 fps the field-map character buzzes vertically when standing on a step or ledge — so this shows on 120 Hz and 144 Hz displays and not on 60 Hz ones. The games discard any frame in which the character moves less than a fixed distance, a constant that is only correct at 60 fps; at rest the only movement is a single frame of gravity, which at high frame rates never covers that distance before the character's footing times out, so it falls and lands repeatedly.

```ini
[Engine]
MaxFps=100
```

The mod holds the frame rate just below where that begins. 100 is both the default and the maximum — a higher ceiling runs straight back into the problem, so larger values are clamped. Lower it if you prefer. Setting it to `0` removes the cap entirely, which is **not recommended**: above roughly 115 fps the field-map stutter returns.

This keeps well above 60 fps, and since it changes nothing inside the game it also covers any other 60 fps assumption the engine may hold that has not been found. Note that pacing turns vsync off: a ceiling that is not a divisor of your display's refresh rate cannot be held with vsync on.

## SMAA anti-aliasing

SMAA (a post-process anti-aliasing pass) is **on by default**. It smooths edges across the whole scene — including the thin alpha-tested costume trim that MSAA cannot touch — at a low, constant cost, and is applied before the UI is drawn so the HUD and text stay crisp. (Atelier Totori DX composites its scene and UI into one render target with no separable pre-UI point, so there SMAA runs as a full-frame pass at present time instead and also lightly affects the UI.) Turn it off with:

```ini
[Rendering]
SMAA=false
```

`ARLAND_SMAA` overrides the INI for a session. SMAA and MSAA can be combined (SMAA cleans up what MSAA's geometry-only multisampling leaves), or SMAA can be used alone as a much cheaper alternative to MSAA. Note: the very fine lace trim is sub-pixel alpha-test detail that no post-process (SMAA included) can fully resolve — only rendering at a higher internal resolution does.

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

Multisample anti-aliasing is disabled by default. `MSAA` in the `[Rendering]` section requests a sample count of `2`, `4`, or `8`:

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

Accepted values are `2`, `4`, `8`, and `16` (the maximum-anisotropy level); `0`, `1` (the shipped value), any other number, or the setting removed all leave the game's original filtering in place. The mod upgrades the game's texture samplers at creation, so all world and character textures benefit with no per-frame cost; shadow-comparison samplers are left untouched. Off by default. `ARLAND_ANISO` overrides the INI.

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

`BattleCutInDimming` (default `true`) keeps the original close-up dimming; set it to `false` to hold the cut-in at full brightness. The two options are independent and apply to all three games.

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

[Engine]
MaxFps=100

[Battle]
BattleShadows=true
BattleCutInShadows=true
BattleCutInDimming=false
```

Supersampling is left blank here because it costs more than everything else in this file combined. On hardware with real headroom it is the single biggest image-quality win available: set `DisplayWidth`/`DisplayHeight` to your monitor and `RenderWidth`/`RenderHeight` one step higher, and drop `MSAA` back to `1` rather than paying for both.

Raise `MSAA` to `8` and `ShadowMultiplier` to `4` on strong hardware. (The high-resolution UI font, SMAA, and the restored fighting battle shadows (`BattleShadows`) are on by default and listed here only for completeness. The cut-in restorations (`BattleCutInShadows` and `BattleCutInDimming`) and `AnisotropicFiltering` are off by default; they cost little, so they are worth enabling.)

## Diagnostic switches

These are environment variables, not INI keys: they are for narrowing down a problem for a bug report, and none of them change how the game plays.

| Variable | Effect |
| --- | --- |
| `ARLAND_VERBOSE_LOG=1` | Extra logging, same as `[Diagnostics] VerboseLogging`. |
| `ARLAND_MENU_STATS=1` | Per-drain menu timings and cache hit rates. |
| `ARLAND_FIELD_TRACE=1` | Logs the field-map character's state around each loss of footing. |
| `ARLAND_PRESENT_INTERVAL=0` | Forces vsync off (`1`, `2`, `3` present every Nth refresh). Useful with an external frame limiter. |
| `ARLAND_RESOLUTION_TRACE=1` | Traces the resolution override's effect on render targets. |
| `ARLAND_PRESENT_TRACE=1` | Reports how the finished frame reaches the screen. Needs a display resolution set. |

Most INI options also have an `ARLAND_`-prefixed override for a single session; those are noted alongside each option above.

## Logs and crash reports

Runtime messages are written to `arland-fix.log` in the game directory; the previous session's log is kept as `arland-fix.log.old`. If the game crashes, the mod appends a `CRASH` post-mortem to the log — the exception, the faulting address as module+offset, registers, and a stack scan — before the process exits. Include both files when reporting a problem.
