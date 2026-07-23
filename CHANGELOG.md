# Changelog

## v0.5

### Added

- **High-resolution UI text, on by default**. The games draw all UI text from a low-resolution pre-baked bitmap font, so menu and dialog text looks soft and pixelated at modern resolutions. The mod now re-renders each string from a bundled scalable font (Cuprum) at full resolution, keeping the game's own layout—line breaks, alignment, and multi-line panels like quest letters and character bios all match the original, laid out on the engine's real line metrics. The replacement font is embedded in the DLL, so nothing extra needs to be installed. Glyphs the font can't provide—such as the game's custom controller-button icons—fall back automatically to a sharpened version of the original, so nothing is ever left pixelated or missing. Three modes are available via `Font` in the `[Rendering]` section of `arland-fix.ini`: `replaced` (the default, the bundled font), `upscaled` (smooth the original baked glyphs while keeping the exact original look), or `default` (the untouched original font). To use a different replacement font, drop a `arland-hires-font.ttf` next to the DLL and it takes over. English versions only; the Japanese and Chinese builds are unaffected. See [ADVANCED.md](ADVANCED.md).
- **SMAA anti-aliasing, on by default**. The games ship with no anti-aliasing; the mod now applies SMAA (a post-process pass) to smooth edges across the whole scene at a low, constant cost—including the thin alpha-tested costume trim that MSAA cannot touch, because MSAA only multisamples geometry silhouettes. It runs before the UI is composited, so the HUD and text stay crisp, and can be combined with MSAA or used alone as a cheaper alternative. On Atelier Totori DX, whose renderer composites the scene and UI into a single render target with no separable pre-UI point, SMAA instead runs as a full-frame pass at present time, so it also lightly affects that game's UI. Set `SMAA=false` in `arland-fix.ini` to disable it. (The very finest lace trim is sub-pixel detail no post-process can fully resolve; only higher-resolution rendering does.)
- **Restored battle cut-in shadows and full-brightness close-ups in all three games, on by default**. The action cut-ins render without ground shadows and with a dimmed scene on every platform; the mod now keeps the close-up lit and the shadows visible in Rorona, Totori, and Meruru. This required solving a stray-shadow glitch where the engine hides or repositions non-focus battlers during the close-up: the mod holds the shadow reception open only once the scene has settled, and forces the engine's own hide of a mid-cut-in battler to complete immediately so its shadow leaves with it. Restored on Totori too, whose D3D11-rewritten shaders route the dim and shadow reception through a different constant-buffer layout. Set `BattleCutInShadows`/`BattleCutInDimming` in [ADVANCED.md](ADVANCED.md) for the vanilla look.
- **Optional anisotropic filtering**. The games sample textures with plain linear filtering, so surfaces at a shallow angle (floors, walls, distant ground) look blurry. Setting `AnisotropicFiltering` to `2`, `4`, `8`, or `16` in `arland-fix.ini` upgrades the game's texture samplers to anisotropic filtering, keeping those surfaces sharp at no per-frame cost. Off by default.
- **Configurable shadow-map resolution**. The games render shadows into a 1024×1024 map, so shadow edges can look blocky, most noticeably in Atelier Meruru DX. The mod can now render the shadow map at up to 8× its original resolution for sharper-edged shadows, allocating its own higher-resolution shadow maps and redirecting the shadow pipeline onto them while the game's own textures stay untouched. Off by default; higher multipliers increase GPU and video-memory cost. See [ADVANCED.md](ADVANCED.md) for configuration.
- **Crash logging**: if a game crashes, the mod appends a `CRASH` post-mortem to `arland-fix.log`—the exception, the faulting address as module+offset, registers, and a stack scan—before the process exits. The report also classifies the faulting module (`AUDIO`, `GRAPHICS`, `GAME`, `MOD`, `SYSTEM`, `OTHER`) and flags when an audio module (XAudio2) appears in the call stack, so a crash the mod's D3D11 layer cannot fix—such as the Atelier Totori DX in-battle crash that begins with an audio screech—is identified as an audio-path fault rather than a rendering one. The previous session's log is also preserved as `arland-fix.log.old` instead of being overwritten on launch.

### Fixed

- **Fixed the field-map slowdown during Atelier Meruru DX's animated-portrait conversations**. The conversation balloon re-ran the executable's slow text-render path continuously, collapsing the framerate for the duration of the conversation; the mod now caches the rendered text across frames while a conversation balloon is on screen, so unchanged text costs a copy instead of a re-render.
- **Fixed a frame-rate drop after battles in Atelier Meruru DX**: the mod's battle-state tracking (used by the cut-in features) did not disengage when returning from battle to an already-loaded field map and kept scanning stale battle data every frame. Battle exit is now detected reliably in all three games.

## v0.4

### Added

- **All game languages are now supported**. The mod previously only recognized the English executables; it now also recognizes the multilingual executables that the launcher runs for Japanese, Simplified Chinese, and Traditional Chinese, in all three games. This covers the synchronization and menu fixes, higher-resolution rendering, optional MSAA, and the restored Rorona battle shadows. No configuration is required; the correct executable is detected automatically.
- **Restored shadows during Atelier Rorona DX battle cut-ins**. The action close-ups render without ground shadows and with a darker scene on every platform: the cut-in lowers the scene-light intensity below the receiver shader's shadow-reception threshold, which both dims the floor and switches shadow reception off for every object at once. The mod now holds that intensity up during cut-ins, so the scene keeps its brightness and the engine's own casters project real shadows again for characters and enemies alike. On by default. Basic and assist cut-ins gain shadows; solo specials that replace the whole background with a dedicated scene have no real floor on screen and are unchanged. See [TECHNICAL.md](TECHNICAL.md) for the mechanism.
- **Battle shadow options are configurable**: the restored in-battle shadows, the restored cut-in shadows, and the original cut-in scene dimming can each be toggled independently. See [ADVANCED.md](ADVANCED.md) for configuration.

## v0.3

### Added

- **Characters and enemies now cast shadows during battles in Atelier Rorona DX**. The port never registered battle shadow casters, so battles rendered without them; the restored shadows use the game's own shadow pipeline. (The action-selection close-up and attack cut-ins never had shadows on any platform and are unchanged.)

### Fixed

- **Even faster text-heavy menus in all three games**. Especially the synthesis menu is much faster.

## v0.2

### Added

- **Added proper support for high resolution 3D rendering.** The games accept 2560×1440 and 3840×2160 but leave internal render targets and raster state at 1920×1080; the mod now renders the full pipeline natively at the selected resolution. The settings launcher always offers 1920×1080, 2560×1440, and 3840×2160 in both windowed and fullscreen mode, and the resolution can also be overridden independently of the launcher (see [ADVANCED.md](ADVANCED.md)).
- **Added optional anti-aliasing with 2×, 4×, and 8× MSAA options.** This ensures that the model's jagged edges are smoothed over. Higher settings have a larger GPU impact, so keep that in mind.

### Fixed

- **Fixed corrupted text** when using the performance fixes on native Windows in Rorona, Totori, and Meruru.

## v0.1

### Added

- **Initial release** for the English Steam versions of Atelier Rorona DX, Atelier Totori DX, and Atelier Meruru DX.
- **Reduced graphics-related stalls** without corrupting rendered text.
- **Significantly faster text-heavy menus** in all three games.
