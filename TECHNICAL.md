# Technical overview

> [!NOTE]
> This technical overview is maintained and kept up-to-date with a large language model to ensure it matches the source code, as such, it wasn't all personally written by the author, who just did some minor editing here. As a player you may find the TL;DR sections interesting.

## Historical background

This repository combines established synchronization work with new Arland-specific research. The components should not be conflated:

- Philip Rebohle created [`atelier-sync-fix`](https://github.com/doitsujin/atelier-sync-fix) in 2022. Its central technique, replacing eligible GPU-to-CPU copies with copies through CPU-accessible shadow resources, is the foundation of `src/sync_fix.cpp`. The proxy loading, MinHook-based native D3D11 interception, staging-resource access correction, and direct-source unmap fixes also originate there.
- TellowKrinkle identified that direct game writes through `Map` and `Unmap` must update the shadow and implemented that correction for Atelier Ayesha in [commit `98b5c9b` of the `atelier-sync-fix` fork](https://github.com/TellowKrinkle/atelier-sync-fix/commit/98b5c9bdb934fa2d74ad17c026bb50598d522cc6). That implementation stored one global last mapping and uploaded the complete resource on every `Unmap`.
- This project refines the Map/Unmap solution for the Arland workload: mappings are keyed by resource and subresource, references are lifetime-safe, dirty shadows are coalesced, uploads are deferred until the GPU can observe the resource, and deferred contexts cannot perform invalid staging reads. This refinement fixed the corrupted-text case encountered during the investigation while avoiding thousands of redundant atlas uploads.
- [TellowKrinkle's rendering fork](https://github.com/TellowKrinkle/atelier-sync-fix) also established the old-Arland render-target and viewport/scissor correction ported into this project. The released configuration retains that resolution logic, and keeps the fork's multisampling opt-in; the anti-aliasing that is on by default is this project's own SMAA, as is the anisotropic filtering. The fork's shader-replacement and LOD-bias features are not included.
- Nico Verbruggen, the author of this repository, led the reverse-engineering and runtime investigation behind the Arland-specific work in this project: the menu-stutter fix (the `.PSSG` validation cache and font-atlas read caches), the battle and cut-in shadow restoration, and the high-resolution UI font rendering. That work was carried out with the assistance of large language models. None of it is part of the original `atelier-sync-fix`.
- Yuri Hime's [Atelier Graphics Tweak](https://steamcommunity.com/app/1152300/discussions/0/3345546664208090238/), together with the earlier [Rorona community investigation](https://steamcommunity.com/app/936160/discussions/0/1742227264210806751/?ctp=2), identified the broader font-atlas GPU-transfer problem that this project's synchronization and atlas-cache work addresses. AGT's experimental upload-suppression approach is examined here and not used; see "Relationship to Atelier Graphics Tweak" below.
- [MinHook](https://github.com/TsudaKageyu/minhook) is an independent library by Tsuda Kageyu and contributors, bundled unchanged under `vendor/minhook`.
- The high-resolution UI text feature rasterizes glyphs with [stb_truetype](https://github.com/nothings/stb) (Sean Barrett, public domain), vendored unchanged under `vendor/stb`. Its bundled replacement typefaces are chosen per game in code rather than by a setting: National Park SemiBold (Rorona) and Nunito Regular (Totori), both under the SIL Open Font License, and Cosmetica Medium (Meruru), an emboldened MgOpen Cosmetica under the MgOpen licence and renamed as that licence requires. All are embedded in the DLL (generated at build time from the vendored `.ttf` files under `vendor/font/` by `scripts/embed_font.py`, so no large byte arrays are checked in); a `arland-hires-font.ttf` placed beside the DLL overrides them. `[Rendering] Font` selects the mode, not the face.
- [SMAA](https://github.com/iryoku/smaa) is by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, and Diego Gutierrez (MIT). Its reference shader and the precomputed `AreaTex`/`SearchTex` lookup textures are vendored unchanged under `vendor/smaa`; the mod adds only the runtime integration (compilation, the three-pass pipeline, and the pre-UI injection). The SMAA preset (`ULTRA`) and injection approach follow Yuri Hime's Atelier Graphics Tweak, which shipped the same SMAA for these games.

The current code supports the exact tested Arland DX executables (the English builds and the multilingual builds used for Japanese and Chinese) and contains the validated D3D11 synchronization and menu-performance fixes described below.

## D3D11 synchronization stalls

### TL;DR

The games often send font textures to the graphics card and then immediately ask for the same data back, forcing the game to stop and wait. The mod keeps a CPU-readable copy synchronized with the real texture. That removes the round trip, and both the game and the graphics card still see the latest text.

### Safety

Nothing here changes what the game draws, only how it gets back data it asked for. The shortcut is taken only for copies the mod fully recognizes: an unfamiliar texture format or layout, a busy destination, or a copy made from a background context falls through to the graphics driver's own path untouched. The game also writes to these textures directly, so every one of those writes is tracked and the finished result reaches the graphics card before anything is drawn with it. That is what keeps the text legible. There is one case the mod refuses to touch at all: a font texture the game fills through a queued command list before the mod has a copy of it. Guessing at what is in that texture is how glyphs end up scrambled, so those copies stay on the original path.

### Details

The Arland ports frequently copy GPU resources into CPU-readable staging resources and then map them. A normal D3D11 `CopyResource` followed by `Map` forces the CPU to wait for the GPU. Font-atlas activity makes this especially visible while constructing text-heavy menus.

The original algorithm, retained here, first examines both resources involved in `CopyResource` or `CopySubresourceRegion`. When the destination is immediately CPU-writable and the source is not CPU-readable, it creates a staging shadow for the source with both `D3D11_CPU_ACCESS_READ` and `D3D11_CPU_ACCESS_WRITE`, initializes it from the real resource, and stores it as private data on that resource. Compatible later copies map the destination and the source shadow and use row- and depth-pitch-aware CPU memory copies. Unsupported formats, layouts, contexts, or busy destinations fall back to the real D3D11 copy.

The optimization is therefore narrow. Ordinary texture uploads, decompression, shader execution and asset loading are untouched; what goes away is the round trip where the game schedules GPU work and then immediately blocks the CPU to retrieve the result.

The original shadow-copy technique has an important coherence problem in these games: they also update the real font atlas directly through `Map` and `Unmap`. If those writes do not reach the shadow, later CPU copies read stale pixels and text becomes corrupted.

This implementation redirects writes for shadowed immediate-context resources into the shadow using `D3D11_MAP_READ_WRITE`, preserving atlas pixels that the game does not rewrite during a nominal `WRITE_DISCARD` operation. `Unmap` marks that exact resource/subresource dirty instead of uploading immediately. Before a draw, dispatch, GPU-backed copy, or `ExecuteCommandList`, one pitch-aware upload updates the real resource. Multiple atlas mutations before that point collapse into one upload.

The bookkeeping is deliberately stricter than the earlier single-map implementation:

- active maps are keyed by `(ID3D11Resource*, subresource)` rather than stored in one global slot;
- both active mappings and pending uploads retain COM references for their complete lifetimes;
- internal maps call the original D3D11 entry points, preventing recursion through our own hooks;
- only the immediate context redirects and flushes staging mappings;
- execution of a deferred command list on the immediate context is a flush boundary;
- a shadow that is still mapped remains pending rather than being uploaded prematurely.

The game can therefore update mutable atlases without producing stale CPU reads, while the GPU sees the newest completed shadow contents before rendering.

Native Windows exposed one additional ordering hazard. The games can populate a dynamic 512×512 font atlas through deferred command submission before the synchronization layer creates its CPU shadow. Treating that atlas as an ordinary CPU-copy source can snapshot stale contents and then preserve them, producing consistently scrambled glyphs even though the later Map/Unmap tracking is correct. The fix identifies dynamic 512×512 texture sources and leaves those copies on the native D3D11 path. This safeguard lives in the shared D3D11 layer installed for each recognized Rorona, Totori, and Meruru executable; it is not Rorona-specific. The queue-scoped atlas-read cache still removes thousands of redundant reads at the game-code layer, so correctness does not require sacrificing the dominant menu optimization.

## Repeated PSSG validation

### TL;DR

When opening a menu, the games repeatedly ask Windows to verify the same few resource files, sometimes thousands of times. The mod remembers successful checks and reuses the answer, removing much of the menu-opening delay without skipping any actual file loading or hiding missing files.

### Safety

Only successful checks are remembered, and only for the paths of the game's own resource archives, which cannot change while the game is running. Failures are never remembered, so a file that appears later is still found. The game's own check still runs the first time, and nothing about opening, reading or unpacking the archives changes. What is stored is the path text itself, not the file, its contents, or anything built from it, and it is forgotten when the game closes.

### Details

The English Arland executables repeatedly validate identical immutable `.PSSG` archive paths while recursively building UI records. Rorona and Meruru perform a metadata lookup followed by a filename-case directory enumeration; Totori repeatedly performs its corresponding metadata validation.

During one unmodified Rorona Status-menu build, only `a11r_menu_EN.PSSG` and `a11r_common_EN.PSSG` were validated 1,245 redundant times. The operating-system file cache does not help much here: the cost is in the repeated path conversion, opens, metadata queries, directory enumeration, Wine/NT transitions and handle teardown, all of which happen whether or not the file itself is cached.

The mod detours the complete game-side validation helper rather than a Windows file API. The original helper runs for the first check. A successful `.PSSG` path is remembered for the process lifetime, and later checks of that exact path return success. Failures are never cached, so resources that appear later can still be discovered. Non-PSSG paths, archive reads, parsing, decompression, and UI ownership remain unchanged.

What the cache holds is path strings, and nothing else: no file handles, no metadata structures, no PSSG contents, no constructed UI graphs. That matches the invariant it relies on. A shipped PSSG archive path cannot become invalid during one run of the game, whereas the parsed UI objects stay mutable and menu-owned, so those are left where they are.

These desktop measurements were captured on an AMD Ryzen 7 5800X3D, Radeon RX 7900 XTX, and 32 GiB of RAM. On Steam Deck, affected menus were observed taking roughly 4–7 seconds without the fix.

| Rorona workload | Before PSSG cache | After PSSG cache | Saving |
|---|---:|---:|---:|
| Status menu | 916.6 ms | 103.1 ms | 88.8% |
| Quest/Container/Basket test | 632–711 ms | 135.6 ms | 78.5–80.9% |

## Repeated font-atlas reads

### TL;DR

While building a menu, the games read the same three font textures over and over even when nothing has changed. The mod takes one temporary copy and reuses it for the rest of that safe menu or frame window, discarding it as soon as the texture may have changed.

### Safety

The reused copy is short-lived by design: it lasts one burst of menu building, or in Rorona a single frame, and the frame boundary throws it away. If the game genuinely touches one of these textures, that texture's copy is discarded on the spot. A write the game asked for is never skipped. That was the shortcut a previous community tool took, and it produced missing text and a crash. The reuse applies only to the exact 512x512 font textures, and only while the game's verified text renderer is the thing asking.

### Details

After the PSSG fix, the games still read the same three 512×512 font atlases once per text operation. A representative Rorona Status build made about 3,642 candidate reads even though the atlases did not change during that synchronous build. Totori and Meruru use the same middleware behavior.

The shared atlas cache performs the first real read of each candidate atlas, takes a CPU snapshot, and serves later reads from that snapshot. It is restricted to 512×512 atlas locks made from each game's verified text renderer. Totori and Meruru currently keep snapshots within one invocation of their resource-event queue drain.

Rorona performs another large batch through the same verified atlas-lock path before entering the queue drain. A repeated Synthesis transition made 864 synchronous `D3D11_MAP_READ` calls from RVA `0x3ee976` across 62 staging textures. Every resource was a 512×512 `DXGI_FORMAT_B8G8R8A8_TYPELESS` texture with CPU read/write access. The calls consumed about 200–211 ms per open, alongside 865 GPU copies and 1,695 resource creations. Rorona therefore retains eligible snapshots until the next `Present`, covering the complete blocked menu-construction frame. A real non-cached lock invalidates that texture immediately, and every snapshot is discarded at the frame boundary; mutable atlases are never cached for the process lifetime.

| Workload after PSSG caching | Without atlas cache | With atlas cache | Saving |
|---|---:|---:|---:|
| Rorona Status queue drain | 103–117 ms | 67–69 ms | 34–43% |
| Totori 2,331-read menu drain | 119.7–126.3 ms | 94.0–99.8 ms | About 21% |
| Meruru Status queue drain | 82–87 ms | 45–46 ms | 44–48% |
| Rorona Synthesis full transition | 367–386 ms | 134–145 ms | 61–65% |

In the Totori measurement, 2,328 of 2,331 candidate reads were served from snapshots. In repeated Meruru Status-class drains, 3,027 of 3,030 reads were served from snapshots. Both therefore reduced the operation to three real atlas reads, matching Rorona.

Two simpler lifetimes were tried and dropped. A process-lifetime snapshot is unsound, since the atlases are mutable. Clearing the cache for every rendered string is sound but slow: the same operation regressed to roughly 2.5 seconds. The frame-scoped Rorona path reduced a repeated Synthesis open from 2,822 Maps and 865 copies to about 230 Maps and one copy; measured Map time fell from roughly 200–211 ms to 0.15–0.19 ms.

#### Relationship to Atelier Graphics Tweak

The community identified the broad transfer problem years before this project. The original [Rorona menu-loading investigation on Steam](https://steamcommunity.com/app/936160/discussions/0/1742227264210806751/?ctp=2) describes the game rendering individual characters through 512×512 CPU textures, compositing them through video memory, and moving gigabytes during a bad menu frame. Yuri Hime's later [Atelier Graphics Tweak discussion](https://steamcommunity.com/app/1152300/discussions/0/3345546664208090238/) introduced an experimental anti-stutter option intended to remove that traffic.

Static analysis of the archived AGT `dinput8_antistutter.dll` shows that it hooks D3D11 texture creation, `Map`, and `Unmap`, recognizes the 512×512 dynamic `B8G8R8A8` font-atlas write path, and prevents selected transfers from executing. This can be extremely fast, but it suppresses uploads without proving that their contents are redundant. The AGT author documented missing text and a reproducible boss-battle crash, and later withdrew the optimization after a user-reported crash.

This repository does not copy that suppression behavior. It hooks the verified game-side text renderer and atlas lock/unlock routines, performs the first required read, and replays an immutable CPU snapshot for later reads within a bounded lifetime. It never suppresses a required atlas write: an unmatched real lock invalidates the affected texture, and Rorona discards every snapshot at the next frame boundary. The `.PSSG` validation cache, queue-scoped atlas cache, and Rorona frame-scoped extension were independently derived from runtime traces during this project's menu-hitch investigation. AGT remains important prior work for identifying the general GPU-transfer problem and demonstrating its potential performance impact.

An additional trace tested whether AGT-style upload avoidance could usefully extend the frame-scoped fix. Across seven repeated Rorona Synthesis openings, each 132–144 ms transition issued about 17 MB of dynamic texture writes, but the D3D11 write-map calls themselves averaged only 0.115 ms. All Map calls averaged 0.246 ms, texture creation averaged 1.60 ms, copies averaged less than 0.001 ms, and the synchronization shadow-flush path was never entered. The approximately 96–101 ms queue drain therefore remained CPU-side construction work, not GPU-transfer latency. Suppressing these required writes would trade correctness for well under one percent of the remaining transition time. It is not implemented.

Deeper tracing localized part of that residual Synthesis cost to eager construction of recipe UI that is not necessarily visible. One repeatable example was the hidden string `True Dragon Hourglass`: one of its four render operations took about 19–21 ms, while the other three took roughly 0.12 ms each. The slow operation made 63 atlas locks; 60 were served by the frame cache, while the first read of each of the three mutable font atlases accounted for essentially the entire 19–21 ms. This is consistent with Synthesis constructing a complete recipe interface, including unavailable or off-screen entries, rather than limiting work to the currently visible list.

Retaining those three atlas snapshots across frames could avoid this remaining first-read cost after the initial construction, but the atlases are mutable and proving complete invalidation coverage would require a broader correctness investigation. The current frame boundary remains the deliberate safety limit. No additional optimization is applied for this residual cost at present.

## Frame-rate independence

### TL;DR

Some movement rules were written as fixed amounts per frame, so high refresh rates made characters jitter or eventually stop moving, and made Totori and Meruru's world-map cursor too fast. The mod scales those rules with actual frame time, preserving the original 60 fps feel at higher refresh rates without imposing a frame-rate cap.

### Safety

The correction is a scaling of one constant, clamped so it can never exceed the value the games ship with. At 60 fps and below, everything therefore behaves exactly as it always did. Nothing touches how frames are presented, so there is no frame-rate cap and no tearing introduced. Each half can be switched off on its own for comparison, and the second half refuses to run without the first, since it depends on the ground contact the first one holds. The world-map correction installs only on exact, verified builds of Totori and Meruru; Rorona was measured, needs nothing, and gets nothing.

### Details

The engine is variable-timestep by design: a frame's elapsed time is measured, clamped to a maximum, and threaded through the update tree, which is why the games do not simply run fast at a high refresh rate. What is not frame-rate independent is a set of constants that describe a *distance per frame* rather than a speed, and those are only correct at the 60 fps the games were built around.

The one that matters is in the field-map character's collision resolver. It computes the total distance the character moved this frame and, if that distance is below a fixed threshold, discards the entire frame's movement: the position is reverted to its value on entry, and the ground-snap sweep that would re-seat the character is skipped along with it. At 60 fps this is a reasonable way to ignore numerical noise. As the frame time shrinks, the same fixed distance covers more and more of what the character is actually doing.

Two symptoms follow, and they are the same bug at different scales. Standing still, the only movement is one frame of gravity, which at a high refresh rate never covers the threshold before the grounded grace period expires, so the character loses its footing, falls, lands, and repeats: a visible vertical buzz that begins around 115 fps and is why interaction prompts near the character can flicker. Far higher, the threshold grows to exceed ordinary walking, and the character cannot move at all.

The mod corrects the constant, not the frame rate. Scaling the threshold with frame time turns it back into a speed, so it means the same thing at any refresh rate, and a clamp keeps it from ever exceeding the shipped value, which leaves low frame rates behaving exactly as before. That alone restores movement and holds ground contact, but it does not remove the resting case: gravity is gated on the character's own state bytes rather than on being grounded, so it keeps integrating into the surface and a frame still breaks through periodically.

The second part suppresses that. While the character is genuinely at rest, meaning grounded, without horizontal velocity, and with the previous frame's movement reverted, the mod zeroes vertical velocity and pins the grounded grace timer before the update runs. Pinning the timer is what makes zeroing the velocity safe: the grace period can no longer expire, so nothing needs the velocity ramp to re-establish contact. It runs before the engine's update rather than after, because the update refreshes the entry-position copy that the rest test compares against.

Both parts are on by default and each can be turned off for comparison, as described in [ADVANCED.md](ADVANCED.md). The second refuses to run without the first, since the grounded state it holds on can otherwise drop while the character is still settling.

Totori and Meruru have a separate frame-rate coupling on the travel map. Their freely controlled analog cursor reads both stick axes, folds in the four digital directions, rotates and normalizes that direction, then adds a fixed step directly to its authoritative position once per rendered frame. The mover receives no elapsed-time argument, although its immediate driver does. The mod hooks both functions, scopes the driver's real frame delta to the mover call, snapshots the position at `self+0x30`, and rescales the engine's step by `min(dt × 60, 1)`. It writes the corrected position back and republishes it through the mover's own target at `[self+0x28]`, keeping the authoritative and rendered positions coherent.

This preserves the shipped movement byte-for-byte at 60 fps and below while holding distance per second steady above 60 fps. Runtime validation at 144 fps measured Totori's raw step at approximately 0.4465 units per frame, the scale at approximately 0.416, and the corrected speed at approximately 26.789 units per second; Totori and Meruru were then both validated against their original feel under a 60 fps MangoHud cap.

Rorona does not share this mover: its stick advances a discrete location selector. A read-only probe on the routine that commits each selected location measured a median interval of 382 ms at the normal 144 Hz and 416 ms under a 60 fps cap; the corresponding means were 394 ms and 384 ms. The cadence therefore stays stable in real time rather than differing by the 2.4× refresh-rate ratio. Rorona needs no corresponding correction, and this subsystem installs nothing there.

The four supported executable rows are exact-address and prologue gated: Totori English uses driver/mover/publisher RVAs `0x2faf60`/`0x2ff540`/`0x2e9710`, Totori multilingual uses `0x518d20`/`0x51d300`/`0x507470`, Meruru English uses `0x2556c0`/`0x259ba0`/`0x247bb0`, and Meruru multilingual uses `0x24a5e0`/`0x24eac0`/`0x23c310`. `ARLAND_WORLDMAP_FIX=0` disables the correction for comparison. `ARLAND_WORLDMAP_PROBE=1` logs the measured raw and corrected movement.

An earlier approach capped the frame rate instead. It was withdrawn: holding a rate that is not a divisor of the display's refresh meant presenting without waiting for vertical blank, which tears in exclusive fullscreen. Nothing in the current fix touches presentation, so the game presents exactly as it always did.

Other frame-rate couplings exist outside the field map, in effect playback and, in Atelier Meruru DX, in secondary motion such as hair and clothing. Those are not yet addressed.

## High-resolution rendering

### TL;DR

The games nominally support 1440p and 4K, but parts of their renderer remain fixed at 1080p, producing an enlarged target with only a smaller image inside it. The mod's own launcher offers the useful resolutions, and the mod scales the affected render targets, screen bounds, and dialogue blur so the games genuinely render at the selected resolution. This rendering layer also provides optional MSAA and sharper texture filtering.

### Safety

Blank, incomplete or out-of-range resolution values are ignored and leave the game's own selection untouched. Only render targets that are exactly 1920x1080 and created empty are enlarged, so a texture that happens to be that size for another reason is not caught by accident, and ordinary 1080p play never activates any of it. MSAA stays off unless it is asked for, and an unsupported request falls back through lower sample counts; it never fails outright. The sharper texture filtering leaves the sampler types the shadow rendering depends on alone. And the mod no longer patches Koei Tecmo's own settings program at all: resolution comes from the mod's launcher instead, so that program is left exactly as it shipped.

### Details

Resolution is chosen in the mod's own launcher, which writes `DisplayWidth` and `DisplayHeight` under `[Rendering]` in `arland-fix.ini`, and the 64-bit game DLL applies them. Blank display keys are created there by default, alongside blank `RenderWidth`/`RenderHeight`. When both display keys hold valid dimensions, the DLL replaces the swap-chain request, clears the inherited refresh-rate constraint, and resizes the matching first main depth target before applying the ordinary auxiliary-target and raster corrections. Missing, blank, incomplete, or out-of-range values leave whatever the game's own settings selected unchanged.

Being independent of Koei Tecmo's settings editor matters, because that editor filters its resolution lists through Windows display-mode reporting: DPI virtualization and the current desktop mode can hide a resolution the game and display can use, most visibly on Steam Deck and other high-DPI handhelds, and in docked use. Earlier releases worked around that by patching the editor's two mode arrays in memory through the 32-bit `msimg32.dll` proxy. That patch has been removed. The mod's launcher does not consult Windows' mode list at all, so the resolutions are simply there, and the stock editor is now left exactly as shipped.

A larger backbuffer alone does not get you there. The old render path creates the main depth target at the requested dimensions but later creates auxiliary render/depth targets and submits viewport/scissor state hard-coded to 1920×1080. It also records rendering through a deferred D3D11 context. Correcting only the immediate context therefore produces genuinely large targets with a 1920×1080 image confined to their upper-left corner.

The D3D11 layer learns the larger main-target size and resizes only later exact-1920×1080 render/depth targets created without initial data. Raster state is tracked independently for the immediate and deferred context paths. When an affected target is bound, exact full-screen 1920×1080 viewport and scissor state is replaced with that target's dimensions before drawing on the same context. This produces direct native 2560×1440 and 3840×2160 rendering; neither mode is a 1080p upscale, and 1440p is not implemented by rendering at 4K and downsampling. Ordinary 1920×1080 and lower-resolution operation remains unchanged.

Rorona's blurred-dialog path contains two additional fixed-resolution assumptions. It copies only a 1920×1080 source box into the dialogue snapshot and submits a four-vertex quad whose positions cover `0..1920 × 0..1080`. Merely resizing the render targets therefore leaves the right and bottom of the snapshot black and limits the blurred output to the same upper-left region. When both copy resources match the enlarged main target, the D3D11 layer expands that exact source box to the configured dimensions.

The 48-byte quad is shared with portraits and other cutscene layers, so changing its original contents globally causes those assets to flash briefly in incorrect positions. The fix preserves the game's buffer and attaches a scaled companion instead. The copied snapshot is tagged, that identity is propagated through the three-vertex blur passes, and the companion is bound only for the final four-vertex draw that composites a processed blur result. The original buffer is restored immediately afterward. Other copy regions, vertex buffers, and cutscene draws are left unchanged.

Totori and Meruru share the blur/capture engine classes and post-blur shaders but did not issue Rorona's exact fixed-size snapshot copy during validated 2560×1440 dialogue scenes. Their ordinary resized blur targets filled the output correctly, so the exact runtime predicates leave the Rorona-specific correction inert in those games.

The D3D11 layer also contains an optional multisample render-target and resolve path, adapted from TellowKrinkle's rendering work. On first launch, if the file is absent, it seeds `arland-fix.ini` with `MSAA=1`, blank `DisplayWidth`/`DisplayHeight`/`RenderWidth`/`RenderHeight`, `ShadowMultiplier=1` and `AnisotropicFiltering=8` under `[Rendering]`, plus `BattleShadows=true` under `[Battle]`. The path stays inactive unless the MSAA value is changed to `2`, `4`, or `8`, or the higher-priority `ARLAND_MSAA` environment variable requests one of those values; absent values and values below two use the original single-sample path, and unsupported requests fall back through lower sample counts.

The game keeps owning single-sample host resources, while matching multisample color and depth targets are attached as private data and resolved before reads, copies, render-target changes, shader-resource binding, and deferred command-list completion. Four-sample MSAA was visually validated at 2560×1440 in Rorona, Totori, and Meruru, including each game's blurred-dialog rendering. It remains opt-in because behavior can still vary across GPUs and drivers.

Optional anisotropic filtering is a separate, cheaper texture-quality knob. The games create their texture samplers with plain linear filtering, so obliquely-viewed surfaces (floors, walls, receding ground) blur. When `AnisotropicFiltering` (or `ARLAND_ANISO`) requests `2`, `4`, `8`, or `16`, the D3D11 layer hooks sampler creation and, for samplers using a basic point/linear filter, substitutes anisotropic filtering at the requested maximum-anisotropy level before the sampler is created. Comparison, minimum, and maximum filters (shadow PCF and similar) are recognized by their filter enum and left unchanged, and the hook is installed only when the feature is enabled. Because the upgrade happens once at sampler creation, there is no per-draw or per-frame cost. Defaults to 8x.

## Alternative launcher

### TL;DR

The mod includes a replacement launcher that puts the game's settings and every mod option in one window. Steam opens it automatically in place of the stock launcher while preserving the overlay, Steam Input, and access to the original tools; if the replacement is missing or cannot start, the stock launcher still works.

### Safety

If the mod's launcher is not installed beside the stock one, nothing is armed and the original launcher comes up exactly as before, so a partial install cannot leave the game unstartable. If our launcher fails to start, the original bytes are put back and the stock launcher runs as though nothing had happened. The substitution verifies the process and the exact instructions it replaces before changing anything, and only ever in memory. The process Steam started stays open behind ours instead of being killed off, which keeps the overlay, playtime tracking and Steam Input attached. The buttons that open Koei Tecmo's original tools switch the redirect off first, so they always open the real thing.

### Details

`arland-fix-launcher.exe` is a 64-bit settings window that puts every option in `arland-fix.ini`, plus the game's own resolution, window mode and language, in one place and starts the game from it. Both of Koei Tecmo's original front-ends stay reachable from it. It writes the mod's settings when the game is started, so there is no separate save step; close the window with unsaved changes and it asks first.

Steam runs `ArlandDXLauncher.exe`, not the game, so the launcher has to insert itself there. A 32-bit `msimg32.dll` proxy does it: when `arland-fix-launcher.exe` is present beside the stock launcher, the proxy points the executable's entry point at its own routine, which starts our launcher instead.

The proxy is loaded because both of Koei Tecmo's 32-bit front-ends import `AlphaBlend` and `TransparentBlt` from MSIMG32, and it forwards both to the system MSIMG32 library. MSIMG32 is used rather than WinMM because native DirectX initialization can dynamically depend on WinMM exports beyond the two functions the front-ends import directly. The three games' outer `ArlandDXLauncher.exe` files are structurally identical, sharing `.text` SHA-256 `58ba7aee62d924d35ca160829766bc8775125475894473bcbadf92d962fcc522`. The redirect is the proxy's only behaviour: `ArlandDXEnv.exe`, the stock settings editor, loads the same DLL and is forwarded to and nothing else. The stock launcher never puts a window on screen, so a plain drop-in install replaces it with no extra steps, and with no `arland-fix-launcher.exe` present the redirect is never armed and the original launcher comes up exactly as before.

Two properties of the redirect are load-bearing:

- It must not run in `DllMain`. The proxy is a static import of the launcher, so its process attach runs before the executable's entry point and before other injected code has finished setting itself up, Steam's overlay among it. The overlay hooks process creation in order to follow the game into child processes; starting our launcher from `DllMain` produced a child Steam knew nothing about, with no overlay, no frame-rate counter and no Steam Input, which is what makes a controller work when Steam is handling it. The redirect is therefore armed during process attach but executed at the entry point, by which time the process is fully assembled.
- The stock launcher process must stay alive while ours is open rather than being terminated once its replacement starts. It is the process Steam launched and is counting, and the game is started from our launcher underneath it. Waiting costs nothing, since that process has no window and no work of its own once its entry point belongs to us.

`[Launcher] SkipLauncher` in `arland-fix.ini` changes only where the redirect goes: the proxy resolves the game executable instead of `arland-fix-launcher.exe` and starts that, so neither front-end appears. Everything else about the substitution is unchanged, which is the point. The game is created by the same process at the same moment our launcher would have been, inherits the same environment, and this process still waits on it, so Steam sees the session it would otherwise have seen. Which executable runs is decided the way both Koei Tecmo's launcher and ours decide it, from `[Lang] Language` in `ArlandDX_Settings.ini`: the multilingual build for Japanese, Simplified Chinese and Traditional Chinese, the English build otherwise, falling back to whichever of the two is installed. If neither is found the setting is ignored and the stock launcher comes up. The proxy reads this key wide and never seeds it, since creating `arland-fix.ini` from the launcher process would suppress the DLL's own first-use seeding of every other default.

The redirect installs a five-byte patch over the verified entry-point prologue, keeping the original bytes so they can be put back: if our launcher fails to start, the original prologue is restored and the stock launcher runs as though nothing had happened. `ARLAND_NO_REDIRECT` stands the redirect down, which is how our own launcher opens the original launcher and settings editor from its own buttons without them bouncing straight back to it. Because the proxy is 32-bit and our launcher is 64-bit, the child is created across that boundary and inherits the environment either way, which is how the Steam variables reach the game.

The proxy's single patch is gated on the verified process image and the exact entry-point bytes it replaces, and leaves the image untouched if anything does not match.

## Battle shadow restoration

### TL;DR

Rorona's battle renderer can draw proper ground shadows, but the port never tells it that the characters and enemies should cast them. The mod registers the battlers with the game's existing shadow system and carefully restores the field-map state after combat. The shadows are the engine's own; the mod draws none of them.

### Safety

The mod does not draw any shadows of its own. It tells the engine's existing shadow system about characters the port never registered, through the same routine the field map already uses, so everything visible is rendered by the game. The objects it registers are identified by their exact class fingerprints first; anything that does not match is skipped. Because the engine raises no event when a battle ends, a per-frame watchdog restores the field map's original shadow state once the battle objects are gone; it only arms after it has actually seen a live battle, so a slow battle intro cannot trip it early, and it exists precisely so the mod stops touching freed battle objects.

### Details

Atelier Rorona DX renders ordinary battles without any character or enemy ground shadows. The engine's shadow pipeline is present and functional (the field map uses it), but the port's battle scene setup never registers the battle actors as shadow casters. Atelier Meruru DX, by contrast, registers its battle casters natively, which is why its ordinary battle shadows work out of the box.

The restoration hooks the engine's shadow-helper initialization and observes its two static call sites: the battle scene-setup path and the field-map re-entry path. When the battle path runs, the mod records the battle game-mode object and the battle's shadow helper, locates the party's character vector inside the game-mode (verified through the BtlChara-family vtables), and registers each character's render node as a shadow caster through the engine's own `ShadowCharacterBuild` routine, the same call the field map uses. The battle helper is then published to the engine's global active-helper slot so the shadow traversal walks the battle casters; the displaced field helper is remembered. Enemies are registered through the same container discovery. The engine still does the rendering, so these shadows go through the game's own pipeline end to end. What the mod adds is bookkeeping, not draw calls.

Alongside registration, the mod tracks the battle state machine by recognizing the `GmStateBtl`-family state vtables (Enter, SelectCommand, WaitAction, the Result states, and so on). This state tracking is what the cut-in features key on, and it has to disengage reliably.

Returning from battle to an already-loaded field map does not re-run the field's shadow-helper initialization, so there is no engine event to observe on the way out. A per-frame watchdog fills that gap. It checks whether the battle game-mode still looks alive, meaning its party vector still holds objects with BtlChara vtables. Once a game-mode that was previously seen alive stops looking alive for twenty consecutive frames, the battle is over, so the saved field helper is restored to the global slot and all battle tracking is cleared.

The watchdog only arms once the game-mode has been seen alive, so a slow battle intro, where the party is not yet spawned, cannot trip it early. Without the watchdog, the tracking kept scanning the freed battle objects every frame after combat and degraded field performance.

## Battle cut-in shadows

### TL;DR

During battle close-ups, the games darken the arena and stop the ground from receiving shadows. The mod can keep the scene bright and its real shadows visible, while following the game's hide-and-show choreography so invisible or repositioned characters do not leave stray shadows behind.

### Safety

Both halves are off unless you turn them on. What they change is one small, specific value the engine sends to the graphics card each frame: a bounded sixteen-byte update in a known position, leaving everything else in that data, including the transform matrices sharing the buffer, untouched. The hooks are checked against each build's exact instruction bytes, and the structure offsets they need are verified byte by byte from that build's own code, so a wrong offset leaves the feature uninstalled instead of writing to the wrong field. If those hooks cannot install, the mod falls back to a mode that waits for the scene to settle before holding anything, which preserves the game's original cover at exactly the moments where stray shadows could otherwise appear.

### Details

Atelier Rorona DX renders attack "cut-ins" (the brief close-up when a character or enemy acts) without ground shadows and with a visibly darker scene. That is how the game behaves on every platform, not a port regression, and both symptoms trace back to a single animated constant.

The battlefield ground is a shadow receiver, and its 880-byte material shader gates shadow reception on scene-light intensity. The vertex shader reads a `diffuse` value at byte 832 of the material constant buffer and computes a gate of `2.5 - 2 * min(diffuse.w, diffuse.x)`. The pixel shader samples the shadow map only when that gate is below one, that is, when the smaller diffuse component exceeds `0.75`. (The pixel shader's own reflection names byte 832 differently, `shadowLPos`, a collision that obscured the mechanism during investigation; the value that matters is the vertex shader's `diffuse`.)

A separate sixteen-byte scene-light parameter drives visible floor brightness from the same logical intensity. The cut-in animates that intensity down to roughly `0.7`. That simultaneously darkens the floor and, sitting `0.05` below the `0.75` reception threshold, trips the gate closed. With the gate closed the receiver never samples the shadow map, so every object (acting character, party, and enemies alike) loses its shadow at once while the floor dims. The shadow casters are unaffected and keep rendering into the shadow map; only reception is switched off.

Holding the gate open is not enough on its own. The engine runs its own caster cover-up during the cut-in, and the closing gate had been hiding the seams in it.

When a cut-in begins, the non-focus battlers are hidden and repositioned ("juggled"), but their per-node caster flags (`PNode::setCastShadow`) are cleared only about a quarter second later, deferred with the visual cross-fade, then restored instantly at cut-in exit, before positions finish restoring. Vanilla never shows stale shadows for these characters because the fading scene light crosses the 0.75 reception threshold during exactly those windows, so the closed gate covers the stale casters. Holding the gate open naively therefore exposed stray floor shadows for characters that were invisible or off-position.

The fix gets ahead of the engine instead of waiting for it. Hooks on the tactical-scene `hideAll`/`showAll` wrappers, the functions the cut-in machinery calls to fade the non-focus battlers out and back, clear the registered casters' flags the moment the hide starts, re-clear them when the show's instant restore fires, and restore them once the battle leaves the cinematic states. The restore waits on a condition, not on a clock. A wall-clock delay is a guess at when the juggle has settled, and guessing short hands shadows back to actors that are still hidden or mid-fade, which puts a shadow under an invisible character. A 0.3 s delay remains as a floor, after which the condition is re-tested every 100 ms until the cinematic ends and everyone is on screen again. Visible cut-in participants keep their shadows, because the event system's own immediate show paths re-set their flags.

A third window sits inside the cut-in itself. The event choreography hides individual non-focus battlers through a deferred per-actor alpha fade of about 0.25 s, and the engine only drops a faded-out model from the shadow map when that fade expires. (Its fade-end handler recursively hides the whole model subtree, shadow nodes included.) During the fade the model is alpha-invisible but still casts, so with the gate held open its full-strength shadow lingers on the ground. Rather than edit the scene graph directly, the mod front-runs that expiry: it hooks the leaf that arms a battler's fade and, when a hide latches during a cinematic state, zeroes the model's fade timer. The engine's next visibility tick then performs the complete native hide, the recursive subtree clear plus the state flags its cancel and re-show path depends on, so there are no manual node writes and the focus actor (which the hide enumerator never arms) is untouched. The only cost is that the hidden battler pops out instead of fading over a quarter second, off-camera and negligible.

That leaf is a thirty-byte function with no `.pdata` entry, which is why it stayed unfound on Totori for so long. Every search for it walked the function table, and a leaf without unwind data does not appear there. Found instead by searching for the instruction bytes of its opening compare, it exists in all six executables and is byte-identical apart from the two Model field offsets it encodes as `disp32` operands. Those offsets differ by engine generation and not by a single constant: Rorona and Meruru put the current-visibility byte at `+0x80`, the fade-pending byte at `+0x8f` and the duration float at `+0x90`, while Totori uses `+0x90`, `+0xa2` and `+0xa4` — the flag bytes shifted by `0x13` and the float by `0x14`, because Totori's structure carries alignment padding the earlier one does not. Each build's values are read from its own setter and constructor. The verification window is built from the offsets in the per-game table, not hardcoded, so a wrong offset fails the byte match and leaves the hook uninstalled instead of writing to the wrong field.

An earlier approach cleared the caster registry's visibility flags directly and was replaced by this one. The registry entries turned out to be model locator roots rather than the drawable shadow leaves, so clearing them never reached the shadow map at all. The leaves hold their own visibility flag, which only the engine's subtree hide clears.

With the stale casters cleared from the first frame, the brightness and reception hold can engage immediately and the cut-in never visibly dims. The `hideAll` prologue is byte-identical across all five battle-capable executables; `showAll` differs per engine generation and is verified per build. If these hooks fail to install on some build, the hold falls back to a transition-aware mode: it engages only once the observed dim value has been bit-identical for at least 60 ms (after the entry fade has bottomed out, by which time the engine has cleared the juggled casters), eases up over a further 120 ms, and never engages during the exit fade. That preserves the vanilla cover at the cost of a brief visible dim.

The restoration addresses the two halves separately during cinematic battle states, and both are configured through the `[Battle]` section of `arland-fix.ini`. Both ship opt-in on all three games: the capability matrix in `src/game.cpp` marks them `OptIn`, so neither restoration runs until it is asked for, and both keys are seeded lazily from those per-game defaults instead of being written eagerly on first launch.

`BattleCutInDimming` governs brightness. It defaults to `true`, which allows the vanilla cut-in darkening; setting it `false` holds the sixteen-byte scene-light parameter at `1.0` and keeps the floor lit. `BattleCutInShadows` governs reception. It defaults to `false`; setting it `true` forces the receiver material's `diffuse` back to `1.0` immediately before each shadow-receiving ground draw through a bounded sixteen-byte update over the `[832, 848)` field, which reopens the reception gate. The two are independent, so the vanilla darker cut-in can be kept while shadows are restored, or either half enabled on its own. The `ARLAND_CUTIN_SHADOWS` and `ARLAND_CUTIN_DIMMING` environment variables override the respective keys for a session. (Note that the dimming key is worded as the inverse of the action it controls: it asks whether the cut-in *may* dim, so `false` is what engages the hold.)

Four further environment switches exist for this subsystem, none with an `arland-fix.ini` counterpart. `ARLAND_CUTIN_ACTOR_CLEAR=0` disables the per-actor front-run in all three games, which is the A/B for the fade-start behaviour: with it set, a battler hidden mid-cut-in fades over its full quarter second and its shadow leaves only at the end. `ARLAND_CUTIN_CB_TRACE=1` dumps the cut-in constant-buffer writes, and `ARLAND_CUTIN_FLAG_TRACE=1` logs the live callers of Rorona's two shadow-node flag routines, which static analysis could only find reached through function-pointer dispatch (English build only, the only one whose RVAs are mapped for it). `ARLAND_CUTIN_SNODE_FLAG=1` is not a diagnostic: it re-enables the earlier experimental path that restores the battle shadow-node flags from the shadow-map clear and the scene pass. It is off because that approach was superseded by the tactical hide/show front-run described above.

Two log lines report this subsystem, and they do not mean the same thing. `Deferred-hide arm hook installed=1` says only that the detour attached; a subsystem can install and then do nothing, which is how an inert caster clear once survived a full day of debugging while its install line read as success. `Deferred-hide arm force-expired a caster fade (first hit)` is emitted the first time the front-run actually acts, and is the line that confirms the feature works.

Both halves stay opt-in pending wider playtest, but they were only offerable at all once the stray-shadow glitch described above was fixed. Before that, the restored reception showed a ground shadow for a character the engine had hidden or repositioned during the close-up; the settle-gated hold and the force-expiry per-actor hide resolved it, validated in all three games.

The patch is a small one. It touches only the diffuse field, never the transform matrices that share the buffer, and the pixel shader does not read that field, so the shared vertex and pixel constant buffer is patched safely; the draw-time path also works on the game's deferred rendering context. The engine's own casters then project real shadows onto the cut-in floor with no injected geometry, and the feature composes with the always-on battle-shadow restoration that supplies those casters. Basic and assist cut-ins keep the real arena floor on screen and gain shadows; solo specials that replace the entire background with a dedicated close-up scene have no real floor and are left unchanged.

Atelier Meruru DX shares the same engine and the same cut-in reception gate, and the restoration applies there too. It differs only in the source of the casters: Meruru registers its battle shadow casters natively (per character, through a build path Rorona lacks), so its ordinary battle shadows already work and no caster restoration is needed, only the cut-in reception gate, which the same value-matched patches reopen. The single Meruru-specific addition is battle-state detection: its `GmStateBtl*` cinematic states were located by RTTI so the patches fire during Meruru cut-ins exactly as they do for Rorona.

Atelier Totori DX (English build) received the same battle-state treatment after a static-plus-runtime investigation established that its fighting shadows are natively healthy: like Meruru, its battle characters register as casters through the game's own build path (confirmed by a runtime probe: configuration byte set, helper context live before the character constructors, caster registry filling). Totori is the structural outlier: its battle shadow helper is embedded at a different game-mode offset, it has no global active-helper slot at all (field and battle each render through their own helper, so the helper-publish machinery is inert there by design), its state machine lacks `SelectDefence`, and its result chain uses different state names (`Result`, `AddPay`, `DropItem`, `LvUp`), which the cinematic-state list carries. These per-game differences are encoded in the battle address pack (`BattleBuildAddrs`), including the helper embed offset and zeroed entries for the structures Totori does not have. The multilingual Totori build is not yet covered.

Totori's cut-in mechanics also differ at the shader level. Its shader set was rewritten for D3D11 (`commonShaderWin.PSSG`, GatherCmp PCF, per-shader `$Params`), while Rorona and Meruru ship the PS3-style pack. That has two consequences.

First, Totori's battle ground receiver has no shadow-reception gate at all. Its PCF sampling is unconditional and `diffuse` only tints the final color, so the gate-hold patch has nothing to reopen there, and no 880-byte constant buffer exists anywhere in Totori's shaders. Missing cut-in shadows in Totori are a caster-side matter, still under investigation.

Second, the cut-in dim is the same `BtlField` fade to (0.7, 0.7, 0.7, 1.0) as Rorona's, but it reaches the GPU through Totori's own constant-buffer layouts rather than the 16-byte `$Params`. A runtime constant-buffer trace during a cut-in also established that Totori's battle arenas render with the fieldmap shader family, not the dedicated battle-ground shader the static analysis predicted. The dim flows through the fieldmap layouts, with `diffuse` at (ByteWidth 304, offset 16), (48, 32), (80, 0), (144, 0), and (160, 16), alongside the battle and character layouts (32, 0) and (16, 0), and the toon vertex-shader families at (224, 208), (13024, 13008), (12960, 12944), (160, 144), and (96, 80).

The dim-hold consults this per-game field table with one value predicate everywhere: only a uniform (s, s, s, ~1) with s in (0.5, 0.98) is rewritten. The (304, 272) location the trace also matched is excluded: it is a light-matrix row. The fieldmap vertex shaders gate shadow reception on the same `diffuse` (closing below 0.85), so on Totori the dim-hold doubles as the reception hold. It carries the same settle gating as the Rorona and Meruru gate-hold, so holding these fields also restores cut-in floor shadows, and the settle delay preserves the vanilla stale-caster cover at the transitions.

## High-resolution shadow maps

### TL;DR

The games draw every shadow into a relatively small 1024×1024 texture, which makes shadow edges look blocky. The mod creates larger companion textures and quietly redirects the existing shadow pipeline through them. Definition improves, while the engine keeps its own textures and its own shadow renderer.

### Safety

The engine's own shadow textures are never resized or replaced, so every size the engine derives from them stays true. The mod creates a larger companion texture alongside each one and ties it to the original's lifetime, so it is released when the game releases its own. Anything ambiguous at creation time, such as initial data, staging use, mip levels or multisampling, declines the companion and keeps that shadow map entirely on the original path. Each of the four redirection points does nothing at all when no companion exists, and at the default setting none of this machinery runs.

### Details

The games render all shadows into two 1024×1024 `R24G8_TYPELESS` depth maps, a caster map (A) and a receiver map (B) with a per-frame A→B transfer between them, so shadow edges are visibly blocky, most noticeably in Meruru. The `ShadowMultiplier` option renders shadows at 2048, 4096, or 8192 instead.

The engine's own maps are never resized. It takes viewport sizes, copy extents and memory assumptions straight from its texture metadata, so resizing one in place would quietly invalidate all three. Instead, each eligible 1024×1024 shadow-map creation also creates a separate mod-owned enlarged "twin" texture, attached to the engine texture as private data so the two share a lifetime, and when the engine releases its map, the twin is released with it. Anything ambiguous at creation (initial data, staging or CPU access, mips, arrays, MSAA, misc flags) declines the twin and keeps that map on the vanilla path.

The shadow pipeline is then redirected onto the twins at four points, each inert when no twin exists:

- the engine's shadow-map clear is mirrored onto the twin;
- depth-only caster binds of a shadow-map DSV are redirected to a lazily created twin DSV (binds that pair the shadow map with a color target fail safe to the vanilla pass);
- the engine's A→B shadow-map transfer is mirrored as an equal-sized copy between the twins;
- the receiver's shadow-map SRV bind is substituted with the twin's SRV, with a pointer-keyed negative cache so the hot bind path stays cheap.

Two size assumptions still need correcting, because the engine sizes everything from its own 1024 metadata. Exact 1024×1024 viewport and scissor state is rewritten to the twin's dimensions during the redirected caster pass. And the receiver material's PCF tap size, which encodes the shadow texel size in the same 880-byte constant buffer the cut-in fix patches, is rescaled so filtering matches the enlarged map. At `ShadowMultiplier=1` none of this machinery activates and the shadow pipeline is untouched.

## Meruru conversation text-render cache

### TL;DR

Meruru was rebuilding unchanged conversation text every frame while an animated portrait was on screen, causing a large slowdown for the whole conversation. The mod keeps the finished text bitmap for exactly as long as the conversation balloon exists, so those repeated renders become inexpensive memory copies.

### Safety

The cache is alive for exactly as long as a conversation balloon is on screen, and the last balloon closing clears it, so nothing can go stale between conversations. Its size is bounded: when a long typewriter reveal fills it, it clears and rebuilds itself, so it cannot grow without limit. If a cached result would not fit the buffer the game supplied, the mod renders normally instead of reallocating memory the game owns. The hooks verify each build's exact function bytes before installing, including the balloon class identity, so they cannot attach to a similar-looking routine.

### Details

Atelier Meruru DX's field-map conversations with animated bust-up portraits collapsed the framerate on the English executable for the duration of the conversation. The cost was not the portraits: the conversation balloon's per-frame callback pump re-entered the executable's text-render path (the same CPU-side glyph and atlas work that makes menu construction slow) every frame, for text that had not changed. Menus pay that cost once per rebuild; the balloon paid it continuously.

The mod already contained a text-bitmap replay cache built for menu diagnosis. `cachedRenderText` keys on the renderer, font, atlas, style, and the exact string, and replays the previously rendered output bitmap into the caller's buffer instead of re-rendering. Its lifetime, though, was scoped to a single queue drain.

The fix gives that cache a cross-frame scope bounded by the conversation. Hooks on the `BalloonBucMode` constructor and destructor count live conversation balloons, and while any balloon is alive the cache activates and its per-drain clears are suspended, so a string that is identical from frame to frame costs one memory copy. The destructor of the last balloon clears the cache.

Two edges needed handling. The typewriter reveal inserts one entry per partial string, so the cache is bounded and an overflow clears and rebuilds it. And a replay whose target buffer is too small falls back to a real render; the mod will not reallocate through the game's allocator, a case that cannot occur for unchanged text anyway. The hooks verify the constructor and destructor prologues per build (the destructor check includes the RIP-relative load of the `BalloonBucMode` vtable, pinning it to the right class) and are installed for both the English and multilingual executables. `ARLAND_BUC_TEXT_CACHE=0` leaves them uninstalled, which is the A/B for the whole conversation scope. The underlying replay cache has its own switch, `ARLAND_TEXT_BITMAP_CACHE`, but a live balloon activates it regardless of that value, so turning the scope off is the switch that restores vanilla behaviour.

## High-resolution UI text

### TL;DR

The games use a small pre-rendered bitmap font, so their text looks soft and pixelated at modern resolutions. The mod replaces English text with crisp scalable fonts, or sharpens the original glyphs when needed, while preserving the game's layout, sizing, and controller icons.

### Safety

The replacement bitmap comes from the game's own text allocator, so the game frees it normally. The temporarily doubled dimensions are put back once the picture has reached the graphics card, so the text keeps its original size on screen. If the replacement font cannot draw a character, most often one of the controller-button icons, the whole string falls back to a sharpened version of the game's own glyphs, so nothing is ever dropped or shown as an empty box. The substitution is wired only to the English builds; on the Japanese and Chinese builds the hooks stay unresolved and every mode is a safe no-op.

### Details

All UI text in these games comes from a pre-baked bitmap font. Koei Tecmo's G1N atlases store every glyph as a fixed 32×48 image that the engine blits 1:1, with no scalable rasterizer, so text is soft and pixelated at 1440p or 4K. This feature re-renders that text at full resolution while preserving the engine's exact layout. `[Rendering] Font` chooses the mode: `replaced` (the default), `upscaled`, or `original` (or `off`; the untouched bitmap).

Every mode works the same way at the top level: let the engine render a string normally, then swap the result. When the engine finishes a string it leaves an "output object" at `renderer+0x1a0`. That struct holds the power-of-two bitmap width and height, a pointer to its 8-bit alpha pixels, four normalized metrics (used-width, used-height, and line-height, each a fraction of the pow2 size), and the line count. The mod reads it, builds a higher-resolution bitmap of its own, and writes the new pixel pointer and doubled (`kScale = 2`) dimensions back into it.

One detail decides whether the swap works at all. A text quad's on-screen size is computed as `fraction × pow2-dimension`, so doubling the bitmap's dimensions would double the text on screen. Two things prevent that. First, the replacement bitmap is allocated through the engine's own text-buffer allocator, so the engine still frees it correctly. Second, the doubled dimensions are restored to their originals once the string's consumer has built its GPU texture, through a hook on that consumer. The glyphs end up twice as dense while the quad keeps its original size. A per-string result cache, keyed on the string and font state, reduces a repeat render to a memory copy and keeps the work off the hot menu path.

`upscaled` keeps the engine's own glyphs and only raises their resolution. The baked bitmap is filtered up 2×, by default an SDF-style alpha-coverage steepen (bilinear and Lanczos are also available), followed by an unsharp pass. Nothing about the layout changes, so multi-line text, alignment, and the game's custom icon glyphs all survive untouched.

`replaced` re-renders each string from a per-game bundled scalable font (National Park SemiBold for Rorona, Nunito for Totori, Cosmetica Medium for Meruru; a compile-time matrix pairs each with a size multiplier and baseline nudge tuned to its metrics), rasterized with stb_truetype through a glyph-atlas cache so each glyph is drawn once and then reused. Matching the engine's layout meant reverse-engineering the text renderer. Three points carry most of the weight:

- Line breaks come only from an explicit `\n`; there is no word wrap.
- Line pitch is read from the engine's own line-height metric (`output+0x18`), not from an even division of the used height. The engine lays text out as `usedH = topOffset + numLines × lineHeight`, so dividing evenly overstates the pitch and multi-line text slowly drifts off the paper's ruled panels. Using the real line-height keeps every row aligned, and the block is centered in the used height so the single-line case matches the simple even split.
- Baselines are placed against a fixed cap-height reference, measured once from `H`, rather than each string's own ink. A line with descenders no longer rides higher than an all-caps one.

The game's English text also carries characters that look like ASCII but are not, and a Latin font has no glyph for them: full-width digits and letters (`１２３`, U+FF01–FF5E), CJK label brackets (`【】`), and Unicode Roman numerals (`Ⅰ–Ⅻ`, used for dungeon tiers). The decoder folds each to its ASCII equivalent before layout. If the font still cannot draw a character, most often one of the controller-button icons, the whole string falls back to the `upscaled` path, so no glyph is ever dropped or shown as a tofu box.

The replacement typefaces are compiled into the DLL from the vendored `.ttf` files under `vendor/font/` (the byte arrays are generated at build time by `scripts/embed_font.py`, not checked in), so the feature needs no loose font file; a `arland-hires-font.ttf` beside the DLL overrides them. The substitution is wired only for the English executables, because the per-game text allocator and consumer hooks resolve English-build addresses. On the multilingual builds those hooks stay unresolved and every mode is a safe no-op, so Japanese and Chinese text is never touched (and neither Latin font carries CJK glyphs regardless).

A smaller mechanism rides the same render path, correcting known English display typos. A per-game table rewrites the string pointer handed to the renderer before it is drawn or cached, for example Totori's "Synth Cateogry" to "Synth Category". Only the displayed text changes, the game's own string data is untouched, and because the rewrite happens on the render path the correction applies in every font mode.

## SMAA anti-aliasing

### TL;DR

The games ship without anti-aliasing, leaving visible jagged edges throughout the scene. The mod runs a lightweight smoothing pass after the 3D scene is finished but before the interface is drawn, so edges improve and menus and text stay crisp.

### Safety

If the runtime shader compiler is unavailable or any resource fails to be created, SMAA switches itself off for the session and the rest of the mod is unaffected. The pass runs before the interface is composited, so menus and text are not softened by it. On Totori, where it is injected between draws rather than at a target change, every piece of graphics state the three passes touch is saved and restored, leaving the pending draw exactly as the game prepared it. When the normal path is selected, no second pass is layered silently on top of it. That would paper over a fault instead of exposing it.

### Details

The games' only built-in anti-aliasing is none; the mod adds optional MSAA, but MSAA only multisamples geometry silhouettes and cannot touch aliasing that lives inside a surface, such as texture and alpha-test edges. The mod therefore also offers SMAA (Enhanced Subpixel Morphological Anti-Aliasing, Jimenez et al.), a post-process that works on the finished image and smooths any visible edge regardless of how it was produced. It is enabled by default and is a cheaper, broader alternative to MSAA: a single constant-cost pass rather than per-pixel supersampled shading.

SMAA runs the standard three passes (luma/colour edge detection, blending-weight calculation against the precomputed `AreaTex`/`SearchTex` lookup textures, and neighborhood blending) using the reference shader (vendored under `vendor/smaa/`, MIT) compiled at runtime through `d3dcompiler`, at the `SMAA_PRESET_ULTRA` quality level. If the runtime shader compiler is unavailable or any resource fails to create, SMAA disables itself for the session and the rest of the mod is unaffected.

The pass is injected before the UI is composited, matching the approach the Atelier Graphics Tweak used, so the HUD, menus, and text stay crisp. The two games' frame architectures differ enough that they need different injection points, and identifying the scene target is the part that took the most work to get right.

Rorona and Meruru render the 3D scene into an offscreen colour target, composite the HUD straight onto that same target, and only blit the finished result into the swap-chain backbuffer at the very end of the frame. The backbuffer therefore never holds a UI-free image, and every render-target or depth-state boundary reachable on it is already post-UI. Dimensions do not identify the scene target either. `CreateTexture2D` promotes the engine's hard-coded 1920×1080 auxiliary targets (blur, snapshot, conversation) to the main render size, so several textures answer any size test identically — and adding that promotion is exactly what silently broke this detection once. What does identify it is *what gets drawn into it*: only the 3D pass accumulates hundreds of depth-tested draws into a main-size single-sample target. SMAA runs on the first draw into that target with depth testing disabled, which is the first HUD element, so the passes land on the finished scene and the UI composites on top of the antialiased result. Binding away from the target is kept as a backstop for the case where the UI never touches it.

The whole frame is recorded on a deferred context and executed on the immediate one, so the injection happens during recording and the passes go into the command list in order. They must not preserve pipeline state on this path: the game's own setup follows the injection point, and the original implementation did not save state either. The passes re-enter the render-target and draw hooks that trigger them, so a thread-local guard prevents unbounded recursion, and the draw counter that gates injection is cleared before the passes run, not after.

Totori never rebinds its main colour target when the UI starts. A draw-state trace established that it keeps the same colour and depth targets attached, then disables depth testing before the final UI draw group. The render-target and depth-state setter hooks retain those two facts as atomic flags, so ordinary draw calls perform no D3D11 state queries; the first draw for which both flags identify the UI transition runs SMAA immediately beforehand. Because this is a draw boundary rather than a target-bind boundary, Totori's path snapshots and restores every pipeline binding the three fullscreen passes modify (render targets, shaders and resources, input assembly, raster state, viewports, blend, and depth state), leaving the pending UI draw exactly as the game prepared it. State preservation is an explicit parameter, not a shared assumption, because the Arland pair must not use it.

SMAA is independent of MSAA and supersampling, because the two solve different problems: MSAA only antialiases polygon silhouettes, while SMAA works on the finished image and so also smooths texture-interior, alpha-test, and shader/specular edges. Enabling one never disables the other. What MSAA changes is only *where* the finished pixels live at the injection point. With MSAA off, the target is single-sample and SMAA runs on it in place. With MSAA on, the multisample twin is bound (the twin is substituted on any depth-bound bind, which the scene pass always is), so the injection resolves the twin into its single-sample host, runs SMAA there, then writes the antialiased result back into the still-bound twin with a fullscreen copy — every sample in a pixel receives the same colour, which loses nothing because the image is already resolved. The host is re-marked dirty so the game's own later resolve carries scene and UI through together. That write-back costs one extra fullscreen pass and only runs when a twin is bound.

No Present-time fallback runs when pre-UI injection is selected, and that omission is the point. A silent full-frame pass layered on top of a working pre-UI pass softens the UI twice over while hiding that something is wrong; pre-UI injection either works or is a bug to fix. `ARLAND_SMAA_BOUNDARY=target|depth|both|scene` overrides the per-title default for testing.

SMAA runs once per frame in every path, latched and reset at Present. `ARLAND_SMAA_PREUI=0` forces the Present-time full-frame fallback (which also softens the UI slightly); `ARLAND_SMAA_DEBUG=1`/`2` visualize the edge and blend-weight intermediates.

SMAA cannot reconstruct detail that was already lost below the finished image's pixel grid. TellowKrinkle's per-sample sample-rate-shading approach is documented as non-functional on the older Arland DX titles; full supersampling is the option that captures such sub-pixel detail, and ships as the feature described next.

## Supersampling

### TL;DR

Supersampling renders the entire game at a higher resolution and then shrinks it cleanly to the display, recovering very fine detail that ordinary anti-aliasing cannot. The mod redirects the game's rendering into a larger internal image, downscales it once at the end, and pads with black bars when the shapes do not match.

### Safety

Only the game's own backbuffer views are redirected, and a view whose format the substitute cannot present is declined; that frame simply renders normally. Where the aspect ratios differ, the frame gets black bars; the picture is never stretched to fit. Those bars are cleared every frame, so nothing stale can show through. The render size is capped, with the aspect preserved, at a point well past where these 2010-era assets have any detail left to give. If a build ever failed to bind the backbuffer the way this depends on, the mod leaves the image alone and says so in the log, rather than quietly doing nothing.

### Details

`[Rendering] RenderWidth`/`RenderHeight` larger than the display resolution renders the whole frame, scene and UI alike, at that larger size and downscales it once into the backbuffer just before Present. Unlike MSAA it resamples shading, not geometry coverage, so it can preserve detail smaller than a display pixel.

These games bind the swap-chain backbuffer itself as their colour render target, so the split between render size and present size is made by redirecting the backbuffer, not by intercepting draws. Every render-target view the game creates over the backbuffer is created over a mod-owned render-resolution texture instead. Because the substitution happens at view creation, everything downstream follows without further interception: binds, clears, the MSAA twin, and the pre-UI SMAA injection all operate on the larger target, and the real backbuffer is touched only by the downscale. A view description naming a format the substitute cannot present is declined, and that frame renders into the backbuffer unchanged.

The downscale is a box filter. Each output pixel averages the source texels its own footprint covers, sampled on a grid spread across that footprint. At an integer ratio the samples land on texel centres and the result is an exact box, which is the case supersampling is really after; other ratios sample the footprint evenly and land close to it. An earlier scheme placed bilinear taps on texel corners to average 2×2 for free, which is a real optimisation at even ratios but degenerates at odd ones: at 3× the output pixel centre falls on a texel centre rather than a corner, so every tap collapsed to a single texel and the filter read the four corners of the 3×3 block while skipping its middle, undersampled and soft at once. The pass runs once per frame at display resolution, so correctness at every ratio is worth more than the saved samples.

When the render and display aspect ratios differ, the frame is fitted inside the backbuffer by the smaller of the two scale factors and the remainder is painted as black bars, which are cleared every frame because nothing else in the pipeline writes those pixels.

The render resolution is clamped to 7680×4320 with the aspect preserved. These are 2010-era assets: past roughly 8K there is no sub-pixel detail left for more samples to resolve, while cost keeps scaling with pixels drawn and the engine derives a large family of render targets from this size. The limit is diminishing returns first and memory second, not a constraint of the code.

Because the whole mechanism rests on the game binding the backbuffer as a render target, a build that never does so would be silently un-supersampled. That case is detected and reported: if no redirect has happened by the time the downscale would run, the backbuffer is left alone and a log line says so. A second line reports the largest viewport the engine actually drew with, so "supersampling is on but the edges are still jagged" can be answered from the log rather than guessed at.

## Borderless windowed mode

### TL;DR

Exclusive fullscreen makes these games slow to alt-tab and can interact poorly with modern desktops, while ordinary windowed mode leaves a visible frame. The mod creates a true borderless window that fills the current monitor without taking control of the display mode. It looks like fullscreen and alt-tabs instantly.

### Safety

Off by default, in which case the game's own fullscreen setting is used exactly as it stands. The mod asks for a plain window before the swap chain is created, so there is no exclusive-fullscreen mode to wrestle with afterwards, and it never takes ownership of the display mode. Because both the engine and the window manager restyle the window on their own, the style is re-checked about once a second, and only re-applied when it actually differs; repeated attempts are capped, so a window that refuses the style cannot spin. When the reposition would have to reach across threads it is posted asynchronously, so it can never block on another thread's message loop.

### Details

The games offer only windowed, with a title bar and border, or exclusive fullscreen. Exclusive fullscreen takes ownership of the display mode, which makes alt-tabbing slow and, under Wine or Proton, interacts badly with compositors and multi-monitor setups. `[Rendering] Borderless` runs the game as a plain window with its decorations removed, sized to cover the monitor it is on: it looks like fullscreen, alt-tabs instantly, and leaves the display mode alone. Off by default, in which case the game's own fullscreen setting is used as-is.

There are two halves to it. Before the swap chain is created its description is forced to windowed, because a swap chain created for exclusive fullscreen owns the display mode and restyling its window afterwards would fight it. After the swap chain exists, the window's caption and frame styles are replaced with a plain popup style and the window is sized to the full bounds of its current monitor, taken from the monitor the window is actually on so that a second display works and the window does not jump to the primary one. The monitor rectangle rather than the work area is used, so the taskbar is covered.

Applying the style once at startup does not hold: the engine restyles its own window in places, as does Wine's window-manager integration. So the check runs from `Present`, throttled to every sixtieth call — about once a second at 60 fps. The check itself is cheap, but the restyle it can trigger is not something to risk at frame rate, and nothing that reverts the window needs correcting inside 16 ms. Re-application is guarded by a comparison against the intended style and bounds, and repeated restyle attempts are capped so a window that refuses to take the style cannot spin. Because Present can run on a thread other than the one owning the window, the reposition is posted asynchronously in exactly that case, so it cannot block on another thread's message loop.

## Totori item and save corruption guard

### TL;DR

Totori accepts a saved container limit larger than its fixed 999-item array. Ordinary item operations then walk and write beyond that array, corrupting later game state including equipment; the resulting bad effect indices can crash battle entry or bomb use. The mod repairs affected saves while they load, writes the repaired data back on the next save, and bounds the remaining consumers so a bad value cannot crash the game in the meantime. Saves that are already valid are left byte-for-byte alone.

### Safety

Data is validated before anything is repaired, so a save whose records all check out is left exactly as it is and an unaffected file is never rewritten. The repairs are conservative: a damaged record keeps whatever part of it is trustworthy and only the unsafe remainder is cleared, and rebuilt skill entries come from tables inside the game's own executable rather than from values the mod invented. The limits are clamped back to the ones the game itself fixes at startup, not to numbers of the mod's choosing. Every hook is verified against exact function fingerprints first, so Rorona and Meruru, whose item system is different, install none of it. Each part can be switched off for comparison. What this cannot do is bring back items that vanilla already overwrote before the mod was installed; it repairs the structure so the damage stops there.

### Details

The work behind this section started from a save file shared by a member of the community whose game had become unplayable. That file is what made the mechanism visible and what the repair was built and validated against; how its container limit came to be wrong in the first place is not established, and does not matter to the fix, because the damage that follows is done entirely by the game's own code once the bad value is present.

Totori's chunk 7 contains 100 carried-item records, 999 container records and four trailer dwords. The 999 limit is fixed in both executables: CRT initialization constructs exactly `0x3e7` records of stride `0x34`; the list initializer writes `0x3e7` to both its logical scan limit at `owner+4` and physical capacity at `owner+8`; and the save reader and writer copy exactly `0xcaec = 999 * 0x34` container bytes. The damaged save instead stored a logical container limit of 5000, and the loader restored 5000 to `owner+4` without ever comparing it against the still-valid physical capacity.

Generic item count and insert routines use `owner+4`. With 5000 loaded they continue through globals beyond the physical array, and insert copies a complete `0x34` item into the first memory that resembles a vacant record. This is directly visible in the damaged save: coherent item records with ids 2840..2852 overlap equipment at offsets `+0x28` and `+0x24`. Those apparently irregular equipment offsets lie exactly on the container's continued `0x34` grid — container records 1171 and 1209 — not on a variable-length serialization layout. The equipment writer then persists the already-corrupted globals. One bad dword therefore does not stay one bad dword: vanilla turns it into continuing runtime damage that is written back to disk on every save.

The repair runs where that data enters and leaves the game. The load and save hooks validate the live list objects against their expected kind, physical capacity, backing pointer and mapped range, clamp the logical limits back to 100 and 999, and structurally scan all 1,099 fixed inventory records. A damaged record with trustworthy ids, finite quality and valid traits keeps that prefix while its unsafe effect suffix is cleared; a record whose prefix cannot be trusted is reset to the engine's canonical empty value. The same validation covers all 30 equipped records. Damaged saved character-skill entries are rebuilt from the executable's own per-character source tables, bounded set metadata is restored, and the engine's own equipment-stat recalculation runs after an equipment repair. Nothing is rewritten speculatively: a record that passes validation is not touched, so an unaffected save loads and saves exactly as it would without the mod. `ARLAND_ITEM_SANITIZE=0` disables this persistent recovery for comparison.

Two read-side guards remain necessary, because an item can also reach these routines from outside the arrays the save hooks audit. Battle entry scans item effects and traits at `0x25b3d0`/`0x25b450` (multilingual `0x477d00`/`0x477d80`) and originally checked only that each table index was nonnegative. The mod skips indices beyond the measured 223-effect and 204-trait tables. Bomb and Craft use reach a separate modifier builder at `0x25a010` (`0x476940`) with the same unbounded effect lookup and quality arithmetic. It normally runs untouched; if quality or an effect index is invalid, the detour calls it synchronously with a sanitized local copy of the item. The builder has a second unchecked lookup: its 161-record action-item table contains ids 0..159 and 163, but vanilla uses the lookup's `-1` failure as an index one record before that table. The detour rejects an id outside that exact set and leaves the caller's modifier vector empty. All seven direct callers in each Totori build initialize that vector before the call and either test its count or iterate from begin to end, so empty is an already-supported failure result. The predicate stays inside the action builder and is not applied to saved items generally, since valid weapons, armour and materials carry ids outside the action-item table.

Every target and source table is prologue- or geometry-verified before a hook installs. Runtime startup validation confirms that the English and multilingual Totori builds both install the effect/trait/modifier guards and all three save hooks with the same measured 223/204 table sizes. Rorona and Meruru have no byte-match for these Totori routines; both report `item_scan_guard=not_applicable` and return before resolving or installing any save hook. Runtime recovery validation loaded the damaged save, visibly removed its placeholder skill rows, clamped 5000 back to 999 and saved it again; the resulting file passed a complete structural scan of all 1,129 item records. A known-good save passed the same scan before and after a normal save. The first rejected read is always logged; `ARLAND_ITEM_PROBE=1` logs all of them, `ARLAND_ITEM_GUARD=0` restores the vanilla read routines, and `ARLAND_ITEM_SANITIZE=0` disables load/save repair.

## Diagnostics

Each session log begins with the version compiled from the repository's `VERSION` file, the recognized title and executable build, the configuration file and relevant environment overrides, the effective render settings, and concise `FIXES` records for the subsystems that installed. Installation failures, signature/prologue declines, device loss and other warnings remain visible in the normal log. A failure reached from a hot D3D path is written on its first occurrence, with sampled repeats in verbose mode, so a persistent resource failure cannot flood the file and erase the useful startup context. Successful low-level hook creation, raw addresses and object fields, repeated render-resource operations and battle lifecycle telemetry are verbose-only so the default log records release-relevant state without becoming a trace.

If a game crashes, a last-chance exception filter appends a post-mortem to `arland-fix.log` before the process dies: the exception code, the faulting address expressed as module+offset, the register state, and a conservative stack scan in which every stack slot pointing into executable module code is resolved to module+offset. The faulting module is also bucketed into a coarse category (`AUDIO`, `MOD(this)`, `GAME`, `GRAPHICS`, `SYSTEM`, `OTHER`, or `UNKNOWN` when the name cannot be read), and an audio module anywhere in the stack is flagged, so a fault the D3D11 layer cannot address is identified as such. `AUDIO`, `MOD(this)` and `GAME` are tested before the broader buckets, so they win ties. `ARLAND_CRASH_LOG=0` stands the whole post-mortem down. The handler chains to any previously installed filter and lets the exception continue, so debugger and Wine crash handling are unaffected. The previous session's log is preserved as `arland-fix.log.old` on each launch so a crash report survives the next start.

With `[Diagnostics] VerboseLogging` enabled (off by default; `ARLAND_VERBOSE_LOG` overrides), successful low-level hook installation, render-resource redirections, battle scene/state transitions and process memory (working set, peak and commit) are logged in addition to the normal records. The `MEM` line appears roughly every ten seconds as a passive probe for a crash that hangs rather than throwing, so a memory climb can be captured even when the exception post-mortem never runs. Menu statistics come with it: per-transition cache statistics, per-conversation cache hit/miss totals, and a periodic per-frame heartbeat (text-render calls and time, cache hits and misses, and the battle/cinematic tracking flags) used to localize frame-time regressions. `ARLAND_MENU_STATS` overrides that either way; unset, it follows `VerboseLogging`, which it can safely do because it only observes. The traces that do move the code path they report on, the menu-transition trace and the cut-in probe among them, never follow the checkbox and have to be asked for by name.

`ARLAND_ITEM_PROBE=1` logs every index the Totori item guards reject, with the whole item record and the caller, not just the first; `ARLAND_ITEM_GUARD=0` stands the read guards down. `ARLAND_ITEM_SANITIZE=0` disables load/save repair. See "Totori item and save corruption guard".

## Runtime memory manipulation

### TL;DR

The mod does not edit or replace the games' executable files. Its proxy DLLs are loaded alongside the game or launcher, make narrowly verified changes inside that running process, and forward everything else to the original Windows libraries. Those temporary changes disappear when the process exits.

### Safety

No game file is ever modified. Every change lives in the running process's memory and disappears when the process exits, and removing the mod's DLL leaves an entirely unmodified game behind. Each change is checked against the exact bytes it expects before it is made. A mismatch disables that one feature and leaves the game's original code running; nothing proceeds on a guess. An executable the mod does not recognize gets ordinary API forwarding and nothing else.

### Details

The 64-bit `d3d11.dll` is a proxy for the system D3D11 library. It exports the device-creation functions the game expects, then forwards them to `d3d11_proxy.dll` when one has been installed for chain-loading, or to the real `d3d11.dll` from the Windows system directory otherwise. This lets the mod observe device creation and install its rendering hooks without replacing the graphics implementation. The 32-bit `msimg32.dll` uses the same pattern for the stock launchers: it forwards `AlphaBlend` and `TransparentBlt` to the system MSIMG32 library while applying only the launcher-specific behavior described above.

Most executable changes are detours. After verifying the target function, the mod temporarily makes its code page writable, places a jump to a mod function at the entry point, restores the page protection, and flushes the processor's instruction cache. A trampoline preserves the displaced instructions and jumps back to the original function, so the mod can do work before or after normal engine behavior without replacing the routine. MinHook provides this mechanism for most game and D3D11 functions; a small in-project equivalent handles the few targets that need a fixed-size absolute jump. The launcher proxy similarly patches the stock launcher's verified entry point in memory, never in the executable on disk.

Other fixes change data rather than code. They read or update known fields in live engine objects, substitute D3D11 resources or views at API boundaries, and attach mod-owned companion resources to engine resources through D3D11 private data. Shared guarded-read helpers reject unavailable game memory before following reverse-engineered pointer chains, and mod-owned COM objects retain references for the lifetime in which the game or GPU can still observe them. These operations use per-build addresses and measured structure offsets; the mod does not search for approximate patterns and then write to whatever happens to match.

Every executable hook comes from a table keyed to a known game and build. Recognition starts with the exact executable name and `.text` size, and each target's complete expected prologue bytes are checked before any jump is installed. Launcher changes likewise require the expected process image and code signatures. A mismatch disables that feature and leaves the original path running, while an unknown executable receives only ordinary API forwarding.

All code patches, trampolines, cached pointers, and replacement resources exist only in the current process. Ending the game discards them with the rest of its memory, and removing the proxy DLL restores an entirely unmodified executable on the next launch. Configuration files written by the launcher are ordinary external settings and are the only intentional persistent changes.

## Hook boundaries

D3D11 hooks are installed only after the process is recognized as one of the six tested Arland executables. Menu detours additionally verify `.text` size and complete instruction prologues before patching anything.

Each game ships two game executables: the launcher runs the English build for `Language=2` and the multilingual build (Japanese and both Chinese locales) for every other language. The multilingual builds are separate compiles with distinct RVAs. Their entry points were located by static homologue matching against the tested English builds (shared exact byte n-grams voted per `.pdata` function, verified in both directions plus prologue and, where applicable, vtable-slot checks); at runtime the same complete-prologue verification applies before any detour is installed. Meruru's multilingual executable is SteamStub-wrapped on disk; recognition and patching happen after the stub has decrypted `.text` in memory, so the same fingerprints apply.

| Game | Executable | SHA-256 | `.text` size | Path-check RVA | Atlas-read cache |
|---|---|---|---:|---:|---|
| Rorona DX | `A11R_x64_Release_en.exe` | `2afd19db0cef3e3f0888fb62e02c9ca5929264ff5ee8c780af06213642988276` | `0x709a9c` | `0x12cc70` | Yes |
| Rorona DX (multilingual) | `A11R_x64_Release.exe` | `b6f8726df7d6cea3ffdeb171d669f8035df322552abdc90a4763523df2b4730d` | `0x72141c` | `0x135130` | Yes |
| Totori DX | `A12V_x64_Release_en.exe` | `38c41df799b207786a11c08d6bf83cec8ac10414757f935311549f74474bfd90` | `0x67da5c` | `0x18b140` | Yes |
| Totori DX (multilingual) | `A12V_x64_Release.exe` | `f8544d7b0ed22a223f080dbcdaa5f387287bccedc1f975d5ad9c304764f0aa6f` | `0x90e1ec` | `0x3a7b20` | Yes |
| Meruru DX | `A13V_x64_Release_EN.exe` | `d69cad45700457128cc8805ea3cf80dfaea0e155e6dfd2d1123277f4ebd7c19b` | `0x61ecec` | `0x1533c0` | Yes |
| Meruru DX (multilingual) | `A13V_x64_Release.exe` | `a39b854771fab1044d03c2da94afda84996eaa2ce9d60e85ca718f29b1700c73` | `0x61ae4c` | `0x140d20` | Yes |

Rorona validates paths with metadata plus filename-case enumeration, Totori with repeated metadata validation, and Meruru with metadata plus filename-case enumeration; each multilingual build matches its English sibling's behavior.

The atlas cache verifies four independent entry points per executable:

| Executable | Queue drain | Text renderer | Atlas lock | Atlas unlock |
|---|---:|---:|---:|---:|
| Rorona EN | `0x08d4b0` | `0x5613b0` | `0x3eea10` | `0x3eea60` |
| Rorona multilingual | `0x094890` | `0x577280` | `0x4048e0` | `0x404930` |
| Totori EN | `0x038a00` | `0x430bf0` | `0x4c2080` | `0x4c20c0` |
| Totori multilingual | `0x255020` | `0x6ae1f0` | `0x73f680` | `0x73f6c0` |
| Meruru EN | `0x0d6210` | `0x5115d0` | `0x3ea7d0` | `0x3ea7f0` |
| Meruru multilingual | `0x0c2e20` | `0x510c30` | `0x3e9cf0` | `0x3e9d10` |

The Rorona battle-shadow restoration is likewise dual-fingerprinted: all ten hooked shadow functions, the BtlChara-family and battle-state vtables, the shadow-manager global, and the two `ShadowHelperInit` caller return addresses have verified multilingual homologues. The Meruru conversation-cache hooks are dual-fingerprinted the same way (`BalloonBucMode` ctor/dtor in both builds).

Totori's battle addresses are mapped for both builds. Its multilingual vtables (the 22 battle states, the BtlChara family, `Chara`/`CharaBase`, the two Event executors, and the battle game mode) were resolved from MSVC RTTI complete-object locators, and its tactical-scene `hideAll`/`showAll` were homologue-matched from the English build on prologue plus a displacement-masked body comparison. Both methods were validated by reproducing already-verified values before being applied: the RTTI locator reproduces Totori's English tables exactly, and the homologue matcher reproduces Rorona's multilingual `hideAll`/`showAll` exactly. Within each group the multilingual vtables sit at a constant offset from their English homologues, and the battle game mode's constructor and destructor carry byte-identical prologues. The battle gate and the state names were then confirmed at runtime on the multilingual build. Totori does not use the `ShadowHelperInit` publish/re-entry gate on that build: the game-mode constructor and destructor already bracket each battle, and with no scene-manager helper slot mapped there is no published helper to restore.

English-only diagnostics (deep menu statistics and the shadow layer/constructor traces) remain gated to the English builds, whose RVAs are the only ones mapped for them.

Per-game availability and defaults are centralized in a capability matrix (`src/game.cpp`): the running title is detected from the executable name independently of the menu hooks, and each feature resolves through the matrix (unsupported titles are hard-off regardless of configuration) before consulting its environment override and `arland-fix.ini` key. The matrix is the source of truth mirrored by the feature tables in the README.

A lock is eligible only while the verified text renderer is active, the middleware texture reports 512×512 dimensions, and the appropriate queue- or frame-scoped cache lifetime is active. A snapshot is only ever *created* from a read lock. The middleware's lock takes an access mode, and the text renderer uses two of them per glyph: it maps the atlas for CPU writing to rasterize the glyph, then maps a staging copy for reading to blit it into the string bitmap. A write mapping is discard-mapped, so its contents are undefined on entry; snapshotting one captures uninitialized memory, and the read that follows is served that garbage instead of the atlas. Both modes are still *served* from an existing snapshot — writer and reader name the same middleware object, so the snapshot is a coherent stand-in for the whole rasterize-then-read-back round trip, which is where the cache's saving comes from. Synthetic locks and first real candidate locks are tracked per thread so only matching synthetic unlocks are suppressed; each synthetic lock retains ownership of its snapshot until that unlock, even if another thread clears or invalidates the cache in the meantime. A different real lock invalidates any frame snapshot for that texture. Installation order keeps partially installed atlas hooks inert. MinHook is used for these four entry points because Totori and Meruru expose atlas unlock through a short relative-jump thunk; every target is still checked against its complete expected prologue before MinHook is invoked.

The D3D11 synchronization hooks and the game-code menu detours are independent layers in one proxy. `ARLAND_MENU_FIX=0` skips the executable detours but leaves the synchronization layer active for a recognized Arland executable. `ARLAND_ATLAS_CACHE=0` disables atlas caching. `ARLAND_FRAME_ATLAS_CACHE=0` restricts Rorona to the queue-scoped lifetime; setting it to `1` opts Totori or Meruru into the frame-scoped path for testing.
