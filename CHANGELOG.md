# Changelog

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
