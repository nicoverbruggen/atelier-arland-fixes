# Technical overview

## Provenance and scope

This repository combines established synchronization work with new Arland-specific research. The components should not be conflated:

- Philip Rebohle created `atelier-sync-fix` in 2022. Its central technique—replacing eligible GPU-to-CPU copies with copies through CPU-accessible shadow resources—is the foundation of `src/sync_fix.cpp`. The proxy loading, MinHook-based native D3D11 interception, staging-resource access correction, and direct-source unmap fixes also originate there.
- TellowKrinkle identified that direct game writes through `Map` and `Unmap` must update the shadow and implemented that correction for Atelier Ayesha in commit `98b5c9b`. That implementation stored one global last mapping and uploaded the complete resource on every `Unmap`.
- This project refines the Map/Unmap solution for the Arland workload: mappings are keyed by resource and subresource, references are lifetime-safe, dirty shadows are coalesced, uploads are deferred until the GPU can observe the resource, and deferred contexts cannot perform invalid staging reads. This refinement fixed the corrupted-text case encountered during the investigation while avoiding thousands of redundant atlas uploads.
- TellowKrinkle's rendering fork also established the old-Arland render-target and viewport/scissor correction ported into this project. The implementation here retains only that resolution logic and excludes the fork's shader replacement, MSAA, anisotropic filtering, and LOD-bias features.
- Nico, the author of this repository, led the reverse-engineering and runtime investigation of the Arland English menu slowdown. The successful `.PSSG` validation cache and the queue-scoped font-atlas read cache are results of that work; they are not features of the original `atelier-sync-fix`.
- MinHook is an independent library by Tsuda Kageyu and contributors, bundled unchanged under `vendor/minhook`.

The current code supports only the exact tested English Arland DX executables and contains the validated D3D11 synchronization and menu-performance fixes described below.

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

The atlas cache is scoped to one invocation of each game's resource-event queue drain. It performs the first real read of each candidate atlas, takes a CPU snapshot, serves later reads from that snapshot, and discards all snapshots when the outermost drain returns. It is additionally restricted to atlas locks made from each game's known text renderer.

| Workload after PSSG caching | Without atlas cache | With atlas cache | Saving |
|---|---:|---:|---:|
| Rorona Status queue drain | 103–117 ms | 67–69 ms | 34–43% |
| Totori 2,331-read menu drain | 119.7–126.3 ms | 94.0–99.8 ms | About 21% |
| Meruru Status queue drain | 82–87 ms | 45–46 ms | 44–48% |

In the Totori measurement, 2,328 of 2,331 candidate reads were served from snapshots. In repeated Meruru Status-class drains, 3,027 of 3,030 reads were served from snapshots. Both therefore reduced the operation to three real atlas reads, matching Rorona.

A process-lifetime snapshot was rejected because the atlases are mutable. Clearing the cache for every rendered string was also rejected because it regressed the same operation to roughly 2.5 seconds.

## High-resolution rendering

The Arland settings launcher contains canonical 1280×720, 1366×768, 1600×900, 1920×1080, and 3840×2160 entries, then filters and merges them with modes reported by DXGI. It omits 2560×1440 and can hide 3840×2160 on a lower-resolution desktop even though the games accept both dimensions in `ArlandDX_Settings.ini`.

The companion 32-bit `winmm.dll` is loaded only by `ArlandDXEnv.exe`. After verifying the launcher filename, PE architecture, image size, complete canonical table, ASLR-adjusted table operands, and mode-builder prologue, it hooks the builder in memory. The hook appends any missing canonical modes, including 2560×1440 and 3840×2160, removes duplicates, and sorts the result. It also expands the launcher's two mode-array allocations from five fallback entries to six. The launcher executable is never edited.

All three tested Steam launchers share the relevant code and table layout, although their complete files have different hashes:

| Game launcher | Original SHA-256 | Resolution table file offset |
|---|---|---:|
| Rorona DX | `167e1c141d0faa03e7baca0034a672f6d8023b446473a6daad6c10b71d5b9667` | `0x1a0888` |
| Totori DX | `c251c7e747e027f75d6e37e4e317cfb599b0db378db9e27ba09043619e02c226` | `0x1a0888` |
| Meruru DX | `fa64db36c92c34429c6c2709ab2126c1ce48f839d9eddd2de1a992564182c732` | `0x1a0888` |

Selecting a larger backbuffer is not sufficient by itself. The old render path creates the main depth target at the requested dimensions but later creates auxiliary render/depth targets and submits viewport/scissor state hard-coded to 1920×1080. It also records rendering through a deferred D3D11 context. Correcting only the immediate context therefore produces genuinely large targets with a 1920×1080 image confined to their upper-left corner.

The D3D11 layer learns the larger main-target size and resizes only later exact-1920×1080 render/depth targets created without initial data. Raster state is tracked independently for the immediate and deferred context paths. When an affected target is bound, exact full-screen 1920×1080 viewport and scissor state is replaced with that target's dimensions before drawing on the same context. This produces direct native 2560×1440 and 3840×2160 rendering; neither mode is a 1080p upscale, and 1440p is not implemented by rendering at 4K and downsampling. Ordinary 1920×1080 and lower-resolution operation remains unchanged.

## Hook boundaries

D3D11 hooks are installed only after the process is recognized as one of the three tested Arland executables. Menu detours additionally verify `.text` size and complete instruction prologues before patching anything.

| Game | Executable | SHA-256 | Path-check RVA | Validation behavior | Atlas-read cache |
|---|---|---|---:|---|---|
| Rorona DX | `A11R_x64_Release_en.exe` | `2afd19db0cef3e3f0888fb62e02c9ca5929264ff5ee8c780af06213642988276` | `0x12cc70` | Metadata plus filename-case enumeration | Yes |
| Totori DX | `A12V_x64_Release_en.exe` | `38c41df799b207786a11c08d6bf83cec8ac10414757f935311549f74474bfd90` | `0x18b140` | Repeated metadata validation | Yes |
| Meruru DX | `A13V_x64_Release_EN.exe` | `d69cad45700457128cc8805ea3cf80dfaea0e155e6dfd2d1123277f4ebd7c19b` | `0x1533c0` | Metadata plus filename-case enumeration | Yes |

The atlas cache verifies four independent entry points per game:

| Game | Queue drain | Text renderer | Atlas lock | Atlas unlock |
|---|---:|---:|---:|---:|
| Rorona | `0x08d4b0` | `0x5613b0` | `0x3eea10` | `0x3eea60` |
| Totori | `0x038a00` | `0x430bf0` | `0x4c2080` | `0x4c20c0` |
| Meruru | `0x0d6210` | `0x5115d0` | `0x3ea7d0` | `0x3ea7f0` |

A lock is eligible only while both the verified text renderer and the outermost verified queue drain are active and the middleware texture reports 512×512 dimensions. Synthetic locks are tracked per thread so only their matching unlock is suppressed. Installation order keeps partially installed atlas hooks inert. MinHook is used for these four entry points because Totori and Meruru expose atlas unlock through a short relative-jump thunk; every target is still checked against its complete expected prologue before MinHook is invoked.

The D3D11 synchronization hooks and the game-code menu detours are independent layers in one proxy. `ARLAND_MENU_FIX=0` skips the executable detours but leaves the synchronization layer active for a recognized Arland executable. `ARLAND_ATLAS_CACHE=0` disables only the atlas cache.
