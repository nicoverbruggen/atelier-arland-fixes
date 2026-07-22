# Technical overview

## Provenance and scope

This repository combines established synchronization work with new Arland-specific research. The components should not be conflated:

- Philip Rebohle created `atelier-sync-fix` in 2022. Its central technique—replacing eligible GPU-to-CPU copies with copies through CPU-accessible shadow resources—is the foundation of `src/sync_fix.cpp`. The proxy loading, MinHook-based native D3D11 interception, staging-resource access correction, and direct-source unmap fixes also originate there.
- TellowKrinkle identified that direct game writes through `Map` and `Unmap` must update the shadow and implemented that correction for Atelier Ayesha in commit `98b5c9b`. That implementation stored one global last mapping and uploaded the complete resource on every `Unmap`.
- This project refines the Map/Unmap solution for the Arland workload: mappings are keyed by resource and subresource, references are lifetime-safe, dirty shadows are coalesced, uploads are deferred until the GPU can observe the resource, and deferred contexts cannot perform invalid staging reads. This refinement fixed the corrupted-text case encountered during the investigation while avoiding thousands of redundant atlas uploads.
- TellowKrinkle's rendering fork also established the old-Arland render-target and viewport/scissor correction ported into this project. The released configuration retains that resolution logic while anti-aliasing remains disabled by default; shader replacement, anisotropic filtering, and LOD-bias features are not included.
- Nico, the author of this repository, led the reverse-engineering and runtime investigation of the Arland English menu slowdown. The successful `.PSSG` validation cache and font-atlas read caches are results of that work; they are not features of the original `atelier-sync-fix`.
- MinHook is an independent library by Tsuda Kageyu and contributors, bundled unchanged under `vendor/minhook`.

The current code supports the exact tested Arland DX executables — the English builds and, since v0.4, the multilingual builds used for Japanese and Chinese — and contains the validated D3D11 synchronization and menu-performance fixes described below.

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

The community identified the broad transfer problem years before this project. The original [Rorona menu-loading investigation on Steam](https://steamcommunity.com/app/936160/discussions/0/1742227264210806751/?ctp=2) describes the game rendering individual characters through 512×512 CPU textures, compositing them through video memory, and moving gigabytes during a bad menu frame. The later [Atelier Graphics Tweak discussion](https://steamcommunity.com/app/1152300/discussions/0/3345546664208090238/) introduced an experimental anti-stutter option intended to remove that traffic.

Static analysis of the archived AGT `dinput8_antistutter.dll` shows that it hooks D3D11 texture creation, `Map`, and `Unmap`, recognizes the 512×512 dynamic `B8G8R8A8` font-atlas write path, and prevents selected transfers from executing. This can be extremely fast, but it suppresses uploads without proving that their contents are redundant. The AGT author documented missing text and a reproducible boss-battle crash, and later withdrew the optimization after a user-reported crash.

This repository does not copy that suppression behavior. It hooks the verified game-side text renderer and atlas lock/unlock routines, performs the first required read, and replays an immutable CPU snapshot for later reads within a bounded lifetime. It never suppresses a required atlas write: an unmatched real lock invalidates the affected texture, and Rorona discards every snapshot at the next frame boundary. The `.PSSG` validation cache, queue-scoped atlas cache, and Rorona frame-scoped extension were independently derived from runtime traces during this project's menu-hitch investigation. AGT remains important prior work for identifying the general GPU-transfer problem and demonstrating its potential performance impact.

An additional trace tested whether AGT-style upload avoidance could usefully extend the frame-scoped fix. Across seven repeated Rorona Synthesis openings, each 132–144 ms transition issued about 17 MB of dynamic texture writes, but the D3D11 write-map calls themselves averaged only 0.115 ms. All Map calls averaged 0.246 ms, texture creation averaged 1.60 ms, copies averaged less than 0.001 ms, and the synchronization shadow-flush path was never entered. The approximately 96–101 ms queue drain therefore remained CPU-side construction work, not GPU-transfer latency. Suppressing these required writes would trade correctness for well under one percent of the remaining transition time, so the repository deliberately does not implement it.

Deeper tracing localized part of that residual Synthesis cost to eager construction of recipe UI that is not necessarily visible. One repeatable example was the hidden string `True Dragon Hourglass`: one of its four render operations took about 19–21 ms, while the other three took roughly 0.12 ms each. The slow operation made 63 atlas locks; 60 were served by the frame cache, while the first read of each of the three mutable font atlases accounted for essentially the entire 19–21 ms. This is consistent with Synthesis constructing a complete recipe interface, including unavailable or off-screen entries, rather than limiting work to the currently visible list.

Retaining those three atlas snapshots across frames could avoid this remaining first-read cost after the initial construction, but the atlases are mutable and proving complete invalidation coverage would require a broader correctness investigation. The current frame boundary remains the deliberate safety limit. No additional optimization is applied for this residual cost at present.

## High-resolution rendering

The games accept high render dimensions, but the settings launcher filters its lists through Windows display-mode reporting. DPI virtualization and the current desktop mode can therefore hide a resolution that the game and display can use. The launcher stores independent fullscreen and windowed arrays, so both must be corrected. The 32-bit `msimg32.dll` proxy is loaded by the shared settings launcher, verifies the exact process image, allocator, and code signatures, expands both mode arrays with the launcher's own allocator, and appends missing canonical modes. It guarantees that 1920×1080, 2560×1440, and 3840×2160 remain available in both states; forcing 1920×1080 is specifically useful on Steam Deck and other lower-resolution, high-DPI handhelds that would otherwise hide it, as well as for docked use. The proxy forwards the launchers' two imported image functions, `AlphaBlend` and `TransparentBlt`, to the system MSIMG32 library. MSIMG32 is deliberately used instead of WinMM because native DirectX initialization can dynamically depend on WinMM exports beyond the two functions directly imported by the launcher.

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

The D3D11 layer also contains an optional multisample render-target and resolve path adapted from TellowKrinkle's rendering work. On first launch it creates `arland-fix.ini` with `MSAA=1`, `Width=`, and `Height=` under `[Rendering]` if the file is absent. The path is inactive unless the MSAA value is changed to `2`, `4`, or `8`, or the higher-priority `ARLAND_MSAA` environment variable requests one of those values; absent values and values below two use the original single-sample path. Unsupported requests fall back through lower sample counts. The game continues to own single-sample host resources, while matching multisample color and depth targets are attached as private data and resolved before reads, copies, render-target changes, shader-resource binding, and deferred command-list completion. Four-sample MSAA was visually validated at 2560×1440 in Rorona, Totori, and Meruru, including each game's blurred-dialog rendering. It remains opt-in because behavior can still vary across GPUs and drivers.

## Battle cut-in shadows

Atelier Rorona DX renders attack "cut-ins"—the brief close-up when a character or enemy acts—without ground shadows and with a visibly darker scene. This is original behavior on every platform, not a port regression, and both symptoms trace to a single animated constant.

The battlefield ground is a shadow receiver whose 880-byte material shader gates shadow reception on scene-light intensity. Its vertex shader reads a `diffuse` value at byte 832 of the material constant buffer and computes a gate of `2.5 - 2 * min(diffuse.w, diffuse.x)`; the pixel shader samples the shadow map only when that gate is below one, that is, when the smaller diffuse component exceeds `0.75`. (The pixel shader's own reflection names byte 832 differently, `shadowLPos`, a collision that obscured the mechanism during investigation—the value that matters is the vertex shader's `diffuse`.) A separate sixteen-byte scene-light parameter drives visible floor brightness from the same logical intensity. The cut-in animates that intensity down to roughly `0.7`, which simultaneously darkens the floor and, sitting `0.05` below the `0.75` reception threshold, trips the gate closed. With the gate closed the receiver never samples the shadow map, so every object—acting character, party, and enemies alike—loses its shadow at once while the floor dims. The shadow *casters* are unaffected and continue rendering into the shadow map; only reception is switched off.

The restoration is enabled by default and configured through the `[Battle]` section of `arland-fix.ini`; it addresses the two halves separately during cinematic battle states. `BattleCutInDimming` (default `false`) governs brightness: while it is off, the sixteen-byte scene-light parameter is held at `1.0`, which keeps the floor lit. `BattleCutInShadows` (default `true`) governs reception: it forces the receiver material's `diffuse` back to `1.0` immediately before each shadow-receiving ground draw by issuing a bounded sixteen-byte update over the `[832, 848)` field, which reopens the reception gate. Because the two are independent, the original darker cut-in can be restored while still keeping shadows, or either can be disabled entirely; the `ARLAND_CUTIN_SHADOWS` and `ARLAND_CUTIN_DIMMING` environment variables override the respective keys for a session. The engine's own casters then project real shadows onto the cut-in floor with no injected geometry; the feature composes with the always-on battle-shadow restoration that supplies those casters. The update touches only the diffuse field, never the transform matrices that share the buffer, and the pixel shader does not read that field, so the shared vertex/pixel constant buffer is patched safely; the draw-time path also functions on the game's deferred rendering context. Basic and assist cut-ins keep the real arena floor on screen and gain shadows; solo specials that replace the entire background with a dedicated close-up scene have no real floor and are left unchanged.

Atelier Meruru DX shares the same engine and the same cut-in reception gate, and the restoration applies there too. It differs only in the source of the casters: Meruru registers its battle shadow casters natively (per character, through a build path Rorona lacks), so its ordinary battle shadows already work and no caster restoration is needed—only the cut-in reception gate, which the same value-matched patches reopen. The single Meruru-specific addition is battle-state detection: its `GmStateBtl*` cinematic states were located by RTTI so the patches fire during Meruru cut-ins exactly as they do for Rorona. Atelier Totori DX is not covered.

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

The Rorona battle-shadow restoration is likewise dual-fingerprinted: all ten hooked shadow functions, the BtlChara-family and battle-state vtables, the shadow-manager global, and the two `ShadowHelperInit` caller return addresses have verified multilingual homologues. English-only diagnostics (deep menu statistics, the opt-in text-bitmap cache, and the shadow layer/constructor traces) remain gated to the English builds, whose RVAs are the only ones mapped for them.

A lock is eligible only while the verified text renderer is active, the middleware texture reports 512×512 dimensions, and the appropriate queue- or frame-scoped cache lifetime is active. Synthetic locks and first real candidate locks are tracked per thread so only matching synthetic unlocks are suppressed; a different real lock invalidates any frame snapshot for that texture. Installation order keeps partially installed atlas hooks inert. MinHook is used for these four entry points because Totori and Meruru expose atlas unlock through a short relative-jump thunk; every target is still checked against its complete expected prologue before MinHook is invoked.

The D3D11 synchronization hooks and the game-code menu detours are independent layers in one proxy. `ARLAND_MENU_FIX=0` skips the executable detours but leaves the synchronization layer active for a recognized Arland executable. `ARLAND_ATLAS_CACHE=0` disables atlas caching. `ARLAND_FRAME_ATLAS_CACHE=0` restricts Rorona to the queue-scoped lifetime; setting it to `1` opts Totori or Meruru into the frame-scoped path for testing.
