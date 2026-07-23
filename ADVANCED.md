# Advanced configuration

The mod works as a drop-in with no configuration: faster menus, the
synchronization and text-corruption fixes, high-resolution rendering support,
and Atelier Rorona DX's restored battle shadows are all active by default. This
document covers the optional graphics tweaks, the on-by-default features you
may want to adjust, how to
combine them for the best experience, and the diagnostic switches. For
installation and the default feature set, see [README.md](README.md).

All settings live in `arland-fix.ini` beside the DLLs (created on first
launch). Close the game before editing the file.

## Wine and Proton

Copy both DLLs into the game directory as described in the README, then add
this to the game's Steam Launch Options:

```text
WINEDLLOVERRIDES="d3d11,msimg32=n,b" %command%
```

The same `arland-fix.ini` file used on Windows configures the mod under Wine
or Proton. Runtime messages are written to `arland-fix.log` in the game
directory.

## Direct resolution override

The launcher DLL always exposes 1920×1080, 2560×1440, and 3840×2160 in both
launcher states even when DPI or display-mode enumeration would normally hide
them. Keeping 1920×1080 visible is intentional for Steam Deck and other
lower-resolution handhelds, where a high-DPI desktop can otherwise prevent the
launcher from exposing 1080p; it also supports higher-resolution rendering for
downsampling and normal docked use. As a launcher-independent fallback,
`arland-fix.ini` can also override the resolution used by the game. Both
values must be present; blank values leave the launcher's selection unchanged:

```ini
[Rendering]
Width=3840
Height=2160
```

## SMAA anti-aliasing

SMAA (a post-process anti-aliasing pass) is **on by default**. It smooths edges
across the whole scene — including the thin alpha-tested costume trim that MSAA
cannot touch — at a low, constant cost, and is applied before the UI is drawn
so the HUD and text stay crisp. Turn it off with:

```ini
[Rendering]
SMAA=false
```

`ARLAND_SMAA` overrides the INI for a session. SMAA and MSAA can be combined
(SMAA cleans up what MSAA's geometry-only multisampling leaves), or SMAA can be
used alone as a much cheaper alternative to MSAA. Note: the very fine lace trim
is sub-pixel alpha-test detail that no post-process (SMAA included) can fully
resolve — only rendering at a higher internal resolution does.

## MSAA

Multisample anti-aliasing is disabled by default. `MSAA` in the `[Rendering]`
section requests a sample count of `2`, `4`, or `8`:

```ini
[Rendering]
MSAA=4
```

If the GPU or selected format does not support the requested count, the mod
falls back to a lower supported count. Use `0` or `1`, or remove the setting
entirely, to disable MSAA. Higher sample counts increase GPU and video-memory
cost.

## Trim anti-aliasing (alpha-to-coverage)

Thin costume trim — the frilled lace on sleeves, collars, and skirts, and the
hair fringe — is drawn with a hard alpha-test cutout against a low-resolution
1-bit-alpha texture, so its edges stay jagged even with MSAA (the aliasing is
inside the texture, not on the model silhouette, which is all MSAA smooths).

`TrimAntiAliasing` enables alpha-to-coverage for those character materials,
which dithers the cutout edges into the multisample coverage mask:

```ini
[Rendering]
MSAA=4
TrimAntiAliasing=true
```

It only takes effect when MSAA is active (it works through the multisampled
render target), and it is scoped to the character trim shaders alone — UI,
fonts, and effects are untouched. The improvement is bounded by the source
textures' 1-bit alpha, so expect noticeably smoother trim edges rather than a
perfectly clean line. Off by default. `ARLAND_TRIM_A2C` overrides the INI.

## Anisotropic filtering

The games sample their textures with plain linear filtering, so surfaces seen at
a shallow angle — floors, walls, the ground stretching into the distance — look
blurry. `AnisotropicFiltering` upgrades that to anisotropic filtering, which
keeps those oblique surfaces sharp:

```ini
[Rendering]
AnisotropicFiltering=16
```

Accepted values are `2`, `4`, `8`, and `16` (the maximum-anisotropy level); any
other value, or the setting removed, leaves the game's original filtering. The
mod upgrades the game's texture samplers at creation, so all world and character
textures benefit with no per-frame cost; shadow-comparison samplers are left
untouched. Off by default. `ARLAND_ANISO` overrides the INI.

## Shadow resolution multiplier

The games render shadows into a 1024×1024 shadow map, so shadow edges can look
blocky, most noticeably in Atelier Meruru DX. `ShadowMultiplier` in the
`[Rendering]` section renders shadows at a higher internal resolution for
crisper edges:

```ini
[Rendering]
ShadowMultiplier=2
```

Accepted values are `1` (the default, unchanged 1024×1024), `2`, `4`, and `8`,
which render the shadow map at 2048, 4096, and 8192 respectively; any other
value falls back to `1`. The mod allocates its own higher-resolution shadow
maps and redirects the shadow pipeline onto them, leaving the game's own
1024×1024 textures untouched so the engine's size and memory assumptions stay
valid. At `1` the mod does not touch the shadow pipeline at all.

Higher multipliers increase GPU and video-memory cost substantially: `8` is
the heaviest setting and its stability under long play sessions is still being
validated — prefer `2` or `4`.

## Battle shadows

Atelier Rorona DX omitted all character and enemy shadows during ordinary
battle; the mod restores them, enabled by default. `BattleShadows` in the
`[Battle]` section toggles the restoration (Meruru already casts these
natively, and the toggle does not affect Atelier Totori DX):

```ini
[Battle]
BattleShadows=true
```

## Battle cut-in shadows and brightness

During the battle action cut-ins (the close-up attack cameras), the games show
no ground shadows on any platform and dim the scene. The mod restores the
shadows and keeps the close-up at full brightness in all three games, **on by
default**. Both are controlled from the `[Battle]` section if you prefer the
vanilla look:

```ini
[Battle]
BattleCutInShadows=true
BattleCutInDimming=false
```

`BattleCutInShadows` (default `true`) restores the ground shadows during
cut-ins; set it to `false` for the vanilla shadowless close-up.

`BattleCutInDimming` (default `false`) controls the original close-up dimming;
set it to `true` to restore the vanilla darkened cut-in. The two options are
independent and apply to all three games.

## Suggested "best experience" configuration

On a GPU with headroom, this is the configuration the optional features were
designed for:

```ini
[Rendering]
MSAA=4
Width=
Height=
ShadowMultiplier=2

[Battle]
BattleShadows=true
BattleCutInShadows=true
BattleCutInDimming=false
```

Raise `MSAA` to `8` and `ShadowMultiplier` to `4` on strong hardware. (Battle
shadows and the cut-in restorations are on by default; they are listed here
only for completeness.)

## Logs and crash reports

Runtime messages are written to `arland-fix.log` in the game directory; the
previous session's log is kept as `arland-fix.log.old`. If the game crashes,
the mod appends a `CRASH` post-mortem to the log — the exception, the faulting
address as module+offset, registers, and a stack scan — before the process
exits. Include both files when reporting a problem.
