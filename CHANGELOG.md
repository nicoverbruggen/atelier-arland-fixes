# Changelog

## v0.5

### Added

- Restored shadows during Atelier Rorona DX battle cut-ins. The action close-ups render without ground shadows and with a darker scene on every platform: the cut-in lowers the scene-light intensity below the receiver shader's shadow-reception threshold, which both dims the floor and switches shadow reception off for every object at once. The mod now holds that intensity up during cut-ins, so the scene keeps its brightness and the engine's own casters project real shadows again for characters and enemies alike. On by default. Basic and assist cut-ins gain shadows; solo specials that replace the whole background with a dedicated scene have no real floor on screen and are unchanged. See [TECHNICAL.md](TECHNICAL.md) for the mechanism.
- Battle shadow options are configurable through a new `[Battle]` section in `arland-fix.ini`. `BattleShadows` (default `true`) toggles the restored in-battle shadows; `BattleCutInShadows` (default `true`) toggles the restored cut-in shadows; and `BattleCutInDimming` (default `false`) restores the original cut-in scene dimming when set to `true`, independently of whether cut-in shadows are enabled. Missing keys are written with their defaults on launch so the options are discoverable. Each value can still be overridden per session with the corresponding `ARLAND_BATTLE_SHADOWS`, `ARLAND_CUTIN_SHADOWS`, and `ARLAND_CUTIN_DIMMING` environment variables.

## v0.4

### Added

- All game languages are now supported. The mod previously only recognized the English executables; it now also recognizes the multilingual executables that the launcher runs for Japanese, Simplified Chinese, and Traditional Chinese, in all three games. This covers the synchronization and menu fixes, higher-resolution rendering, optional MSAA, and the restored Rorona battle shadows. No configuration is required; the correct executable is detected automatically.

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
