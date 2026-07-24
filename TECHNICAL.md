# Technical overview

## Provenance and scope

This repository combines established synchronization work with new Arland-specific research. The components should not be conflated:

- Philip Rebohle created `atelier-sync-fix` in 2022. Its central technique, replacing eligible GPU-to-CPU copies with copies through CPU-accessible shadow resources, is the foundation of `src/sync_fix.cpp`. The proxy loading, MinHook-based native D3D11 interception, staging-resource access correction, and direct-source unmap fixes also originate there.
- TellowKrinkle identified that direct game writes through `Map` and `Unmap` must update the shadow and implemented that correction for Atelier Ayesha in commit `98b5c9b`. That implementation stored one global last mapping and uploaded the complete resource on every `Unmap`.
- This project refines the Map/Unmap solution for the Arland workload: mappings are keyed by resource and subresource, references are lifetime-safe, dirty shadows are coalesced, uploads are deferred until the GPU can observe the resource, and deferred contexts cannot perform invalid staging reads. This refinement fixed the corrupted-text case encountered during the investigation while avoiding thousands of redundant atlas uploads.
- TellowKrinkle's rendering fork also established the old-Arland render-target and viewport/scissor correction ported into this project. The released configuration retains that resolution logic while anti-aliasing remains disabled by default; shader replacement, anisotropic filtering, and LOD-bias features are not included.
- Nico Verbruggen, the author of this repository, led the reverse-engineering and runtime investigation behind the Arland-specific work in this project: the menu-stutter fix (the `.PSSG` validation cache and font-atlas read caches), the battle and cut-in shadow restoration, and the high-resolution UI font rendering. That work was carried out with the assistance of large language models. None of it is part of the original `atelier-sync-fix`.
- Yuri Hime's Atelier Graphics Tweak, together with the earlier Rorona community investigation, identified the broader font-atlas GPU-transfer problem that this project's synchronization and atlas-cache work addresses. AGT's experimental upload-suppression approach is examined but deliberately not used here; see "Relationship to Atelier Graphics Tweak" below.
- MinHook is an independent library by Tsuda Kageyu and contributors, bundled unchanged under `vendor/minhook`.
- The high-resolution UI text feature rasterizes glyphs with stb_truetype (Sean Barrett, public domain), vendored unchanged under `vendor/stb`. Its bundled replacement typefaces are National Park (the default) and Cuprum, both under the SIL Open Font License, embedded in the DLL (generated at build time from the vendored `.ttf` files under `vendor/font/` by `scripts/embed_font.py`, so no large byte arrays are checked in) and selected by `[Rendering] FontName`; a `arland-hires-font.ttf` placed beside the DLL overrides them.
- SMAA is by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, and Diego Gutierrez (MIT). Its reference shader and the precomputed `AreaTex`/`SearchTex` lookup textures are vendored unchanged under `vendor/smaa`; the mod adds only the runtime integration (compilation, the three-pass pipeline, and the pre-UI injection). The SMAA preset (`ULTRA`) and injection approach follow Yuri Hime's Atelier Graphics Tweak, which shipped the same SMAA for these games.

The current code supports the exact tested Arland DX executables (the English builds and, since v0.4, the multilingual builds used for Japanese and Chinese) and contains the validated D3D11 synchronization and menu-performance fixes described below.

## D3D11 synchronization stalls

The Arland ports frequently copy GPU resources into CPU-readable staging resources and then map them. A normal D3D11 `CopyResource` followed by `Map` forces the CPU to wait for the GPU. Font-atlas activity makes this especially visible while constructing text-heavy menus.

The original algorithm, retained here, first examines both resources involved in `CopyResource` or `CopySubresourceRegion`. When the destination is immediately CPU-writable and the source is not CPU-readable, it creates a staging shadow for the source with both `D3D11_CPU_ACCESS_READ` and `D3D11_CPU_ACCESS_WRITE`, initializes it from the real resource, and stores it as private data on that resource. Compatible later copies map the destination and the source shadow and use row- and depth-pitch-aware CPU memory copies. Unsupported formats, layouts, contexts, or busy destinations fall back to the real D3D11 copy.

This is why the optimization targets synchronization rather than general rendering: it does not replace ordinary texture uploads, decompression, shader execution, or asset loading. It removes the particular round trip where the game schedules GPU work and then immediately blocks the CPU to retrieve the result.

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

The English Arland executables repeatedly validate identical immutable `.PSSG` archive paths while recursively building UI records. Rorona and Meruru perform a metadata lookup followed by a filename-case directory enumeration; Totori repeatedly performs its corresponding metadata validation.

During one unmodified Rorona Status-menu build, only `a11r_menu_EN.PSSG` and `a11r_common_EN.PSSG` were validated 1,245 redundant times. The operating-system file cache cannot eliminate the overhead of repeated path conversion, opens, metadata queries, directory enumeration, Wine/NT transitions, and handle teardown.

The mod detours the complete game-side validation helper rather than a Windows file API. The original helper runs for the first check. A successful `.PSSG` path is remembered for the process lifetime, and later checks of that exact path return success. Failures are never cached, so resources that appear later can still be discovered. Non-PSSG paths, archive reads, parsing, decompression, and UI ownership remain unchanged.

The cache stores complete path strings, not file handles, file metadata structures, PSSG contents, or constructed UI graphs. This is the narrowest lifetime that matches the underlying invariant: shipped PSSG archive paths do not become invalid during one game process, while parsed UI objects remain mutable and menu-owned.

These desktop measurements were captured on an AMD Ryzen 7 5800X3D, Radeon RX 7900 XTX, and 32 GiB of RAM. On Steam Deck, affected menus were observed taking roughly 4–7 seconds without the fix.

| Rorona workload | Before PSSG cache | After PSSG cache | Saving |
|---|---:|---:|---:|
| Status menu | 916.6 ms | 103.1 ms | 88.8% |
| Quest/Container/Basket test | 632–711 ms | 135.6 ms | 78.5–80.9% |

## Repeated font-atlas reads

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

A process-lifetime snapshot was rejected because the atlases are mutable. Clearing the cache for every rendered string was also rejected because it regressed the same operation to roughly 2.5 seconds. The frame-scoped Rorona path reduced a repeated Synthesis open from 2,822 Maps and 865 copies to about 230 Maps and one copy; measured Map time fell from roughly 200–211 ms to 0.15–0.19 ms.

### Relationship to Atelier Graphics Tweak

The community identified the broad transfer problem years before this project. The original [Rorona menu-loading investigation on Steam](https://steamcommunity.com/app/936160/discussions/0/1742227264210806751/?ctp=2) describes the game rendering individual characters through 512×512 CPU textures, compositing them through video memory, and moving gigabytes during a bad menu frame. Yuri Hime's later [Atelier Graphics Tweak discussion](https://steamcommunity.com/app/1152300/discussions/0/3345546664208090238/) introduced an experimental anti-stutter option intended to remove that traffic.

Static analysis of the archived AGT `dinput8_antistutter.dll` shows that it hooks D3D11 texture creation, `Map`, and `Unmap`, recognizes the 512×512 dynamic `B8G8R8A8` font-atlas write path, and prevents selected transfers from executing. This can be extremely fast, but it suppresses uploads without proving that their contents are redundant. The AGT author documented missing text and a reproducible boss-battle crash, and later withdrew the optimization after a user-reported crash.

This repository does not copy that suppression behavior. It hooks the verified game-side text renderer and atlas lock/unlock routines, performs the first required read, and replays an immutable CPU snapshot for later reads within a bounded lifetime. It never suppresses a required atlas write: an unmatched real lock invalidates the affected texture, and Rorona discards every snapshot at the next frame boundary. The `.PSSG` validation cache, queue-scoped atlas cache, and Rorona frame-scoped extension were independently derived from runtime traces during this project's menu-hitch investigation. AGT remains important prior work for identifying the general GPU-transfer problem and demonstrating its potential performance impact.

An additional trace tested whether AGT-style upload avoidance could usefully extend the frame-scoped fix. Across seven repeated Rorona Synthesis openings, each 132–144 ms transition issued about 17 MB of dynamic texture writes, but the D3D11 write-map calls themselves averaged only 0.115 ms. All Map calls averaged 0.246 ms, texture creation averaged 1.60 ms, copies averaged less than 0.001 ms, and the synchronization shadow-flush path was never entered. The approximately 96–101 ms queue drain therefore remained CPU-side construction work, not GPU-transfer latency. Suppressing these required writes would trade correctness for well under one percent of the remaining transition time, so the repository deliberately does not implement it.

Deeper tracing localized part of that residual Synthesis cost to eager construction of recipe UI that is not necessarily visible. One repeatable example was the hidden string `True Dragon Hourglass`: one of its four render operations took about 19–21 ms, while the other three took roughly 0.12 ms each. The slow operation made 63 atlas locks; 60 were served by the frame cache, while the first read of each of the three mutable font atlases accounted for essentially the entire 19–21 ms. This is consistent with Synthesis constructing a complete recipe interface, including unavailable or off-screen entries, rather than limiting work to the currently visible list.

Retaining those three atlas snapshots across frames could avoid this remaining first-read cost after the initial construction, but the atlases are mutable and proving complete invalidation coverage would require a broader correctness investigation. The current frame boundary remains the deliberate safety limit. No additional optimization is applied for this residual cost at present.

## High-resolution rendering

The games accept high render dimensions, but the settings launcher filters its lists through Windows display-mode reporting. DPI virtualization and the current desktop mode can therefore hide a resolution that the game and display can use. The launcher stores independent fullscreen and windowed arrays, so both have to be corrected.

The 32-bit `msimg32.dll` proxy handles this. Loaded by the shared settings launcher, it verifies the exact process image, allocator, and code signatures, then expands both mode arrays with the launcher's own allocator and appends the missing canonical modes. It guarantees that 1920×1080, 2560×1440, and 3840×2160 stay available in both states. Forcing 1920×1080 is specifically useful on Steam Deck and other lower-resolution, high-DPI handhelds that would otherwise hide it, and for docked use.

The proxy forwards the launcher's two imported image functions, `AlphaBlend` and `TransparentBlt`, to the system MSIMG32 library. MSIMG32 is used instead of WinMM because native DirectX initialization can dynamically depend on WinMM exports beyond the two functions the launcher imports directly.

The relevant launcher binaries are structurally identical across the trilogy. Every `ArlandDXEnv.exe` has image size `0x317000`, `.text` SHA-256 `32c441b19f242a249145215eb9b4be315095563b839f23762c18519f8fedc4cc`, and `.data` SHA-256 `eb3bc4cdce506b628e36b6d5dac94951142ca40e90af8f38ab508d72759c0fe2`. Their complete files differ only in sections containing game-specific material outside the patched paths:

| Game | `ArlandDXEnv.exe` SHA-256 |
|---|---|
| Rorona DX | `167e1c141d0faa03e7baca0034a672f6d8023b446473a6daad6c10b71d5b9667` |
| Totori DX | `c251c7e747e027f75d6e37e4e317cfb599b0db378db9e27ba09043619e02c226` |
| Meruru DX | `fa64db36c92c34429c6c2709ab2126c1ce48f839d9eddd2de1a992564182c732` |

The three outer `ArlandDXLauncher.exe` files likewise have identical `.text` SHA-256 `58ba7aee62d924d35ca160829766bc8775125475894473bcbadf92d962fcc522` and import the same `AlphaBlend` and `TransparentBlt` entry points. The proxy remains forwarding-only in that process and installs resolution hooks only in `ArlandDXEnv.exe`.

The 64-bit game DLL provides a second, launcher-independent path. Blank `Width` and `Height` keys are created in `arland-fix.ini` by default. When both are changed to valid dimensions, the DLL replaces the swap-chain request, clears the inherited refresh-rate constraint, and resizes the matching first main depth target before applying the ordinary auxiliary-target and raster corrections. Missing, blank, incomplete, or out-of-range values leave the launcher's selection unchanged.

Selecting a larger backbuffer is not sufficient by itself. The old render path creates the main depth target at the requested dimensions but later creates auxiliary render/depth targets and submits viewport/scissor state hard-coded to 1920×1080. It also records rendering through a deferred D3D11 context. Correcting only the immediate context therefore produces genuinely large targets with a 1920×1080 image confined to their upper-left corner.

The D3D11 layer learns the larger main-target size and resizes only later exact-1920×1080 render/depth targets created without initial data. Raster state is tracked independently for the immediate and deferred context paths. When an affected target is bound, exact full-screen 1920×1080 viewport and scissor state is replaced with that target's dimensions before drawing on the same context. This produces direct native 2560×1440 and 3840×2160 rendering; neither mode is a 1080p upscale, and 1440p is not implemented by rendering at 4K and downsampling. Ordinary 1920×1080 and lower-resolution operation remains unchanged.

Rorona's blurred-dialog path contains two additional fixed-resolution assumptions. It copies only a 1920×1080 source box into the dialogue snapshot and submits a four-vertex quad whose positions cover `0..1920 × 0..1080`. Merely resizing the render targets therefore leaves the right and bottom of the snapshot black and limits the blurred output to the same upper-left region. When both copy resources match the enlarged main target, the D3D11 layer expands that exact source box to the configured dimensions.

The 48-byte quad is shared with portraits and other cutscene layers, so changing its original contents globally causes those assets to flash briefly in incorrect positions. The fix preserves the game's buffer and attaches a scaled companion instead. The copied snapshot is tagged, that identity is propagated through the three-vertex blur passes, and the companion is bound only for the final four-vertex draw that composites a processed blur result. The original buffer is restored immediately afterward. Other copy regions, vertex buffers, and cutscene draws are left unchanged.

Totori and Meruru share the blur/capture engine classes and post-blur shaders but did not issue Rorona's exact fixed-size snapshot copy during validated 2560×1440 dialogue scenes. Their ordinary resized blur targets filled the output correctly, so the exact runtime predicates leave the Rorona-specific correction inert in those games.

The D3D11 layer also contains an optional multisample render-target and resolve path, adapted from TellowKrinkle's rendering work. On first launch it seeds `arland-fix.ini` with `MSAA=1`, `Width=`, and `Height=` under `[Rendering]` if the file is absent. The path stays inactive unless the MSAA value is changed to `2`, `4`, or `8`, or the higher-priority `ARLAND_MSAA` environment variable requests one of those values; absent values and values below two use the original single-sample path, and unsupported requests fall back through lower sample counts.

The game keeps owning single-sample host resources, while matching multisample color and depth targets are attached as private data and resolved before reads, copies, render-target changes, shader-resource binding, and deferred command-list completion. Four-sample MSAA was visually validated at 2560×1440 in Rorona, Totori, and Meruru, including each game's blurred-dialog rendering. It remains opt-in because behavior can still vary across GPUs and drivers.

Optional anisotropic filtering is a separate, cheaper texture-quality knob. The games create their texture samplers with plain linear filtering, so obliquely-viewed surfaces (floors, walls, receding ground) blur. When `AnisotropicFiltering` (or `ARLAND_ANISO`) requests `2`, `4`, `8`, or `16`, the D3D11 layer hooks sampler creation and, for samplers using a basic point/linear filter, substitutes anisotropic filtering at the requested maximum-anisotropy level before the sampler is created. Comparison, minimum, and maximum filters (shadow PCF and similar) are recognized by their filter enum and left unchanged, and the hook is installed only when the feature is enabled. Because the upgrade happens once at sampler creation, there is no per-draw or per-frame cost. Off by default.

## Battle shadow restoration

Atelier Rorona DX renders ordinary battles without any character or enemy ground shadows. The engine's shadow pipeline is present and functional (the field map uses it), but the port's battle scene setup never registers the battle actors as shadow casters. Atelier Meruru DX, by contrast, registers its battle casters natively, which is why its ordinary battle shadows work out of the box.

The restoration hooks the engine's shadow-helper initialization and observes its two static call sites: the battle scene-setup path and the field-map re-entry path. When the battle path runs, the mod records the battle game-mode object and the battle's shadow helper, locates the party's character vector inside the game-mode (verified through the BtlChara-family vtables), and registers each character's render node as a shadow caster through the engine's own `ShadowCharacterBuild` routine, the same call the field map uses. The battle helper is then published to the engine's global active-helper slot so the shadow traversal walks the battle casters; the displaced field helper is remembered. Enemies are registered through the same container discovery. Because the engine performs the actual shadow rendering, the restored shadows use the game's own pipeline end to end; the mod injects bookkeeping, not draw calls.

Alongside registration, the mod tracks the battle state machine by recognizing the `GmStateBtl`-family state vtables (Enter, SelectCommand, WaitAction, the Result states, and so on). This state tracking is what the cut-in features key on, and it has to disengage reliably.

The complication is that returning from battle to an already-loaded field map does not re-run the field's shadow-helper initialization, so there is no engine event to observe on exit. A per-frame watchdog fills that gap. It checks whether the battle game-mode still looks alive, meaning its party vector still holds objects with BtlChara vtables. Once a game-mode that was previously seen alive stops looking alive for twenty consecutive frames, the battle is over, so the saved field helper is restored to the global slot and all battle tracking is cleared.

Arming the watchdog only after the game-mode has been seen alive prevents a slow battle intro, where the party is not yet spawned, from tripping it. Without the watchdog, the tracking kept scanning the freed battle objects every frame after combat and degraded field performance.

## Battle cut-in shadows

Atelier Rorona DX renders attack "cut-ins" (the brief close-up when a character or enemy acts) without ground shadows and with a visibly darker scene. This is original behavior on every platform, not a port regression, and both symptoms trace to a single animated constant.

The battlefield ground is a shadow receiver, and its 880-byte material shader gates shadow reception on scene-light intensity. The vertex shader reads a `diffuse` value at byte 832 of the material constant buffer and computes a gate of `2.5 - 2 * min(diffuse.w, diffuse.x)`. The pixel shader samples the shadow map only when that gate is below one, that is, when the smaller diffuse component exceeds `0.75`. (The pixel shader's own reflection names byte 832 differently, `shadowLPos`, a collision that obscured the mechanism during investigation; the value that matters is the vertex shader's `diffuse`.)

A separate sixteen-byte scene-light parameter drives visible floor brightness from the same logical intensity. The cut-in animates that intensity down to roughly `0.7`. That simultaneously darkens the floor and, sitting `0.05` below the `0.75` reception threshold, trips the gate closed. With the gate closed the receiver never samples the shadow map, so every object (acting character, party, and enemies alike) loses its shadow at once while the floor dims. The shadow casters are unaffected and keep rendering into the shadow map; only reception is switched off.

Holding the gate open is not enough by itself, because the engine runs a deliberate caster cover-up during the cut-in, and the closing gate was hiding its seams.

When a cut-in begins, the non-focus battlers are hidden and repositioned ("juggled"), but their per-node caster flags (`PNode::setCastShadow`) are cleared only about a quarter second later, deferred with the visual cross-fade, then restored instantly at cut-in exit, before positions finish restoring. Vanilla never shows stale shadows for these characters because the fading scene light crosses the 0.75 reception threshold during exactly those windows, so the closed gate covers the stale casters. Holding the gate open naively therefore exposed stray floor shadows for characters that were invisible or off-position.

The fix front-runs the engine instead of waiting for it. Hooks on the tactical-scene `hideAll`/`showAll` wrappers, the functions the cut-in machinery calls to fade the non-focus battlers out and back, clear the registered casters' flags the moment the hide starts, re-clear them when the show's instant restore fires, and restore them about 0.3 s later once the juggle has settled. Visible cut-in participants keep their shadows, because the event system's own immediate show paths re-set their flags.

A third window sits inside the cut-in itself. The event choreography hides individual non-focus battlers through a deferred per-actor alpha fade of about 0.25 s, and the engine only drops a faded-out model from the shadow map when that fade expires. (Its fade-end handler recursively hides the whole model subtree, shadow nodes included.) During the fade the model is alpha-invisible but still casts, so with the gate held open its full-strength shadow lingers on the ground. Rather than edit the scene graph directly, the mod front-runs that expiry: it hooks the leaf that arms a battler's fade and, when a hide latches during a cinematic state, zeroes the model's fade timer. The engine's next visibility tick then performs the complete native hide, the recursive subtree clear plus the state flags its cancel and re-show path depends on, so there are no manual node writes and the focus actor (which the hide enumerator never arms) is untouched. The only cost is that the hidden battler pops out instead of fading over a quarter second, off-camera and negligible.

This replaced an earlier approach that cleared the caster registry's visibility flags directly. Those registry entries turned out to be model locator roots, not the drawable shadow leaves, so clearing them never touched the shadow map. The leaves hold their own visibility flag, which only the engine's subtree hide clears.

With the stale casters cleared from the first frame, the brightness and reception hold can engage immediately and the cut-in never visibly dims. The `hideAll` prologue is byte-identical across all five battle-capable executables; `showAll` differs per engine generation and is verified per build. If these hooks fail to install on some build, the hold falls back to a transition-aware mode: it engages only once the observed dim value has been bit-identical for at least 60 ms (after the entry fade has bottomed out, by which time the engine has cleared the juggled casters), eases up over a further 120 ms, and never engages during the exit fade. That preserves the vanilla cover at the cost of a brief visible dim.

The restoration addresses the two halves separately during cinematic battle states, and both are configured through the `[Battle]` section of `arland-fix.ini`. Both default to on across all three games.

`BattleCutInDimming` governs brightness. It defaults to `true`, which allows the vanilla cut-in darkening; setting it `false` holds the sixteen-byte scene-light parameter at `1.0` and keeps the floor lit. `BattleCutInShadows` governs reception. It defaults to `false`; setting it `true` forces the receiver material's `diffuse` back to `1.0` immediately before each shadow-receiving ground draw through a bounded sixteen-byte update over the `[832, 848)` field, which reopens the reception gate. The two are independent, so the vanilla darker cut-in can be kept while shadows are restored, or either half enabled on its own. Both restorations are off by default. The `ARLAND_CUTIN_SHADOWS` and `ARLAND_CUTIN_DIMMING` environment variables override the respective keys for a session, and the per-game defaults live in the capability matrix in `src/game.cpp`.

On-by-default was only safe once the stray-shadow glitch described above was fixed. Before that, the restored reception showed a ground shadow for a character the engine had hidden or repositioned during the close-up; the settle-gated hold and the force-expiry per-actor hide resolved it, validated in all three games.

The patch itself is narrow. It touches only the diffuse field, never the transform matrices that share the buffer, and the pixel shader does not read that field, so the shared vertex and pixel constant buffer is patched safely; the draw-time path also works on the game's deferred rendering context. The engine's own casters then project real shadows onto the cut-in floor with no injected geometry, and the feature composes with the always-on battle-shadow restoration that supplies those casters. Basic and assist cut-ins keep the real arena floor on screen and gain shadows; solo specials that replace the entire background with a dedicated close-up scene have no real floor and are left unchanged.

Atelier Meruru DX shares the same engine and the same cut-in reception gate, and the restoration applies there too. It differs only in the source of the casters: Meruru registers its battle shadow casters natively (per character, through a build path Rorona lacks), so its ordinary battle shadows already work and no caster restoration is needed, only the cut-in reception gate, which the same value-matched patches reopen. The single Meruru-specific addition is battle-state detection: its `GmStateBtl*` cinematic states were located by RTTI so the patches fire during Meruru cut-ins exactly as they do for Rorona.

Atelier Totori DX (English build) received the same battle-state treatment after a static-plus-runtime investigation established that its fighting shadows are natively healthy: like Meruru, its battle characters register as casters through the game's own build path (confirmed by a runtime probe: configuration byte set, helper context live before the character constructors, caster registry filling). Totori is the structural outlier: its battle shadow helper is embedded at a different game-mode offset, it has no global active-helper slot at all (field and battle each render through their own helper, so the helper-publish machinery is inert there by design), its state machine lacks `SelectDefence`, and its result chain uses different state names (`Result`, `AddPay`, `DropItem`, `LvUp`), which the cinematic-state list carries. These per-game differences are encoded in the battle address pack (`BattleBuildAddrs`), including the helper embed offset and zeroed entries for the structures Totori does not have. The multilingual Totori build is not yet covered.

Totori's cut-in mechanics also differ at the shader level. Its shader set was rewritten for D3D11 (`commonShaderWin.PSSG`, GatherCmp PCF, per-shader `$Params`), while Rorona and Meruru ship the PS3-style pack. That has two consequences.

First, Totori's battle ground receiver has no shadow-reception gate at all. Its PCF sampling is unconditional and `diffuse` only tints the final color, so the gate-hold patch has nothing to reopen there, and no 880-byte constant buffer exists anywhere in Totori's shaders. Missing cut-in shadows in Totori are a caster-side matter, still under investigation.

Second, the cut-in dim is the same `BtlField` fade to (0.7, 0.7, 0.7, 1.0) as Rorona's, but it reaches the GPU through Totori's own constant-buffer layouts rather than the 16-byte `$Params`. A runtime constant-buffer trace during a cut-in also established that Totori's battle arenas render with the fieldmap shader family, not the dedicated battle-ground shader the static analysis predicted. The dim flows through the fieldmap layouts, with `diffuse` at (ByteWidth 304, offset 16), (48, 32), (80, 0), (144, 0), and (160, 16), alongside the battle and character layouts (32, 0) and (16, 0), and the toon vertex-shader families at (224, 208), (13024, 13008), (12960, 12944), (160, 144), and (96, 80).

The dim-hold consults this per-game field table with one value predicate everywhere: only a uniform (s, s, s, ~1) with s in (0.5, 0.98) is rewritten. It deliberately excludes the (304, 272) location the trace also matched, which is a light-matrix row. The fieldmap vertex shaders gate shadow reception on the same `diffuse` (closing below 0.85), so on Totori the dim-hold doubles as the reception hold. It carries the same settle gating as the Rorona and Meruru gate-hold, so holding these fields also restores cut-in floor shadows, and the settle delay preserves the vanilla stale-caster cover at the transitions.

## High-resolution shadow maps

The games render all shadows into two 1024×1024 `R24G8_TYPELESS` depth maps, a caster map (A) and a receiver map (B) with a per-frame A→B transfer between them, so shadow edges are visibly blocky, most noticeably in Meruru. The `ShadowMultiplier` option renders shadows at 2048, 4096, or 8192 instead.

The engine's own maps are never resized: the engine sizes viewports, copies, and memory assumptions from its texture metadata, and an in-place resize would invalidate them. Instead, each eligible 1024×1024 shadow-map creation also creates a separate mod-owned enlarged "twin" texture, attached to the engine texture as private data so the two share a lifetime, and when the engine releases its map, the twin is released with it. Anything ambiguous at creation (initial data, staging or CPU access, mips, arrays, MSAA, misc flags) declines the twin and keeps that map on the vanilla path.

The shadow pipeline is then redirected onto the twins at four points, each inert when no twin exists:

- the engine's shadow-map clear is mirrored onto the twin;
- depth-only caster binds of a shadow-map DSV are redirected to a lazily created twin DSV (binds that pair the shadow map with a color target fail safe to the vanilla pass);
- the engine's A→B shadow-map transfer is mirrored as an equal-sized copy between the twins;
- the receiver's shadow-map SRV bind is substituted with the twin's SRV, with a pointer-keyed negative cache so the hot bind path stays cheap.

Two size assumptions still need correcting, because the engine sizes everything from its own 1024 metadata. Exact 1024×1024 viewport and scissor state is rewritten to the twin's dimensions during the redirected caster pass. And the receiver material's PCF tap size, which encodes the shadow texel size in the same 880-byte constant buffer the cut-in fix patches, is rescaled so filtering matches the enlarged map. At `ShadowMultiplier=1` none of this machinery activates and the shadow pipeline is untouched.

## Meruru conversation text-render cache

Atelier Meruru DX's field-map conversations with animated bust-up portraits collapsed the framerate on the English executable for the duration of the conversation. The cost was not the portraits: the conversation balloon's per-frame callback pump re-entered the executable's text-render path (the same CPU-side glyph and atlas work that makes menu construction slow) every frame, for text that had not changed. Menus pay that cost once per rebuild; the balloon paid it continuously.

The mod already contained a text-bitmap replay cache built for menu diagnosis. `cachedRenderText` keys on the renderer, font, atlas, style, and the exact string, and replays the previously rendered output bitmap into the caller's buffer instead of re-rendering. Its lifetime, though, was scoped to a single queue drain.

The fix gives that cache a cross-frame scope bounded by the conversation. Hooks on the `BalloonBucMode` constructor and destructor count live conversation balloons, and while any balloon is alive the cache activates and its per-drain clears are suspended, so a string that is identical from frame to frame costs one memory copy. The destructor of the last balloon clears the cache.

Two edges are handled. The typewriter reveal inserts one entry per partial string, so the cache is bounded and an overflow clears and rebuilds it. And a replay whose target buffer is too small falls back to a real render rather than reallocating through the game's allocator, a case that cannot occur for unchanged text. The hooks verify the constructor and destructor prologues per build (the destructor check includes the RIP-relative load of the `BalloonBucMode` vtable, pinning it to the right class) and are installed for both the English and multilingual executables.

## High-resolution UI text

All UI text in these games comes from a pre-baked bitmap font. Koei Tecmo's G1N atlases store every glyph as a fixed 32×48 image that the engine blits 1:1, with no scalable rasterizer, so text is soft and pixelated at 1440p or 4K. This feature re-renders that text at full resolution while preserving the engine's exact layout. `[Rendering] Font` chooses the mode: `replaced` (the default), `upscaled`, or `default`/`off` (the untouched bitmap).

Every mode works the same way at the top level: let the engine render a string normally, then swap the result. When the engine finishes a string it leaves an "output object" at `renderer+0x1a0`. That struct holds the power-of-two bitmap width and height, a pointer to its 8-bit alpha pixels, four normalized metrics (used-width, used-height, and line-height, each a fraction of the pow2 size), and the line count. The mod reads it, builds a higher-resolution bitmap of its own, and writes the new pixel pointer and doubled (`kScale = 2`) dimensions back into it.

One detail makes or breaks the swap. A text quad's on-screen size is computed as `fraction × pow2-dimension`, so doubling the bitmap's dimensions would double the text on screen. Two things prevent that. First, the replacement bitmap is allocated through the engine's own text-buffer allocator, so the engine still frees it correctly. Second, the doubled dimensions are restored to their originals once the string's consumer has built its GPU texture, through a hook on that consumer. The glyphs end up twice as dense while the quad keeps its original size. A per-string result cache, keyed on the string and font state, reduces a repeat render to a memory copy and keeps the work off the hot menu path.

`upscaled` keeps the engine's own glyphs and only raises their resolution. The baked bitmap is filtered up 2×, by default an SDF-style alpha-coverage steepen (bilinear and Lanczos are also available), followed by an unsharp pass. Nothing about the layout changes, so multi-line text, alignment, and the game's custom icon glyphs all survive untouched.

`replaced` re-renders each string from a bundled scalable font (National Park by default, or Cuprum), rasterized with stb_truetype through a glyph-atlas cache so each glyph is drawn once and then reused. Matching the engine's layout took reverse-engineering the text renderer, and three points matter:

- Line breaks come only from an explicit `\n`; there is no word wrap.
- Line pitch is read from the engine's own line-height metric (`output+0x18`), not from an even division of the used height. The engine lays text out as `usedH = topOffset + numLines × lineHeight`, so dividing evenly overstates the pitch and multi-line text slowly drifts off the paper's ruled panels. Using the real line-height keeps every row aligned, and the block is centered in the used height so the single-line case matches the simple even split.
- Baselines are placed against a fixed cap-height reference, measured once from `H`, rather than each string's own ink. A line with descenders no longer rides higher than an all-caps one.

The game's English text also carries characters that look like ASCII but are not, and a Latin font has no glyph for them: full-width digits and letters (`１２３`, U+FF01–FF5E), CJK label brackets (`【】`), and Unicode Roman numerals (`Ⅰ–Ⅻ`, used for dungeon tiers). The decoder folds each to its ASCII equivalent before layout. If the font still cannot draw a character, most often one of the controller-button icons, the whole string falls back to the `upscaled` path, so no glyph is ever dropped or shown as a tofu box.

The replacement typefaces are compiled into the DLL from the vendored `.ttf` files under `vendor/font/` (the byte arrays are generated at build time by `scripts/embed_font.py`, not checked in), so the feature needs no loose font file; a `arland-hires-font.ttf` beside the DLL overrides them. The substitution is wired only for the English executables, because the per-game text allocator and consumer hooks resolve English-build addresses. On the multilingual builds those hooks stay unresolved and every mode is a safe no-op, so Japanese and Chinese text is never touched (and neither Latin font carries CJK glyphs regardless).

A small related mechanism rides the same render path to correct known English display typos. A per-game table rewrites the string pointer handed to the renderer before it is drawn or cached, for example Totori's "Synth Cateogry" to "Synth Category". Only the displayed text changes, the game's own string data is untouched, and because the rewrite happens on the render path the correction applies in every font mode.

## SMAA anti-aliasing

The games' only built-in anti-aliasing is none; the mod adds optional MSAA, but MSAA only multisamples geometry silhouettes and cannot touch aliasing that lives inside a surface, most visibly the thin alpha-tested costume trim (frilled lace on sleeves, collars, skirts). The mod therefore also offers SMAA (Enhanced Subpixel Morphological Anti-Aliasing, Jimenez et al.), a post-process that works on the finished image and smooths any visible edge regardless of how it was produced. It is enabled by default and is a cheaper, broader alternative to MSAA: a single constant-cost pass rather than per-pixel supersampled shading.

SMAA runs the standard three passes (luma/colour edge detection, blending-weight calculation against the precomputed `AreaTex`/`SearchTex` lookup textures, and neighborhood blending) using the reference shader (vendored under `vendor/smaa/`, MIT) compiled at runtime through `d3dcompiler`, at the `SMAA_PRESET_ULTRA` quality level. If the runtime shader compiler is unavailable or any resource fails to create, SMAA disables itself for the session and the rest of the mod is unaffected.

The pass is injected before the UI is composited, matching the approach the Atelier Graphics Tweak used, so the HUD, menus, and text stay crisp. The injection point is the frame's first bind of the main-size colour render target without a depth buffer: the 3D scene renders to that target with depth testing, and the UI is drawn onto it afterward with depth off, so that transition is exactly the moment the scene is finished but the UI has not yet been drawn. Under MSAA the scene lives in the multisample twin and is resolved to this single-sample target by the existing resolve immediately beforehand, so either way the target holds the finished scene when SMAA runs; the antialiased result is written back in place and the UI composites on top of it. It runs once per frame, latched and reset at Present. `ARLAND_SMAA_PREUI=0` falls back to applying SMAA at Present over the fully composited frame (which also softens the UI slightly); `ARLAND_SMAA_DEBUG=1`/`2` visualize the edge and blend-weight intermediates.

The fine costume trim specifically is beyond what SMAA, or any edge-based or coverage-based technique, can resolve on these games: the trim is a hard alpha-test cutout of a DXT1 one-bit-alpha texture, so its edges are sub-pixel and near-binary, and TellowKrinkle's per-sample sample-rate-shading approach is documented as non-functional on the older Arland DX titles. Full supersampling is the only technique that resolves it, and is tracked separately.

## Diagnostics

If a game crashes, a last-chance exception filter appends a post-mortem to `arland-fix.log` before the process dies: the exception code, the faulting address expressed as module+offset, the register state, and a conservative stack scan in which every stack slot pointing into executable module code is resolved to module+offset. The faulting module is also bucketed into a coarse category (`AUDIO`, `GRAPHICS`, `GAME`, `MOD`, `SYSTEM`, `OTHER`) and an audio module anywhere in the stack is flagged, so a fault the D3D11 layer cannot address is identified as such. The handler chains to any previously installed filter and lets the exception continue, so debugger and Wine crash handling are unaffected. The previous session's log is preserved as `arland-fix.log.old` on each launch so a crash report survives the next start.

With `[Diagnostics] VerboseLogging` enabled (off by default; `ARLAND_VERBOSE_LOG` overrides), process memory (working set, peak, commit) is logged as a periodic `MEM` line roughly every ten seconds. This is a passive probe for a crash that hangs rather than throwing (a runaway allocation that the exception post-mortem never sees), so a memory climb is captured in the log before the hang. An `ARLAND_MENU_STATS=1` session additionally logs per-transition cache statistics, per-conversation cache hit/miss totals, and a periodic per-frame heartbeat (text-render calls and time, cache hits and misses, and the battle/cinematic tracking flags) used to localize frame-time regressions.

## Crash analysis (Atelier Totori DX)

Atelier Totori DX has two widely reported crash classes. They occur in the unmodified release game, and this mod fixes neither yet. Both were reverse-engineered to specific engine code, and in both cases that code is byte-identical to Rorona and Meruru, which do not crash. Each is therefore driven by runtime usage or timing rather than a defect unique to Totori's binary, so pinning down the exact mechanism needs runtime evidence, not static analysis alone. The diagnostics above exist to capture them.

The entries below record what we currently believe each bug is. Both still need two things that are not done yet: runtime confirmation of the mechanism, and an actual fix in the mod.

**In-battle bomb/Craft crash (audio path).** This is reported as a crash preceded by an audio screech, frequently degrading into a whole-system hang with memory climbing to 100% rather than a clean fault. The audio stack is Koei Tecmo's shared KTGL sound library (`ktgl::CSoundEffectManager` / `CSoundDevice`, XAudio2 2.7 backend).

We believe the mechanism is reclaim-starvation. Every sound-effect play heap-allocates a play-instance (allocation site `0x2b68e6`) and a voice-instance (`0x26171`) and creates a source voice, and these are reclaimed only asynchronously, when a voice signals finished. In dense fights, bomb and Craft bursts request plays faster than reclaim frees them; a stuck looping voice (the screech) can also never signal finished and never be reclaimed. Either way the instances accumulate without bound until memory is exhausted. The backend holds a fixed voice set rather than a growable pool, and the sound-effect assets were verified structurally clean across all three games, so this looks like reclaim-starvation under load rather than a bad asset or a missing free on a normal path. Because it hangs, the crash post-mortem never runs, and the periodic `MEM` line is what would capture the runaway climb.

This still needs runtime confirmation that allocations outrun frees during a bomb-spam fight, and then a mod-side fix that bounds the KTGL request and voice population (for instance capping concurrent play-instances at the allocation site, or force-reclaiming an idle voice when the pool is saturated).

**Menu-close and battle-enter crash (scene-transition teardown).** This is reported on leaving vendor menus and entering battle, roughly half the time, independent of Proton version, and avoided on Steam Deck by enabling "allow screen tearing." Both transitions run through the engine's post-effect transition system (the `Pe*` wipe/fade classes), which samples a captured screenshot of the outgoing frame during the wipe.

We believe the mechanism is a CPU-side cross-thread lifetime race. When the transition completes, the `PeFrameCapture` destructor at `0x1e07a0` releases those captures, returning each texture's C++ wrapper object to a pool allocator synchronously on the teardown thread. The engine records graphics on worker threads (it spawns a worker pool and drives deferred contexts), so the teardown thread can free the capture, dropping the COM object's last reference and returning the wrapper's memory to a reusable pool, at the same moment a render thread is still binding or dereferencing it. That would hand a racing or freed pointer into the graphics runtime. The signatures fit this reading: the fault sits inside `d3d11.dll` (a bad COM pointer), the odds are roughly even (a race window rather than a deterministic error), it is independent of Proton version (the game's own threading bug, present on native Windows too), and disabling vsync shifts the phase between the two threads so their windows stop overlapping.

This still needs runtime confirmation, correlating the thread that frees the capture with the thread still binding it, and then a fix in the mod that serializes the free against the render thread's last use (for example by holding a reference or draining the worker before `0x1e07a0` frees it). The `Present device lost` line names the fault if it removes the device.

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

The Rorona battle-shadow restoration is likewise dual-fingerprinted: all ten hooked shadow functions, the BtlChara-family and battle-state vtables, the shadow-manager global, and the two `ShadowHelperInit` caller return addresses have verified multilingual homologues. The Meruru conversation-cache hooks are dual-fingerprinted the same way (`BalloonBucMode` ctor/dtor in both builds). English-only diagnostics (deep menu statistics and the shadow layer/constructor traces) remain gated to the English builds, whose RVAs are the only ones mapped for them.

Per-game availability and defaults are centralized in a capability matrix (`src/game.cpp`): the running title is detected from the executable name independently of the menu hooks, and each feature resolves through the matrix (unsupported titles are hard-off regardless of configuration) before consulting its environment override and `arland-fix.ini` key. The matrix is the source of truth mirrored by the feature tables in the README.

A lock is eligible only while the verified text renderer is active, the middleware texture reports 512×512 dimensions, and the appropriate queue- or frame-scoped cache lifetime is active. Synthetic locks and first real candidate locks are tracked per thread so only matching synthetic unlocks are suppressed; a different real lock invalidates any frame snapshot for that texture. Installation order keeps partially installed atlas hooks inert. MinHook is used for these four entry points because Totori and Meruru expose atlas unlock through a short relative-jump thunk; every target is still checked against its complete expected prologue before MinHook is invoked.

The D3D11 synchronization hooks and the game-code menu detours are independent layers in one proxy. `ARLAND_MENU_FIX=0` skips the executable detours but leaves the synchronization layer active for a recognized Arland executable. `ARLAND_ATLAS_CACHE=0` disables atlas caching. `ARLAND_FRAME_ATLAS_CACHE=0` restricts Rorona to the queue-scoped lifetime; setting it to `1` opts Totori or Meruru into the frame-scoped path for testing.
