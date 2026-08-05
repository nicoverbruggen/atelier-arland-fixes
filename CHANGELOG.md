# Changelog

## v0.14

### Added

- **An option to skip the startup logos.** `SkipLogos=true` under `[Startup]` in `arland-fix.ini`, or the checkbox in the launcher. The logos play while the game loads, so skipping them shows a black screen for as long as the loading takes rather than getting you in sooner. It also stops the title screen replaying its demo when left idle. Off by default, all three games.

- **An option to skip the opening movie.** `SkipIntroMovie=true` under `[Startup]` goes straight to the title screen. The ending movies still play. Off by default, all three games.

- **The save data slots view now opens immediately.** It waited on a timer before appearing, and again before it would let you leave, and neither wait was checking anything. Holding the confirm button no longer carries that press into the view and opens the load prompt by itself. Set `[Menus] FastSaveMenu=false` for the original pacing. All three games.

### Fixed

- **The grey flash at startup is now black**, which is what the game fades up from anyway. All three games.

- **Combat no longer fails to start when you swing your staff at a monster.** A chasing monster covered the end of each step in a single frame, so on a high-refresh display it could cross the reach of your staff between two frames and the swing connected with nothing. That movement is spread over time now, and the monster still arrives in the same place at the same moment. Set `[Field] MonsterSnapFix=false` to turn it off.

- **The synthesis animation no longer runs too fast above 59.94 Hz.** The product cards played at the frame rate rather than at the rate they were drawn for, about two and a half times too fast at 144 Hz. Nothing changes at 60 Hz and below.

- **Logs record which build they came from**, so a log can be matched to the binary that wrote it.

- **Crash reports no longer blame the mod for faults in the system Direct3D driver.** The mod and the driver are both called `d3d11.dll`, and the report named the wrong one.

### Changed

- **Shadows now default to a 2048 map.** The engine draws every shadow in a scene into a single 1024 map, which looks coarse at 1440p and above. `[Rendering] ShadowMultiplier` defaults to `2`, and `1` restores the original. An `arland-fix.ini` that already has the key keeps its value.

- **The battle cut-in checkboxes are one setting with two states, and the upgraded cut-ins are opt-in.** **Classic**, which is what you get if you change nothing, leaves the close-up attack cameras as the game shipped them. **Enhanced** restores the ground shadows and holds the scene at full brightness. It is off by default because it changes how the game looks rather than repairing it, and because it is still being played through. The same `BattleCutInShadows` and `BattleCutInDimming` keys store the choice, so an existing `arland-fix.ini` is read exactly as before.

- **Rorona's restored ordinary-battle shadows no longer have a setting.** Rorona alone is missing the character and enemy shadows in the normal battle view, so putting them back is a fix and is always on. The `[Battle] BattleShadows` key is gone, and a line for it in an existing file is ignored. Totori and Meruru cast those shadows natively and are unaffected.

- **The launcher has been reorganized.** It is down to three tabs: **General**, **Graphics** and **About**. Everything that changes what you see is on Graphics, and the settings that were on their own tab have moved there. Several bugs were fixed along the way, the important one being that settings could fail to save while the window reported success.

## v0.12

### Fixed

- **Fixed Atelier Totori DX crashing when leaving a shop.** The shop could process an input update before it had selected a valid row and write just outside its item list, corrupting memory. Depending on the heap layout, the game then crashed either during shop cleanup or a few seconds later in seemingly unrelated work. The invalid write is now prevented while normal buying, selling and shop behavior remain unchanged.

- **Fixed a timing-dependent Atelier Totori DX crash when using bombs and other combat items.** The crash could happen while the battle camera was moving, sometimes before the item animation even began, and became more likely after several encounters. Totori queued temporary index and vertex stream state for its render worker without keeping that state alive until the worker used it, so effect or camera cleanup could leave the renderer with freed memory. Queued stream state is now retained until rendering has finished with it. This is separate from the malformed-item protection added previously: valid items and healthy saves could trigger this renderer-lifetime bug.

### Changed

- **Anisotropic filtering now defaults to 16x, and its launcher control has been removed.** It was 8x, and offered as a five-way dropdown. On any GPU able to run these ports the two are not measurably different, and the upgrade happens once at sampler creation rather than per frame, so the lower default preserved no trade-off, only a smaller improvement. With nothing left to trade, a control for it was only a question with one sensible answer, so the setting is applied and the row is gone. `AnisotropicFiltering` in `arland-fix.ini` still accepts `1` (off), `2`, `4`, `8` or `16` for troubleshooting; saving from the launcher normalises it to `16`, which is what migrates an existing file that still says `8`.

- **The quality presets lost their Low rung.** Low and Balanced had differed in anisotropic filtering alone, so with that setting no longer part of the ladder the two described exactly the same thing. Balanced is now the floor and remains the default. The Dusk launcher carries the same ladder, rung for rung and label for label.

- **The upgraded battle cut-ins are now on by default for new installations.** Both restorations shipped off while they were being playtested; they are now on by default, so the close-up attack cameras cast ground shadows and are no longer dimmed. Neither touches an existing choice: a `BattleCutInShadows` or `BattleCutInDimming` line already in your `arland-fix.ini` is read as before. To go back to the vanilla close-ups, set `BattleCutInShadows=false` and `BattleCutInDimming=true`, or untick the two boxes in the launcher.

- **Rorona's restored ordinary-battle shadows are now treated as a standard bug fix.** Rorona alone is missing character and enemy shadows in the normal battle view; Totori and Meruru already render them. The separate launcher checkbox has therefore been removed and the restoration remains enabled by default. This is distinct from the cut-in upgrades above, which apply to all three games and remain configurable. A manually edited `BattleShadows` setting is still honored for troubleshooting.

## v0.11

### Fixed

- **Much faster field interfaces in Atelier Totori DX.** The basket you open while gathering, and Totori's other field screens, still paused noticeably long after the menus had been fixed, which was very obvious on a Steam Deck. Totori builds those screens differently from the rest of the trilogy, outside the path the existing fix covered, so the font-texture cache did nothing for them and every piece of text was drawn the slow way. Drawing text now costs roughly a sixth of what it did, and up to twenty times less on the heaviest screens. Both the English and the Japanese/Chinese versions were checked for correct text; Rorona already worked this way, and Meruru does not need it.

### Changed

- **New defaults: your screen's resolution, in a borderless window.** These apply only where nothing has been chosen. A resolution or window mode already set in your `arland-fix.ini` is read exactly as before and left alone, so nothing changes for an existing setup. What they fix is the starting point: a fresh installation ran at the games' own 1280x720 until you opened the launcher and picked something, so anyone who simply dropped the DLL in got a 720p image on a 1440p screen. Blank `DisplayWidth`/`DisplayHeight` now means your desktop's current resolution, not the highest your monitor could manage, and `Borderless` now defaults to on, so the window fills the monitor with nothing scaled. Both remain settings: `DisplayWidth=game` follows the game's own resolution as before, and `Borderless=false` restores exclusive fullscreen.

## v0.10

### Added

- **An option to skip the launcher.** Once the mod is configured the way you want it, the launcher is a window you press one button in. Setting `SkipLauncher=true` under `[Launcher]` in `arland-fix.ini`, or ticking the box beside **Play with mod** in the launcher, makes Play in Steam go straight into the game with the settings already saved. Only the destination changes: the game is started by the same process at the same point in startup that the launcher would have been, with the same environment, so the Steam session, the overlay, playtime tracking and Steam Input behave exactly as they do without it, and the language you chose still decides which of the game's two executables runs. The launcher no longer opens by itself while this is set, so run `arland-fix-launcher.exe` from the game folder to change a setting or turn it back off. Off by default, and if no game is found beside the launcher the setting is ignored and Koei Tecmo's own launcher opens as before. See [ADVANCED.md](ADVANCED.md).

### Changed

- **Koei Tecmo's own settings program is no longer modified.** Earlier releases patched `ArlandDXEnv.exe` in memory so its resolution lists always offered 1920×1080, 2560×1440 and 3840×2160, because Windows display-mode reporting can hide modes the game and screen can perfectly well use. The mod's own launcher offers those resolutions directly and does not consult that list at all, so the patch has been removed and the stock settings program now runs exactly as it shipped. Resolution is set in the mod's launcher, or with `DisplayWidth`/`DisplayHeight` in `arland-fix.ini`. The 32-bit `msimg32.dll` is still needed: it is what opens the mod's launcher when you press Play in Steam.

### Fixed

- **Fixed the screen flashing with MSAA enabled, seen in Atelier Totori DX.** Several times a second, a single frame showed an image from seconds earlier. Totori's pre-UI SMAA pass could lose the deferred context's association between the multisample target and its visible host, causing the final command list to omit its resolve. The association is now restored after SMAA, and Present has a resource-directed safety resolve over the actual finished-frame surface. Rorona and Meruru were unaffected, as was any setup with supersampling enabled.

- **Prevented malformed Atelier Totori DX inventory data from corrupting saves and crashing combat, in both the English and multilingual executables.** Totori trusted a saved container limit even when it exceeded the game's fixed 999-item array, allowing ordinary item operations to write through unrelated game state and equipment. The mod now clamps that limit, repairs damaged item and character-skill records only when a selected save is actually loaded, bounds the remaining effect lookups used at battle entry and by bombs/Crafts, and rejects a malformed combat item whose id is absent from Totori's fixed action-item table instead of letting vanilla index before that table. When Totori parses saves to build its save list, the mod performs the same checks without changing the preview data and adds `[TBR]` ("to be repaired") to any slot that will be repaired when loaded. Runtime validation repaired an affected save—including its visibly broken skill lists—and the normally saved result passed a complete structural scan; an unaffected save remained clean. Rorona and Meruru use a different item system and explicitly leave this fix inactive.

## v0.9.1

### Fixed

- **Fixed styling of the launcher app on Windows.** On Windows, the launcher didn't really look that great. It should look better now.

## v0.9

### Added

- **A launcher for the mod**, `arland-fix-launcher.exe`. Every setting in one window, across Display, Image Quality, Game and About tabs, with five quality presets and a live render-resolution readout. It writes both `arland-fix.ini` and the game's own `ArlandDX_Settings.ini`, touching only the keys it owns, and configures whichever of the three games it sits beside, wearing that game's icon. **Play with mod** saves and launches in one step, running the executable that matches the chosen language: these games ship as two, an English build and a multilingual one, and the launcher picks between them exactly as Koei Tecmo's own does. **Play without the mod** starts the game with the mod stood down, so "is the mod causing this?" is one click rather than an uninstall, and Koei Tecmo's own launcher and settings editor remain one click away, unmodified. It fits a 720p screen, scales with the display's DPI, and carries its own font so it looks the same on Windows as under Wine.
- **The launcher opens automatically when the game is started from Steam.** Both of the games' 32-bit front-ends load `msimg32.dll`, so the mod's copy now recognises the launcher among them and opens `arland-fix-launcher.exe` in its place before the original puts anything on screen. Nothing needs configuring: drop the four files into the game folder and press Play. The launcher Steam started stays open behind the mod's own rather than being ended, and the substitution waits until the process is fully assembled, so the game still runs inside the session Steam is counting: the overlay, the frame-rate counter and Steam Input all keep working. If the launcher is not installed the stock one comes up exactly as before, so a partial install cannot leave the game unstartable. The mod's own buttons for the original tools bypass the substitution, so they always open the real thing.
- **Optional borderless windowed mode**. The games offer only a plain window with a title bar, or exclusive fullscreen, which takes control of the display and makes alt-tabbing slow, and interacts badly with compositors and multi-monitor setups under Wine and Proton. Setting `Borderless=true` in `arland-fix.ini` runs the game as a window with its decorations removed, sized to fill the monitor it is on: it looks like fullscreen, alt-tabs instantly, and leaves the display mode alone. The game's own `FullScreen` setting is ignored while it is on, so nothing needs changing in `ArlandDX_Settings.ini`. It fits displays that are not 16:9, such as a Steam Deck's 1280×800, by presenting the frame at its own shape with black bars rather than stretching it, and it presents through a flip-model swap chain so frames are handed to the desktop compositor rather than copied into it, which is what would otherwise cap a windowed game at half the display's refresh rate. Off by default. See [ADVANCED.md](ADVANCED.md).
- **Optional supersampling (internal render resolution)**. The resolution the game renders at and the resolution it presents at can now be set separately: `DisplayWidth`/`DisplayHeight` size the window, `RenderWidth`/`RenderHeight` size everything the game draws, and the larger frame is downscaled once just before it reaches the screen. Unlike MSAA or SMAA, which smooth edges after the fact, this resolves detail finer than a display pixel and sharpens menus and text with the scene. Every ratio resolves with a true box filter, and the frozen backgrounds behind shop and conversation menus are captured at the render resolution too. Capped at 7680×4320, past which these games have no more detail to give. Off by default and the most expensive option the mod offers, since cost scales with pixels drawn, so prefer raising the render resolution over stacking it with MSAA. See [ADVANCED.md](ADVANCED.md).
- **High-resolution UI text, on by default**. The games draw all UI text from a low-resolution pre-baked bitmap font, so menus and dialogue look soft at modern resolutions. The mod re-renders each string from a bundled scalable font at full resolution, keeping the game's own layout: line breaks, alignment, and multi-line panels like quest letters all match the original. A different font is embedded per game, so nothing needs installing, and glyphs it cannot provide (the controller-button icons) fall back to a sharpened version of the original rather than going missing. Three modes are available via `Font` in `arland-fix.ini`, and a `arland-hires-font.ttf` beside the DLL replaces the bundled one. English versions only; the Japanese and Chinese builds are unaffected. See [ADVANCED.md](ADVANCED.md).
- **SMAA anti-aliasing, on by default**. The games ship with no anti-aliasing; the mod now applies SMAA (a post-process pass) to smooth edges across the whole scene at a low, constant cost, including visible edges inside textures that geometry-only MSAA cannot touch. It runs before the UI is composited in all three games, so the HUD and text stay crisp, and can be combined with MSAA or used alone as a cheaper alternative. Totori exposes that boundary differently from Rorona and Meruru: it keeps the depth buffer attached but disables depth testing before its UI draws, which the mod detects directly. When optional MSAA is enabled in Totori, SMAA retains its full-frame fallback and therefore lightly affects the UI, because Totori keeps the multisample target bound across that transition. Set `SMAA=false` in `arland-fix.ini` to disable it.
- **Optional restored battle cut-in shadows and full-brightness close-ups in all three games**. The action cut-ins render without ground shadows and with a dimmed scene on every platform; the mod can keep the close-up lit and the shadows visible in Rorona, Totori, and Meruru. This required solving a stray-shadow glitch where the engine hides or repositions non-focus battlers during the close-up: the mod holds the shadow reception open only once the scene has settled, and forces the engine's own hide of a mid-cut-in battler to complete immediately so its shadow leaves with it. Restored on Totori too, whose D3D11-rewritten shaders route the dim and shadow reception through a different constant-buffer layout. Off by default pending wider playtest; enable `BattleCutInShadows` and `BattleCutInDimming` as described in [ADVANCED.md](ADVANCED.md).
- **Anisotropic filtering, on by default at 8x**. The games sample textures with plain linear filtering, so surfaces at a shallow angle (floors, walls, distant ground) look blurry. The mod upgrades the game's texture samplers to anisotropic filtering, keeping those surfaces sharp. The upgrade happens once at sampler creation, so it costs nothing per frame. `AnisotropicFiltering` in `arland-fix.ini` accepts `1` (off), `2`, `4`, `8` or `16`.
- **Configurable shadow-map resolution**. The games render shadows into a 1024×1024 map, so shadow edges can look blocky, most noticeably in Atelier Meruru DX. The mod can now render the shadow map at up to 8× its original resolution for sharper-edged shadows, allocating its own higher-resolution shadow maps and redirecting the shadow pipeline onto them while the game's own textures stay untouched. Off by default; higher multipliers increase GPU and video-memory cost. See [ADVANCED.md](ADVANCED.md) for configuration.
- **Crash logging**: if a game crashes, the mod appends a `CRASH` post-mortem to `arland-fix.log` before the process exits: the exception, the faulting address as module+offset, registers, and a stack scan. The report also classifies the faulting module (`AUDIO`, `GRAPHICS`, `GAME`, `MOD`, `SYSTEM`, `OTHER`) and flags when an audio module (XAudio2) appears in the call stack, so a crash the mod's D3D11 layer cannot fix, such as the Atelier Totori DX in-battle crash that begins with an audio screech, is identified as an audio-path fault rather than a rendering one. The previous session's log is also preserved as `arland-fix.log.old` instead of being overwritten on launch.
- **Clearer release logs**. `arland-fix.log` now identifies the exact mod version, game executable and configuration at startup, then reports which fixes installed successfully. Detailed hook addresses, render-resource operations and battle-state telemetry move behind `VerboseLogging`, duplicate activation records are collapsed, and failures from hot rendering paths are reported once by default rather than flooding the file. Warnings, failures, device loss and crash reports remain visible by default.

### Fixed

- **High-refresh displays now work properly.** On 120 Hz and 144 Hz displays the field-map character buzzed vertically whenever it stood still on a step or ledge, which was visible in the world and could make an interaction prompt, such as the one at the cauldron, flicker on and off. Above roughly 700 fps it went further and the character could not walk at all. Both come from the same place: the games discard any frame in which the character moves less than a fixed distance, and that distance is only correct at 60 fps, so the faster the game runs the more of its own movement it throws away. The mod now scales that distance with frame time, so it means a speed rather than a per-frame step, and separately holds the character still when it is genuinely at rest instead of letting gravity build up against the ground underneath it. The result is that the game plays the same at any refresh rate, with no frame-rate limit and nothing changed about how it presents frames. On by default and nothing to configure.
- **Fixed the world-map analog cursor moving too quickly at high refresh rates in Atelier Totori DX and Atelier Meruru DX.** Its movement was applied once per rendered frame, so it became increasingly fast above 60 fps. The mod now scales each step with frame time, preserving the original feel at 60 fps while keeping the same movement speed at higher refresh rates. Rorona uses a discrete location selector whose timing was already frame-rate independent, so it needs no corresponding correction.
- **Fixed garbled characters in menus on the Japanese and Chinese builds**. A rarely used character could render as stripes of garbage, reliably reproducible on a shop's buy menu, and stayed wrong for as long as the menu was open. The font-atlas read cache that removes the menu stutter took its snapshot from whichever mapping came first, and the renderer maps the atlas twice per glyph: once for writing, to draw the glyph, and once for reading, to copy it into the string. Taking the snapshot from the write mapping captured uninitialized memory, which the read was then served. Snapshots are now only taken from the read mapping. Latin text never showed this because its characters are resident long before any menu opens, whereas Japanese and Chinese page characters in continuously. The cache keeps working exactly as before, so the menu speed-up is unaffected.
- **The display resolution is now clamped to what the screen can actually show**. Setting `DisplayWidth`/`DisplayHeight` above the display's own resolution gained nothing, since the extra pixels were only scaled away again, so a larger value is now reduced to the display's and noted in the log. Rendering at a higher resolution than the screen is what `RenderWidth`/`RenderHeight` (supersampling) is for, and the config tool no longer offers base resolutions the display cannot show.
- **Fixed the resolution override being ignored depending on how the game creates its swap chain**. The games reach Direct3D by two different routes, and the override was only applied on one of them; on the other it silently did nothing, leaving the window at the launcher's resolution while the mod resized the internal render targets to the configured one. That mismatch is what made the background blur and depth-of-field tear when the configured resolution was higher than the launcher's. The override now applies on both routes, so the whole pipeline agrees on one resolution.
- **Fixed the field-map slowdown during Atelier Meruru DX's animated-portrait conversations**. The conversation balloon re-ran the executable's slow text-render path continuously, collapsing the framerate for the duration of the conversation; the mod now caches the rendered text across frames while a conversation balloon is on screen, so unchanged text costs a copy instead of a re-render.
- **Fixed a frame-rate drop after battles in Atelier Meruru DX**: the mod's battle-state tracking (used by the cut-in features) did not disengage when returning from battle to an already-loaded field map and kept scanning stale battle data every frame. Battle exit is now detected reliably in all three games.
- **Fixed the garbled window title on the Japanese executable under a non-Japanese locale**. The multilingual build passes a UTF-8 title to the ANSI window API; a non-UTF-8 system codepage (such as 1252, the usual case under Wine and Proton) decodes those bytes with the wrong table, so the title bar shows mojibake. The game's window is ANSI-classed, so the real Japanese cannot be shown on such a codepage at all (it collapses to `?` characters), so the mod instead substitutes a readable romanized title such as `Rorona no Atelier ~Arland no Renkinjutsushi~ DX`. Applied only when the game language is Japanese and the system codepage is neither Japanese (932) nor UTF-8 (65001), where the original title already renders; the Chinese and English builds are left untouched.

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
