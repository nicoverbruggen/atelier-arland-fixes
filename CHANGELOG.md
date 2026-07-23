# Changelog

## v0.5

### Added

- Extended the restored battle cut-in shadows to Atelier Meruru DX. Meruru casts its battle shadows normally in ordinary combat but lost them during the action close-ups for the same reason as Rorona—the cut-in drops the scene light below the shadow-reception threshold—so the same restoration now applies there. It is governed by the same `[Battle]` `BattleCutInShadows` and `BattleCutInDimming` settings. Both cut-in options are **off by default for now** (vanilla cut-in behavior) while a visual glitch is investigated: the engine sometimes hides or repositions the non-focus characters during the close-up, which can leave a ground shadow standing where its character is not.
- Configurable shadow-map resolution through `ShadowMultiplier` in the `[Rendering]` section of `arland-fix.ini`. The games render shadows into a 1024×1024 map, so shadow edges can look blocky, most noticeably in Atelier Meruru DX. Setting `ShadowMultiplier` to `2`, `4`, or `8` renders the shadow map at 2048, 4096, or 8192 for sharper-edged shadows. The mod allocates its own higher-resolution shadow maps and redirects the shadow pipeline onto them, leaving the game's own 1024×1024 textures untouched so the engine's size and memory assumptions stay valid. Default `1` (unchanged). Higher multipliers increase GPU and video-memory cost.
- Crash reporting: if a game crashes, the mod appends a `CRASH` post-mortem to `arland-fix.log`—the exception, the faulting address as module+offset, registers, and a stack scan—before the process exits. The previous session's log is also preserved as `arland-fix.log.old` instead of being overwritten on launch.

### Fixed

- Fixed the field-map slowdown during Atelier Meruru DX's animated-portrait conversations on the English executable. The conversation balloon re-ran the executable's slow text-render path continuously, collapsing the framerate for the duration of the conversation; the mod now caches the rendered text across frames while a conversation balloon is on screen, so unchanged text costs a copy instead of a re-render.
- Fixed a frame-rate drop after battles in Atelier Meruru DX: the mod's battle-state tracking (used by the cut-in features) did not disengage when returning from battle to an already-loaded field map and kept scanning stale battle data every frame. Battle exit is now detected reliably in all three games.

### Changed

- Documentation restructure: the README now covers only the drop-in installation and default fixes; the optional enhancements and their `arland-fix.ini` settings moved to `ADVANCED.md`, and build instructions moved to `BUILDING.md`. Configuration is documented exclusively through `arland-fix.ini`.

## v0.4

### Added

- All game languages are now supported. The mod previously only recognized the English executables; it now also recognizes the multilingual executables that the launcher runs for Japanese, Simplified Chinese, and Traditional Chinese, in all three games. This covers the synchronization and menu fixes, higher-resolution rendering, optional MSAA, and the restored Rorona battle shadows. No configuration is required; the correct executable is detected automatically.
- Restored shadows during Atelier Rorona DX battle cut-ins. The action close-ups render without ground shadows and with a darker scene on every platform: the cut-in lowers the scene-light intensity below the receiver shader's shadow-reception threshold, which both dims the floor and switches shadow reception off for every object at once. The mod now holds that intensity up during cut-ins, so the scene keeps its brightness and the engine's own casters project real shadows again for characters and enemies alike. On by default. Basic and assist cut-ins gain shadows; solo specials that replace the whole background with a dedicated scene have no real floor on screen and are unchanged. See [TECHNICAL.md](TECHNICAL.md) for the mechanism.
- Battle shadow options are configurable through a new `[Battle]` section in `arland-fix.ini`. `BattleShadows` (default `true`) toggles the restored in-battle shadows; `BattleCutInShadows` (default `true`) toggles the restored cut-in shadows; and `BattleCutInDimming` (default `false`) restores the original cut-in scene dimming when set to `true`, independently of whether cut-in shadows are enabled. Missing keys are written with their defaults on launch so the options are discoverable. Each value can still be overridden per session with the corresponding `ARLAND_BATTLE_SHADOWS`, `ARLAND_CUTIN_SHADOWS`, and `ARLAND_CUTIN_DIMMING` environment variables.

### Fixed

- The restored Rorona battle shadows no longer depend on the frame atlas cache: disabling the cache with `ARLAND_FRAME_ATLAS_CACHE=0` previously also disabled the shadows.

## v0.3

### Added

- Characters and enemies now cast shadows during battles in Atelier Rorona DX. The port never registered battle shadow casters, so battles rendered without them; the restored shadows use the game's own shadow pipeline. Set `ARLAND_BATTLE_SHADOWS=0` to disable. (The action-selection close-up and attack cut-ins never had shadows on any platform and are unchanged.)

### Fixed

- Even faster text-heavy menus in all three games. Especially the synthesis menu is much faster.

## v0.2

### Added

- Native 2560×1440 and 3840×2160 rendering.
- Launcher options for 1920×1080, 2560×1440, and 3840×2160 in windowed and fullscreen mode.
- Optional 2×, 4×, and 8× MSAA with automatic fallback to supported sample counts.
- Optional launcher-independent resolution overrides through `arland-fix.ini`.

### Fixed

- Corrupted text when using the performance fixes on native Windows in Rorona, Totori, and Meruru.

## v0.1

### Added

- Initial release for the English Steam versions of Atelier Rorona DX, Atelier Totori DX, and Atelier Meruru DX.
- Reduced graphics-related stalls without corrupting rendered text.
- Significantly faster text-heavy menus in all three games.
