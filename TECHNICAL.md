# Technical overview

> [!NOTE]
> This technical overview is maintained and kept up-to-date with a large language model to ensure it matches the source code, as such, it wasn't all personally written by the author, who just did some minor editing here. As a player you may find the TL;DR sections interesting.

## Contents

- [Historical background](#historical-background)
- [A note on the vocabulary](#a-note-on-the-vocabulary)
- [How the mod attaches to the game](#how-the-mod-attaches-to-the-game)
- [D3D11 synchronization stalls](#d3d11-synchronization-stalls)
- [Repeated PSSG validation](#repeated-pssg-validation)
- [Repeated font-atlas reads](#repeated-font-atlas-reads)
- [Frame-rate independence](#frame-rate-independence)
- [High-resolution rendering](#high-resolution-rendering)
- [Alternative launcher](#alternative-launcher)
- [Rorona battle shadow restoration](#rorona-battle-shadow-restoration)
- [Battle cut-in shadows](#battle-cut-in-shadows)
- [High-resolution shadow maps](#high-resolution-shadow-maps)
- [Meruru conversation text-render cache](#meruru-conversation-text-render-cache)
- [High-resolution UI text](#high-resolution-ui-text)
- [SMAA anti-aliasing](#smaa-anti-aliasing)
- [Supersampling](#supersampling)
- [Borderless windowed mode](#borderless-windowed-mode)
- [Totori item and save corruption guard](#totori-item-and-save-corruption-guard)
- [Totori asynchronous render-stream lifetime fix](#totori-asynchronous-render-stream-lifetime-fix)
- [Totori shop heap-corruption fix](#totori-shop-heap-corruption-fix)
- [Synthesis card animation rate](#synthesis-card-animation-rate)
- [Field movement and collision](#field-movement-and-collision)
- [Startup logo skip](#startup-logo-skip)
- [Opening-movie skip](#opening-movie-skip)
- [Save data slots view pacing](#save-data-slots-view-pacing)
- [Startup window background](#startup-window-background)
- [Window title on Western locales](#window-title-on-western-locales)
- [Diagnostics](#diagnostics)
- [Runtime memory manipulation](#runtime-memory-manipulation)
- [Hook boundaries](#hook-boundaries)

## Historical background

This repository combines established synchronization work with new Arland-specific research. The components should not be conflated:

- Philip Rebohle created [`atelier-sync-fix`](https://github.com/doitsujin/atelier-sync-fix) in 2022. Its central technique, replacing eligible GPU-to-CPU copies with copies through CPU-accessible shadow resources, is the foundation of `src/sync_fix.cpp`. The proxy loading, MinHook-based native D3D11 interception, staging-resource access correction, and direct-source unmap fixes also originate there.
- TellowKrinkle identified that direct game writes through `Map` and `Unmap` must update the shadow and implemented that correction for Atelier Ayesha in [commit `98b5c9b` of the `atelier-sync-fix` fork](https://github.com/TellowKrinkle/atelier-sync-fix/commit/98b5c9bdb934fa2d74ad17c026bb50598d522cc6). That implementation stored one global last mapping and uploaded the complete resource on every `Unmap`.
- This project refines the Map/Unmap solution for the Arland workload: mappings are keyed by resource and subresource, references are lifetime-safe, dirty shadows are coalesced, uploads are deferred until the GPU can observe the resource, and deferred contexts cannot perform invalid staging reads. This refinement fixed the corrupted-text case encountered during the investigation while avoiding thousands of redundant atlas uploads.
- [TellowKrinkle's rendering fork](https://github.com/TellowKrinkle/atelier-sync-fix) also established the old-Arland render-target and viewport/scissor correction ported into this project. The released configuration retains that resolution logic. The anti-aliasing that is on by default is this project's own SMAA, as is the anisotropic filtering. The fork's shader-replacement and LOD-bias features are not included.

- **Multisampling was carried over from that fork and has since been removed.** It shipped opt-in and off by default through v0.12. Three separate defects surfaced in it: the stale-frame flash fixed in v0.10, a resolve flag written when a target was bound rather than when it was drawn into, and a black backdrop behind Atelier Totori DX's conversation text. The last of these resisted five measured explanations in one evening, each coherent and each disproved by an instrumented run. Multisampling also cannot address what aliases most in these games, which is sub-pixel detail inside textures and alpha-tested edges rather than geometry silhouettes; SMAA handles the first and supersampling the second, and both ship on paths that are validated. The whole twin-and-resolve machinery, its `[Rendering] MSAA` key, its launcher control and its diagnostics were removed in 0.14. The supersampling path covers the same intent at every tier, including on a handheld, where rendering 1280x800 at 1.5x is within reach on 2010-era geometry.
- Nico Verbruggen, the author of this repository, led the reverse-engineering and runtime investigation behind the Arland-specific work in this project: the menu-stutter fix (the `.PSSG` validation cache and font-atlas read caches), the battle and cut-in shadow restoration, and the high-resolution UI font rendering. That work was carried out with the assistance of large language models. None of it is part of the original `atelier-sync-fix`.
- Yuri Hime's [Atelier Graphics Tweak](https://steamcommunity.com/app/1152300/discussions/0/3345546664208090238/), together with the earlier [Rorona community investigation](https://steamcommunity.com/app/936160/discussions/0/1742227264210806751/?ctp=2), identified the broader font-atlas GPU-transfer problem that this project's synchronization and atlas-cache work addresses. AGT's experimental upload-suppression approach is examined here and not used; see "Relationship to Atelier Graphics Tweak" below.
- [MinHook](https://github.com/TsudaKageyu/minhook) is an independent library by Tsuda Kageyu and contributors, bundled unchanged under `vendor/minhook`.
- The high-resolution UI text feature rasterizes glyphs with [stb_truetype](https://github.com/nothings/stb) (Sean Barrett, public domain), vendored unchanged under `vendor/stb`. Its bundled replacement typefaces are chosen per game in code rather than by a setting: National Park SemiBold (Rorona) and Nunito Regular (Totori), both under the SIL Open Font License, and Cosmetica Medium (Meruru), an emboldened MgOpen Cosmetica under the MgOpen licence and renamed as that licence requires. All are embedded in the DLL (generated at build time from the vendored `.ttf` files under `vendor/font/` by `scripts/embed_font.py`, so no large byte arrays are checked in); a `arland-hires-font.ttf` placed beside the DLL overrides them. `[Rendering] Font` selects the mode, not the face.
- [SMAA](https://github.com/iryoku/smaa) is by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro, and Diego Gutierrez (MIT). Its reference shader and the precomputed `AreaTex`/`SearchTex` lookup textures are vendored unchanged under `vendor/smaa`; the mod adds only the runtime integration (compilation, the three-pass pipeline, and the pre-UI injection). The SMAA preset (`ULTRA`) and injection approach follow Yuri Hime's Atelier Graphics Tweak, which shipped the same SMAA for these games.

The current code supports the exact tested Arland DX executables (the English builds and the multilingual builds used for Japanese and Chinese) and contains the validated D3D11 synchronization and menu-performance fixes described below.

## A note on the vocabulary

Several sections use Direct3D 11 terms. In short: the game talks to the graphics card through a *context*, and there are two kinds. The *immediate context* sends work to the card as it is issued; a *deferred context* records work into a *command list* that is played back later on the immediate one. A *resource* is a buffer or a texture the card owns. Reading one back on the CPU normally means copying it into a *staging resource* first and then *mapping* it, which hands your program a pointer to its contents until you *unmap* it. A *render target* is a texture the card draws into, and the *swap chain* holds the ones that end up on screen. A *constant buffer* is a small block of values a shader reads each draw.

## How the mod attaches to the game

Everything in this document rests on getting the mod's own code to run inside a game nobody has the source to, at a moment of its choosing, without changing a single file on disk. This section explains how that is done, because the rest of the document assumes it.

### Getting loaded at all

Windows resolves a program's library dependencies by name, and looks in the program's own folder before the system directory. A library placed next to the game under a name the game already asks for will therefore be loaded instead of the real one. This is what a *proxy DLL* is.

The mod's 64-bit half is a `d3d11.dll` that sits beside the game. Windows loads it, the mod's startup code runs, and every function the game expects to find in the real Direct3D library is exported and forwarded to it. The game is none the wiser: it asked for a graphics library and got one that works. The 32-bit half does the same for the stock settings launcher through `msimg32.dll`.

Being loaded early matters as much as being loaded. The mod's startup code runs before the game's own entry point, which is why a fix like the black startup window can change the window class the game is about to register.

### Running code inside a function you did not write

Forwarding gets the mod into the process. Changing what a *game* function does needs something else, because there is no library boundary to stand on.

The technique is a *detour*, sometimes called an inline hook. The first few instructions of the target function are overwritten with a jump to the mod's own function. When the game calls the target, it lands in mod code instead. The instructions that were overwritten are copied somewhere else first, followed by a jump back to the rest of the original function; that copy is called a *trampoline*, and calling it is how the mod runs the original behaviour before, after, or instead of its own.

The fiddly part is the housekeeping around it. Code pages are not writable by default, so the mod has to make the page writable, patch it, and put the protection back. Processors cache instructions they have already fetched, so the cache has to be flushed or the old bytes may keep executing. And x86 instructions vary in length, so the jump has to displace a whole number of them; you cannot overwrite half an instruction and leave the rest.

That last point is why the mod checks the exact bytes at the target before touching it. It knows which instructions it expects to find, and moving them correctly depends on that being true.

Two detours (the window title and the startup background) are installed while Windows is still running the mod's own load, before the game's first instruction, because they must be in place before the game registers its window class. Installing a detour that early is normal practice for this kind of tool; what makes it sharp is the library doing the installing. MinHook's enable path suspends every other thread in the process and takes a spin lock with no timeout. At that moment the process has exactly one thread, which is why it is safe there, and why the same call would need more care anywhere else.

### Why the byte check is the whole safety story

A function's address is only meaningful for one exact build of one executable. Recompile the game and everything moves. So an address on its own is a guess, and acting on a wrong guess means writing a jump into the middle of an instruction, or into a function that does something else entirely.

The mod therefore never treats an address as sufficient. Every target carries the exact instruction bytes expected at that address, and they are compared before anything is written. A mismatch disables that one feature and leaves the game's original code running. This is the reason the mod fails quietly on an unrecognised build rather than crashing it, and it is why the sections above spend so much time on what identifies a target.

### Two other ways in

Not everything needs a detour.

Direct3D objects are COM objects, which means the game holds a pointer to a table of function pointers. The mod reads a function's address out of that table and then detours the function itself, exactly as it does for game code; the table is never written. Most of the mod's graphics work is done this way. Two consequences follow. The detour catches every Direct3D device and context in the process, not only the game's, because it patches the implementation every one of them shares; the hooks therefore guard on state they own rather than assuming the game is the caller. And the game keeps the real Direct3D objects, so nothing that compares object identity can tell the mod is present. That is a deliberate choice over wrapping the objects in substitutes, which is the other established approach (ReShade wraps; Special K detours as the mod does): a wrapper must reimplement COM identity and unwrap itself for system code, which is a large amount of machinery for a mod that alters a bounded set of methods.

And when the change is simply "do not take this branch", the branch instruction can be overwritten with instructions that do nothing. There is no jump, no trampoline, and no mod code in the path at run time. The waits in front of the save data slots view are removed exactly like this.

### What this cannot do

A detour is only as correct as its understanding of the function it displaces. If a routine is reached through a small jump stub, hooking the stub sees only the callers who went through it. If the mod calls a system function from inside its own hook of that function, it will call itself. If two threads run the same hooked routine, whatever the hook keeps between calls has to cope with that.

Each of these has bitten this project at least once, and where a section describes an unusual precaution, that is generally why.

The specifics of what is hooked, in which builds, and what declines to install, are in [Hook boundaries](#hook-boundaries) and [Runtime memory manipulation](#runtime-memory-manipulation).

## D3D11 synchronization stalls

### TL;DR

The games often send font textures to the graphics card and then immediately ask for the same data back, forcing the game to stop and wait. The mod keeps a CPU-readable copy synchronized with the real texture. That removes the round trip, and both the game and the graphics card still see the latest text.

### Safety

Nothing here changes what the game draws, only how it gets back data it asked for. The shortcut is taken only for copies the mod fully recognizes: an unfamiliar texture format or layout, a busy destination, or a copy made from a background context falls through to the graphics driver's own path untouched. The game also writes to these textures directly, so every one of those writes is tracked and the finished result reaches the graphics card before anything is drawn with it. That is what keeps the text legible. There is one case the mod refuses to touch at all: a font texture the game fills through a queued command list before the mod has a copy of it. Guessing at what is in that texture is how glyphs end up scrambled, so those copies stay on the original path.

### Details

The Arland ports frequently copy GPU resources into CPU-readable staging resources and then map them. A normal D3D11 `CopyResource` followed by `Map` forces the CPU to wait for the GPU. Font-atlas activity makes this especially visible while constructing text-heavy menus.

The original algorithm, retained here, first examines both resources involved in `CopyResource` or `CopySubresourceRegion`. When the destination is immediately CPU-writable and the source is not CPU-readable, it creates a staging shadow for the source with both `D3D11_CPU_ACCESS_READ` and `D3D11_CPU_ACCESS_WRITE`, initializes it from the real resource, and stores it as private data on that resource. Compatible later copies map the destination and the source shadow and use row- and depth-pitch-aware CPU memory copies. Unsupported formats, layouts, contexts, or busy destinations fall back to the real D3D11 copy. A format is unsupported when the mod cannot state its size in bytes per texel: every block-compressed format, whose rows are counted in 4x4 blocks rather than in texels, and any uncompressed format the size table does not list. That test runs before either resource is mapped and before a shadow is created, so an unsupported format never acquires a shadow and never reaches the deferred upload either.

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

All six Arland executables repeatedly validate identical immutable `.PSSG` archive paths (PSSG is the engine's resource archive format) while recursively building UI records. Rorona and Meruru perform a metadata lookup followed by a filename-case directory enumeration; Totori repeatedly performs its corresponding metadata validation.

Building a single Rorona status menu validated the same two archive files over a thousand times. The operating system's file cache does not help much here: the cost is in the repeated path conversion, opens, metadata queries, directory enumeration, Wine/NT transitions and handle teardown, all of which happen whether or not the file itself is cached.

The mod detours the complete game-side validation helper rather than a Windows file API. The original helper runs for the first check. A successful `.PSSG` path is remembered for the process lifetime, and later checks of that exact path return success. Failures are never cached, so resources that appear later can still be discovered. Non-PSSG paths, archive reads, parsing, decompression, and UI ownership remain unchanged.

What the cache holds is path strings, and nothing else: no file handles, no metadata structures, no PSSG contents, no constructed UI graphs. That matches the invariant it relies on. A shipped PSSG archive path cannot become invalid during one run of the game, whereas the parsed UI objects stay mutable and menu-owned, so those are left where they are.

Menus that took most of a second to open now open in about a tenth of that. The gain is around 80 to 90 per cent of the time spent, because almost all of it was the repeated checking rather than anything being read.

## Repeated font-atlas reads

### TL;DR

While building a menu, the games read the same three font textures over and over even when nothing has changed. The mod takes one temporary copy and reuses it for the rest of that safe menu or frame window, discarding it as soon as the texture may have changed.

### Safety

The reused copy is short-lived by design: it lasts one burst of menu building, or in Rorona and Totori a single frame, and the frame boundary throws it away. If the game itself touches one of these textures, that texture's copy is discarded on the spot. A write the game asked for is never skipped. That was the shortcut a previous community tool took, and it produced missing text and a crash. The reuse applies only to the exact 512x512 font textures, and only while the game's verified text renderer is the thing asking.

### Details

Even after the PSSG fix, the games read the same three 512×512 font textures once per text operation. Building a single status menu produced thousands of those reads, and the textures did not change during it. All three games behave the same way here, because it comes from the middleware they share.

The cache performs the first real read of a texture, keeps a copy of it in main memory, and serves later reads from that copy. It only ever applies to 512×512 font textures, and only while the game's own verified text renderer is what is asking.

A copy is only ever made from a read. The text renderer maps each texture twice per glyph: once for writing, to draw the glyph into it, and once for reading, to copy the result out. A write mapping is handed to the caller with its contents undefined, so copying one would capture uninitialised memory and every later read would be served that garbage instead of the font. Copies are therefore taken from reads only, while both kinds of mapping are served from an existing copy: the writer and the reader are working on the same texture, so one copy stands in for the whole draw-then-read-back round trip, and that is where the saving comes from.

### How long a copy lives

This is the part that differs per game, and the reason it differs is that the three games do not build menus the same way.

Meruru does its text work inside one pass over a queue of resource events, so a copy that lives for the length of that pass covers nearly all of it. Anything left over is a handful of reads, and a longer lifetime would be extra exposure for no gain.

Rorona does a second large batch of the same work before that queue pass even begins, so a copy scoped to the pass would miss it. Rorona keeps copies until the next frame is presented, which covers the whole menu-building frame that the player experiences as a freeze.

Totori needs the same wider boundary for a different reason: its field interfaces draw text outside any queue pass at all, where a pass-scoped cache never switches on. The trilogy is not uniform here. Rorona and Meruru build menus through a shared menu framework and open the basket, the container interface, as an object inside it. Totori has no such framework: it uses generated layout scenes and a field-state machine that neither other game has, and its basket is a standalone scene opened from that machine rather than through the menu path.

`ARLAND_FRAME_ATLAS_CACHE` overrides the choice either way, for comparison.

### Why this is safe to do at all

Both lifetimes rest on one thing: a real read that the cache did not serve drops that texture's copy immediately, so a change the game makes can never be hidden behind a stale one. That depends on seeing every unlock.

The hook therefore attaches to the middleware's real unlock routine in all six builds. Totori and Meruru put a small jump thunk in front of that routine, and hooking the thunk, as an earlier version did, saw only the callers that went through it. No bypass was ever found, but this engine dispatches through virtual tables, and looking at callers cannot prove that none exists. Hooking the routine itself covers everything the thunk did and more, and makes the three games behave identically. `ARLAND_ATLAS_RECONCILE` checks that coverage while the game runs, comparing the writes the D3D11 layer sees against the unlocks the hook observes.

Simpler lifetimes were tried first and rejected. Keeping a copy for the life of the process is simply wrong, because the textures do change. Throwing the copy away after every rendered string is correct but slow enough to be worse than not caching at all.

### What it is worth

Menu-building work that used to block for a noticeable fraction of a second gets between a fifth and a half faster, depending on the game and the menu. Rorona's Synthesis transition, the worst case in the trilogy, gets roughly three times faster. Within a cached run the redundant reads collapse to a handful of real ones.

#### Relationship to Atelier Graphics Tweak

The community identified the broad transfer problem years before this project. The original [Rorona menu-loading investigation on Steam](https://steamcommunity.com/app/936160/discussions/0/1742227264210806751/?ctp=2) describes the game rendering individual characters through 512×512 CPU textures, compositing them through video memory, and moving gigabytes during a bad menu frame. Yuri Hime's later [Atelier Graphics Tweak discussion](https://steamcommunity.com/app/1152300/discussions/0/3345546664208090238/) introduced an experimental anti-stutter option intended to remove that traffic.

Static analysis of the archived AGT `dinput8_antistutter.dll` shows that it hooks D3D11 texture creation, `Map`, and `Unmap`, recognizes the 512×512 dynamic `B8G8R8A8` font-atlas write path, and prevents selected transfers from executing. This can be extremely fast, but it suppresses uploads without proving that their contents are redundant. The AGT author documented missing text and a reproducible boss-battle crash, and later withdrew the optimization after a user-reported crash.

This repository does not copy that suppression behavior. It hooks the verified game-side text renderer and atlas lock/unlock routines, performs the first required read, and replays an immutable CPU snapshot for later reads within a bounded lifetime. It never suppresses a required atlas write: an unmatched real lock invalidates the affected texture, and Rorona discards every snapshot at the next frame boundary. The `.PSSG` validation cache, queue-scoped atlas cache, and Rorona frame-scoped extension were independently derived from runtime traces during this project's menu-hitch investigation. AGT remains important prior work for identifying the general GPU-transfer problem and demonstrating its potential performance impact.

A trace tested whether AGT-style upload avoidance could usefully extend the frame-scoped fix. It could not: by the time the cache is working, the write traffic it would suppress is a small part of the cost, and what remains is CPU-side construction that suppressing uploads does not touch.

Part of that remaining Synthesis cost is the game eagerly building recipe interface elements that are not necessarily on screen, each carrying its own text. The cache serves nearly all of the reads involved; the first read of each atlas is what is left.

Retaining those three atlas snapshots across frames could avoid this remaining first-read cost after the initial construction, but the atlases are mutable and proving complete invalidation coverage would require a broader correctness investigation. The current frame boundary remains the deliberate safety limit. No additional optimization is applied for this residual cost at present.

## Frame-rate independence

### TL;DR

Some movement rules were written as fixed amounts per frame, so high refresh rates made characters jitter or eventually stop moving, and made Totori and Meruru's world-map cursor too fast. The mod scales those rules with actual frame time, preserving the original 60 fps feel at higher refresh rates without imposing a frame-rate cap.

### Safety

The correction is a scaling of one constant, clamped so it can never exceed the value the games ship with. At 60 fps and below, everything therefore behaves exactly as it always did. Nothing touches how frames are presented, so there is no frame-rate cap and no tearing introduced. Each half can be switched off on its own for comparison, and the second half refuses to run without the first, since it depends on the ground contact the first one holds. The world-map correction installs only on exact, verified builds of Totori and Meruru; Rorona was measured, needs nothing, and gets nothing.

### Details

The engine is variable-timestep by design: a frame's elapsed time is measured, clamped to a maximum, and threaded through the update tree, which is why the games do not simply run fast at a high refresh rate. What is not frame-rate independent is a set of constants that describe a *distance per frame* rather than a speed, and those are only correct at the 60 fps the games were built around.

The one that matters is in the field-map character's collision resolver. It computes the total distance the character moved this frame and, if that distance is below a fixed threshold, discards the entire frame's movement: the position is reverted to its value on entry, and the ground-snap sweep that would re-seat the character is skipped along with it. At 60 fps this is a reasonable way to ignore numerical noise. As the frame time shrinks, the same fixed distance covers more and more of what the character is actually doing.

The same bug shows up at two scales. Standing still, the only movement is one frame of gravity, which at a high refresh rate never covers the threshold before the grounded grace period expires, so the character loses its footing, falls, lands, and repeats: a visible vertical buzz that begins around 115 fps and is why interaction prompts near the character can flicker. Far higher, the threshold grows to exceed ordinary walking, and the character cannot move at all.

The mod corrects the constant, not the frame rate. Scaling the threshold with frame time turns it back into a speed, so it means the same thing at any refresh rate, and a clamp keeps it from ever exceeding the shipped value, which leaves low frame rates behaving exactly as before. That alone restores movement and holds ground contact, but it does not remove the resting case: gravity is gated on the character's own state bytes rather than on being grounded, so it keeps integrating into the surface and a frame still breaks through periodically.

The second part suppresses that. While the character is at rest, meaning grounded, without horizontal velocity, and with the previous frame's movement reverted, the mod zeroes vertical velocity and pins the grounded grace timer before the update runs. Pinning the timer is what makes zeroing the velocity safe: the grace period can no longer expire, so nothing needs the velocity ramp to re-establish contact. It runs before the engine's update rather than after, because the update refreshes the entry-position copy that the rest test compares against.

Both parts are on by default and each can be turned off for comparison: `ARLAND_FIELD_ENGINE_FIX=0` restores the shipped movement constant and `ARLAND_FIELD_STABILIZER=0` the shipped resting behaviour. The second refuses to run without the first, since the grounded state it holds on can otherwise drop while the character is still settling.

Totori and Meruru have a separate frame-rate coupling on the travel map. Their freely controlled analog cursor reads both stick axes, folds in the four digital directions, rotates and normalizes that direction, then adds a fixed step directly to its authoritative position once per rendered frame. The mover receives no elapsed-time argument, although its immediate driver does. The mod hooks both functions, scopes the driver's real frame delta to the mover call, snapshots the position at `self+0x30`, and rescales the engine's step by `min(dt × 60, 1)`. It writes the corrected position back and republishes it through the mover's own target at `[self+0x28]`, keeping the authoritative and rendered positions coherent.

This preserves the shipped movement exactly at 60 fps and below, and holds distance per second steady above it, so the cursor covers the same ground per second whatever the refresh rate.

Rorona does not share this mover: its stick advances a discrete location selector, whose stepping is already driven by real time rather than by frames, so it needs no correction.

Each supported build is gated on its own addresses and a full prologue check, as everywhere else. `ARLAND_WORLDMAP_FIX=0` disables the correction for comparison.

An earlier approach capped the frame rate instead. It was withdrawn: holding a rate that is not a divisor of the display's refresh meant presenting without waiting for vertical blank, which tears in exclusive fullscreen. Nothing in the current fix touches presentation, so the game presents exactly as it always did.

Other frame-rate couplings exist outside the field map, in effect playback and, in Atelier Meruru DX, in secondary motion such as hair and clothing. Those are not yet addressed.

## High-resolution rendering

### TL;DR

The games claim to support 1440p and 4K, but parts of their renderer stay fixed at 1080p, so a larger window gets a smaller picture inside it. Left alone they also open at 720p. The mod presents at your desktop resolution unless told otherwise, and scales what the renderer left behind so the picture really is the size you chose. Sharper texture filtering lives in the same layer.

### Safety

A blank, half-filled or out-of-range resolution falls back to your desktop resolution, and a size larger than the monitor is clamped to it. Only targets the mod recognises are enlarged: those created empty at exactly 1920×1080, the half-size blur targets, and targets matching the game's own swap-chain size when a render and display split is active. A texture that happens to be one of those sizes for another reason is not caught by accident, and ordinary 1080p play never activates any of it. The sharper texture filtering leaves the sampler types the shadow rendering depends on alone. And the mod no longer patches Koei Tecmo's own settings program at all: resolution comes from the mod's launcher instead, so that program is left exactly as it shipped.

### Details

Resolution is chosen in the mod's own launcher, which writes `DisplayWidth` and `DisplayHeight` under `[Rendering]` in `arland-fix.ini`, and the 64-bit game DLL applies them. Blank display keys are created there by default, alongside blank `RenderWidth`/`RenderHeight`. When both display keys hold valid dimensions, the DLL replaces the swap-chain request, clears the inherited refresh-rate constraint, and resizes the matching first main depth target before applying the ordinary auxiliary-target and raster corrections. Missing, blank, incomplete or out-of-range values fall back to the desktop resolution, because the game's own default is 720p. There is no mode that defers to the game's own choice: the launcher has no way to express one, and a fresh install on a screen larger than 720p is worse off for it.

Being independent of Koei Tecmo's settings editor matters, because that editor filters its resolution lists through Windows display-mode reporting: DPI virtualization and the current desktop mode can hide a resolution the game and display can use, most visibly on Steam Deck and other high-DPI handhelds, and in docked use. Earlier releases worked around that by patching the editor's two mode arrays in memory through the 32-bit `msimg32.dll` proxy. That patch has been removed. The mod's launcher does not consult Windows' mode list at all, so the resolutions are simply there, and the stock editor is now left exactly as shipped.

A larger backbuffer alone does not get you there. The old render path creates the main depth target at the requested dimensions but later creates auxiliary render/depth targets and submits viewport/scissor state hard-coded to 1920×1080. It also records rendering through a deferred D3D11 context. Correcting only the immediate context therefore produces full-size targets with a 1920×1080 image confined to their upper-left corner.

The D3D11 layer learns the larger main-target size and resizes only the targets it recognises: later 1920×1080 render and depth targets created without initial data, the half-size blur targets, and swap-chain-sized targets under a render and display split. Raster state is tracked independently for the immediate and deferred context paths. When an affected target is bound, exact full-screen 1920×1080 viewport and scissor state is replaced with that target's dimensions before drawing on the same context. This produces direct native 2560×1440 and 3840×2160 rendering; neither mode is a 1080p upscale, and 1440p is not implemented by rendering at 4K and downsampling. Ordinary 1920×1080 and lower-resolution operation remains unchanged.

Rorona's blurred-dialog path contains two additional fixed-resolution assumptions. It copies only a 1920×1080 source box into the dialogue snapshot and submits a four-vertex quad whose positions cover `0..1920 × 0..1080`. Merely resizing the render targets therefore leaves the right and bottom of the snapshot black and limits the blurred output to the same upper-left region. When both copy resources match the enlarged main target, the D3D11 layer expands that exact source box to the configured dimensions.

The 48-byte quad is shared with portraits and other cutscene layers, so changing its original contents globally causes those assets to flash briefly in incorrect positions. The fix preserves the game's buffer and attaches a scaled companion instead. The copied snapshot is tagged, that identity is propagated through the three-vertex blur passes, and the companion is bound only for the final four-vertex draw that composites a processed blur result. The original buffer is restored immediately afterward. Other copy regions, vertex buffers, and cutscene draws are left unchanged.

Totori and Meruru share the blur/capture engine classes and post-blur shaders but did not issue Rorona's exact fixed-size snapshot copy during validated 2560×1440 dialogue scenes. Their ordinary resized blur targets filled the output correctly, so the exact runtime predicates leave the Rorona-specific correction inert in those games.

On first launch, if the file is absent, the D3D11 layer seeds `arland-fix.ini` with blank `DisplayWidth`/`DisplayHeight`/`RenderWidth`/`RenderHeight`, `ShadowMultiplier=2` and `AnisotropicFiltering=16` under `[Rendering]`.

Optional anisotropic filtering is a separate, cheaper texture-quality knob. The games create their texture samplers with plain linear filtering, so obliquely-viewed surfaces (floors, walls, receding ground) blur. When `AnisotropicFiltering` (or `ARLAND_ANISO`) requests `2`, `4`, `8`, or `16`, the D3D11 layer hooks sampler creation and, for samplers using a basic point/linear filter, substitutes anisotropic filtering at the requested maximum-anisotropy level before the sampler is created. Comparison, minimum, and maximum filters (shadow PCF and similar) are recognized by their filter enum and left unchanged, and the hook is installed only when the feature is enabled. Because the upgrade happens once at sampler creation, there is no per-draw or per-frame cost. Defaults to 16x: on any GPU able to run these ports 8x and 16x are not measurably different, so the lower default preserved no trade-off, only a smaller improvement. For that reason the launcher no longer offers a control for it, and normalises the key to `16` when it saves; the ini value is still read, so a lower setting can be forced by hand for troubleshooting.

## Alternative launcher

### TL;DR

The mod includes a replacement launcher that puts the game's settings and every mod option in one window. Steam opens it automatically in place of the stock launcher while preserving the overlay, Steam Input, and access to the original tools; if the replacement is missing or cannot start, the stock launcher still works.

### Safety

If the mod's launcher is not installed beside the stock one, nothing is armed and the original launcher comes up exactly as before, so a partial install cannot leave the game unstartable. If our launcher fails to start, the original bytes are put back and the stock launcher runs as though nothing had happened. The substitution verifies the process and the exact instructions it replaces before changing anything, and only ever in memory. The process Steam started stays open behind ours instead of being killed off, which keeps the overlay, playtime tracking and Steam Input attached. The buttons that open Koei Tecmo's original tools switch the redirect off first, so they always open the real thing.

### Details

`arland-fix-launcher.exe` is a 64-bit settings window that puts every option in `arland-fix.ini`, plus the game's own resolution, window mode, language and character outlines, in one place and starts the game from it. The settings it takes from the game rather than from the mod are read from and written straight back to `ArlandDX_Settings.ini`, under the keys the game itself uses (`[Graphics] Outline` for the outlines); the mod never reads them, so unchecking outlines is the game's own switch rather than anything this project does at runtime. Both of Koei Tecmo's original front-ends stay reachable from it. It writes the mod's settings when the game is started, so there is no separate save step; close the window with unsaved changes and it asks first.

Steam runs `ArlandDXLauncher.exe`, not the game, so the launcher has to insert itself there. A 32-bit `msimg32.dll` proxy does it: when `arland-fix-launcher.exe` is present beside the stock launcher, the proxy points the executable's entry point at its own routine, which starts our launcher instead.

The proxy is loaded because both of Koei Tecmo's 32-bit front-ends import `AlphaBlend` and `TransparentBlt` from MSIMG32, and it forwards both to the system MSIMG32 library. MSIMG32 is used rather than WinMM because native DirectX initialization can dynamically depend on WinMM exports beyond the two functions the front-ends import directly. The three games' outer `ArlandDXLauncher.exe` files are structurally identical, sharing `.text` SHA-256 `58ba7aee62d924d35ca160829766bc8775125475894473bcbadf92d962fcc522`. The redirect is the proxy's only behaviour: `ArlandDXEnv.exe`, the stock settings editor, loads the same DLL and is forwarded to and nothing else. The stock launcher never puts a window on screen, so a plain drop-in install replaces it with no extra steps, and with no `arland-fix-launcher.exe` present the redirect is never armed and the original launcher comes up exactly as before.

The redirect depends on these properties:

- It must not run in `DllMain`. The proxy is a static import of the launcher, so its process attach runs before the executable's entry point and before other injected code has finished setting itself up, Steam's overlay among it. The overlay hooks process creation in order to follow the game into child processes; starting our launcher from `DllMain` produced a child Steam knew nothing about, with no overlay, no frame-rate counter and no Steam Input, which is what makes a controller work when Steam is handling it. The redirect is therefore armed during process attach but executed at the entry point, by which time the process is fully assembled.
- The stock launcher process must stay alive while ours is open rather than being terminated once its replacement starts. It is the process Steam launched and is counting, and the game is started from our launcher underneath it. Waiting is harmless, since that process has no window and no work of its own once its entry point belongs to us.

`[Launcher] SkipLauncher` in `arland-fix.ini` changes only where the redirect goes: the proxy resolves the game executable instead of `arland-fix-launcher.exe` and starts that, so neither front-end appears. Everything else about the substitution is unchanged, which is the point. The game is created by the same process at the same moment our launcher would have been, inherits the same environment, and this process still waits on it, so Steam sees the session it would otherwise have seen. Which executable runs is decided the way both Koei Tecmo's launcher and ours decide it, from `[Lang] Language` in `ArlandDX_Settings.ini`: the multilingual build for Japanese, Simplified Chinese and Traditional Chinese, the English build otherwise, falling back to whichever of the two is installed. If neither is found the setting is ignored and the stock launcher comes up. The proxy reads this key wide and never seeds it, since creating `arland-fix.ini` from the launcher process would suppress the DLL's own first-use seeding of every other default.

The redirect installs a five-byte patch over the verified entry-point prologue, keeping the original bytes so they can be put back: if our launcher fails to start, the original prologue is restored and the stock launcher runs as though nothing had happened. `ARLAND_NO_REDIRECT` stands the redirect down, which is how our own launcher opens the original launcher and settings editor from its own buttons without them bouncing straight back to it. Because the proxy is 32-bit and our launcher is 64-bit, the child is created across that boundary and inherits the environment either way, which is how the Steam variables reach the game.

The proxy's single patch is gated on the verified process image and the exact entry-point bytes it replaces, and leaves the image untouched if anything does not match.

## Rorona battle shadow restoration

### TL;DR

Rorona's battle renderer can draw proper ground shadows, but the port never tells it that the characters and enemies should cast them. The mod registers the battlers with the game's existing shadow system and carefully restores the field-map state after combat. The shadows are the engine's own; the mod draws none of them.

### Safety

The mod does not draw any shadows of its own. It tells the engine's existing shadow system about characters the port never registered, through the same routine the field map already uses, so everything visible is rendered by the game. The objects it registers are identified by their exact class fingerprints first; anything that does not match is skipped. Because the engine raises no event when a battle ends, a per-frame watchdog restores the field map's original shadow state once the battle objects are gone; it only arms after it has actually seen a live battle, so a slow battle intro cannot trip it early, and it exists precisely so the mod stops touching freed battle objects.

### Details

Atelier Rorona DX renders ordinary battles without any character or enemy ground shadows. The engine's shadow pipeline is present and functional (the field map uses it), but the port's battle scene setup never registers the battle actors as shadow casters. Atelier Meruru DX, by contrast, registers its battle casters natively, which is why its ordinary battle shadows work unmodified.

The restoration hooks the engine's shadow-helper initialization and observes its two static call sites: the battle scene-setup path and the field-map re-entry path. When the battle path runs, the mod records the battle game-mode object and the battle's shadow helper, locates the party's character vector inside the game-mode (verified through the BtlChara-family vtables), and registers each character's render node as a shadow caster through the engine's own `ShadowCharacterBuild` routine, the same call the field map uses. The battle helper is then published to the engine's global active-helper slot so the shadow traversal walks the battle casters; the displaced field helper is remembered. Enemies are registered through the same container discovery. The engine still does the rendering, so these shadows go through the game's own pipeline end to end. What the mod adds is bookkeeping, not draw calls.

Alongside registration, the mod tracks the battle state machine by recognizing the `GmStateBtl`-family state vtables (Enter, SelectCommand, WaitAction, the Result states, and so on). This state tracking is what the cut-in features key on, and it has to disengage reliably.

Returning from battle to an already-loaded field map does not re-run the field's shadow-helper initialization, so the field's own initialization cannot signal the end of a battle. The battle game mode's construction and destruction are hooked instead, which are exact once-per-battle edges, and a per-frame watchdog is kept behind them as a fallback. `ARLAND_BATTLE_MODE_GATE` switches the mode edges off to compare against the watchdog alone. It checks whether the battle game-mode still looks alive, meaning its party vector still holds objects with BtlChara vtables. Once a game-mode that was previously seen alive stops looking alive for twenty consecutive frames, the battle is over, so the saved field helper is restored to the global slot and all battle tracking is cleared.

The watchdog only arms once the game-mode has been seen alive, so a slow battle intro, where the party is not yet spawned, cannot trip it early. Without the watchdog, the tracking kept scanning the freed battle objects every frame after combat and degraded field performance.

## Battle cut-in shadows

### TL;DR

During battle close-ups, the games darken the arena and stop the ground from receiving shadows. The mod can keep the scene bright and its real shadows visible, while following the game's hide-and-show choreography so invisible or repositioned characters do not leave stray shadows behind.

### Safety

Both halves are on by default, and either can be turned off. What they change is one small, specific value the engine sends to the graphics card each frame: a bounded sixteen-byte update in a known position, leaving everything else in that data, including the transform matrices sharing the buffer, untouched. The hooks are checked against each build's exact instruction bytes, and the structure offsets they need are verified byte by byte from that build's own code, so a wrong offset leaves the feature uninstalled instead of writing to the wrong field. If those hooks cannot install, the mod falls back to a mode that waits for the scene to settle before holding anything, which preserves the game's original cover at exactly the moments where stray shadows could otherwise appear.

### Details

Atelier Rorona DX renders attack "cut-ins" (the brief close-up when a character or enemy acts) without ground shadows and with a visibly darker scene. That is how the game behaves on every platform, not a port regression, and both symptoms trace back to a single animated constant.

The battlefield ground is a shadow receiver, and its material gates shadow reception on scene-light intensity. The vertex shader reads a brightness value out of the material's constant buffer and computes a gate of `2.5 - 2 * min(diffuse.w, diffuse.x)`. The pixel shader samples the shadow map only when that gate is below one, that is, when the smaller diffuse component exceeds `0.75`. (The pixel shader's own reflection names byte 832 differently, `shadowLPos`, a collision that makes the mechanism hard to see; the value that matters is the vertex shader's `diffuse`.)

A separate sixteen-byte scene-light parameter drives visible floor brightness from the same logical intensity. The cut-in animates that intensity down to about `0.7`. That simultaneously darkens the floor and, sitting `0.05` below the `0.75` reception threshold, trips the gate closed. With the gate closed the receiver never samples the shadow map, so every object (acting character, party, and enemies alike) loses its shadow at once while the floor dims. The shadow casters are unaffected and keep rendering into the shadow map; only reception is switched off.

Holding the gate open is not enough on its own. The engine runs its own caster cover-up during the cut-in, and the closing gate had been hiding the seams in it.

When a cut-in begins, the non-focus battlers are hidden and repositioned ("juggled"), but their per-node caster flags (`PNode::setCastShadow`) are cleared only about a quarter second later, deferred with the visual cross-fade, then restored instantly at cut-in exit, before positions finish restoring. Vanilla never shows stale shadows for these characters because the fading scene light crosses the 0.75 reception threshold during exactly those windows, so the closed gate covers the stale casters. Holding the gate open naively therefore exposed stray floor shadows for characters that were invisible or off-position.

The fix gets ahead of the engine instead of waiting for it. Hooks on the tactical-scene `hideAll`/`showAll` wrappers, the functions the cut-in machinery calls to fade the non-focus battlers out and back, clear the registered casters' flags the moment the hide starts, re-clear them when the show's instant restore fires, and restore them once the battle leaves the cinematic states. The restore waits on a condition, not on a clock. A wall-clock delay is a guess at when the juggle has settled, and guessing short hands shadows back to actors that are still hidden or mid-fade, which puts a shadow under an invisible character. A 0.3 s delay remains as a floor, after which the condition is re-tested every 100 ms until the cinematic ends and everyone is on screen again. Visible cut-in participants keep their shadows, because the event system's own immediate show paths re-set their flags.

A third window sits inside the cut-in itself. The event choreography hides individual non-focus battlers through a deferred per-actor alpha fade of about 0.25 s, and the engine only drops a faded-out model from the shadow map when that fade expires. (Its fade-end handler recursively hides the whole model subtree, shadow nodes included.) During the fade the model is alpha-invisible but still casts, so with the gate held open its full-strength shadow lingers on the ground. Rather than edit the scene graph directly, the mod front-runs that expiry: it hooks the leaf that arms a battler's fade and, when a hide latches during a cinematic state, zeroes the model's fade timer. The engine's next visibility tick then performs the complete native hide, the recursive subtree clear plus the state flags its cancel and re-show path depends on, so there are no manual node writes and the focus actor (which the hide enumerator never arms) is untouched. The only cost is that the hidden battler pops out instead of fading over a quarter second, off-camera and negligible.

That leaf is a thirty-byte function with no `.pdata` entry, which is why it stayed unfound on Totori for so long. Every search for it walked the function table, and a leaf without unwind data does not appear there. Found instead by searching for the instruction bytes of its opening compare, it exists in all six executables and is byte-identical apart from the two Model field offsets it encodes as `disp32` operands. Those offsets differ by engine generation and not by a single constant: Rorona and Meruru put the current-visibility byte at `+0x80`, the fade-pending byte at `+0x8f` and the duration float at `+0x90`, while Totori uses `+0x90`, `+0xa2` and `+0xa4` — the flag bytes shifted by `0x13` and the float by `0x14`, because Totori's structure carries alignment padding the earlier one does not. Each build's values are read from its own setter and constructor. The verification window is built from the offsets in the per-game table, not hardcoded, so a wrong offset fails the byte match and leaves the hook uninstalled instead of writing to the wrong field.

An earlier approach cleared the caster registry's visibility flags directly and was replaced by this one. The registry entries turned out to be model locator roots rather than the drawable shadow leaves, so clearing them never reached the shadow map at all. The leaves hold their own visibility flag, which only the engine's subtree hide clears.

With the stale casters cleared from the first frame, the brightness and reception hold can engage immediately and the cut-in never visibly dims. The `hideAll` prologue is byte-identical across all six executables; `showAll` differs per engine generation and is verified per build. If these hooks fail to install on some build, the hold falls back to a transition-aware mode: it engages only once the observed dim value has been bit-identical for at least 60 ms (after the entry fade has bottomed out, by which time the engine has cleared the juggled casters), eases up over a further 120 ms, and never engages during the exit fade. That preserves the vanilla cover at the cost of a brief visible dim.

The restoration addresses the two halves separately during cinematic battle states, and both are configured through the `[Battle]` section of `arland-fix.ini`. Both ship off on all three games: the capability matrix in `src/game.cpp` marks them opt-in, so neither restoration runs until it is asked for, and both keys are seeded lazily from those per-game defaults instead of being written eagerly on first launch. The launcher offers the pair as one "Attack cut-ins" list with two entries, Classic (the shipped state, the close-up as the game renders it) and Enhanced (both halves restored). The keys stay independent in the file, so half the restoration can still be asked for by hand; the launcher opens such a file on Enhanced and normalises it on the next save.

`BattleCutInDimming` governs brightness. It defaults to `true`, which is the game's own darkening; setting it `false` holds the sixteen-byte scene-light parameter at `1.0` and keeps the floor lit. `BattleCutInShadows` governs reception. It defaults to `false`; setting it `true` forces the receiver material's `diffuse` back to `1.0` immediately before each shadow-receiving ground draw through a bounded sixteen-byte update over the `[832, 848)` field, which reopens the reception gate. The two keys store one choice, and the launcher writes them together. They are not independent in practice: on Rorona and Meruru either half can be set on its own by hand, but on Totori `BattleCutInShadows` alone does nothing at all, because no 880-byte receiver buffer exists in any of its shaders and both gate paths are gated on that size. Totori's reception is carried by the same `(304, 16)` field the brightness hold writes, so there the dim hold is the reception hold and the shadows key has nothing of its own to switch. The `ARLAND_CUTIN_SHADOWS` and `ARLAND_CUTIN_DIMMING` environment variables override the respective keys for a session. (Note that the dimming key is worded as the inverse of the action it controls: it asks whether the cut-in *may* dim, so `false` is what engages the hold.)

One environment-only A/B switch remains for this subsystem. `ARLAND_CUTIN_ACTOR_CLEAR=0` disables the per-actor front-run in all three games; with it set, a battler hidden mid-cut-in fades over its full quarter second and its shadow leaves only at the end.

This subsystem writes two log lines, and they do not mean the same thing. `Deferred-hide arm hook installed=1` says only that the detour attached; a subsystem can install and then do nothing, which is exactly the failure a bare install line would hide. `Deferred-hide arm force-expired a caster fade (first hit)` is emitted the first time the front-run actually acts, and is the line that confirms the feature works.

Both halves are opt-in. They were on by default for a while, after the stray-shadow glitch described above was fixed, and were put back to opt-in because they change how the close-ups look rather than repair them, and because the brightness hold is still being played through: it selects the scene-light parameter by buffer size and byte offset, and battle arenas ship their own shaders with their own layouts, so a material the table does not list stays dark while the rest of the scene brightens. Rorona's restored ordinary-battle shadows are separate and are not a setting at all, since the engine plainly means to draw them there.

The patch is a small one. It touches only the diffuse field, never the transform matrices that share the buffer, and the pixel shader does not read that field, so the shared vertex and pixel constant buffer is patched safely; the draw-time path also works on the game's deferred rendering context. The engine's own casters then project real shadows onto the cut-in floor with no injected geometry, and the feature composes with the battle-shadow restoration, which is unconditional, that supplies those casters. Basic and assist cut-ins keep the real arena floor on screen and gain shadows; solo specials that replace the entire background with a dedicated close-up scene have no real floor and are left unchanged.

Atelier Meruru DX shares the same engine and the same cut-in reception gate, and the restoration applies there too. It differs only in the source of the casters: Meruru registers its battle shadow casters natively (per character, through a build path Rorona lacks), so its ordinary battle shadows already work and no caster restoration is needed, only the cut-in reception gate, which the same value-matched patches reopen. The single Meruru-specific addition is battle-state detection: its `GmStateBtl*` cinematic states were located by the compiler's run-time type information, the tables C++ emits so a program can identify an object's class so the patches fire during Meruru cut-ins exactly as they do for Rorona.

Atelier Totori DX received the same battle-state treatment in both builds after a static-plus-runtime investigation established that its fighting shadows are natively healthy: like Meruru, its battle characters register as casters through the game's own build path (confirmed by a runtime probe: configuration byte set, helper context live before the character constructors, caster registry filling). Totori is the structural outlier: its battle shadow helper is embedded at a different game-mode offset, it has no global active-helper slot at all (field and battle each render through their own helper, so the helper-publish machinery is inert there by design), its state machine lacks `SelectDefence`, and its result chain uses different state names (`Result`, `AddPay`, `DropItem`, `LvUp`), which the cinematic-state list carries. These per-game differences are encoded in the battle address pack (`BattleBuildAddrs`), including the helper embed offset and zeroed entries for the structures Totori does not have. The multilingual address pack and state table were RTTI-located and homologue-matched from the English build, then confirmed in-game through the complete battle-state sequence and the actor-clear path's first real operation.

Totori's cut-in mechanics also differ at the shader level, though not in the way the file names suggest. All three games ship D3D11 shader packs: every program in Rorona's `commonShader.PSSG` and in Totori's and Meruru's `commonShaderWin.PSSG` is a shader-model 5 DXBC container built by the same HLSL compiler, and all three use `GatherCmp` for the shadow filter. The `CgRsxBinary` node label wrapped around each blob is a leftover from the PS3 tooling with no RSX code behind it. Rorona and Meruru ship the same pack, 113 of their 114 programs byte-identical. What separates Totori is where material uniforms live: Rorona and Meruru put them in large shared `$Globals` blocks, the fieldmap receiver's being 880 bytes, while every `$Globals` in Totori's pack is the 16-byte alpha reference and each shader carries its own `$Params` instead. That has two consequences.

First, Totori's dedicated battle-ground receiver has no shadow-reception gate at all. Its PCF sampling is unconditional and `diffuse` only tints the final color, so the 880-byte gate-hold used by Rorona and Meruru does not apply. Runtime tracing then established that Totori's actual battle arenas use the fieldmap shader family described below, whose vertex shaders do gate reception on their own `diffuse` fields. Totori's complete fix therefore combines its per-actor caster protection with the fieldmap reception hold rather than using an 880-byte receiver buffer.

Second, the cut-in dim is the same `BtlField` fade to (0.7, 0.7, 0.7, 1.0) as Rorona's, but Totori spreads `diffuse` over many differently sized `$Params` blocks instead of the single 16-byte one Rorona and Meruru use for it. Totori's battle arenas render with the fieldmap shader family rather than a dedicated battle-ground shader. The dim flows through the fieldmap layouts, with `diffuse` at (ByteWidth 304, offset 16), (48, 32), (80, 0), (144, 0), and (160, 16), alongside the battle and character layouts (32, 0) and (16, 0), the `chara_*` vertex-shader families at (224, 208) and (13024, 13008), and the toon vertex-shader families at (12960, 12944), (160, 144), and (96, 80).

The dim-hold consults this per-game field table with one value predicate everywhere: only a uniform (s, s, s, ~1) with s in (0.5, 0.98) is rewritten. Two 304-byte layouts exist and size alone cannot separate them. The common-pack receiver holds `diffuse` at 16 and a row of `PSSGLightModelViewProjTex` at 272; the shaders a battle arena ships with itself hold `WorldITXf` at 0 and `diffuse` at 272. Their contents do separate them, so offset 272 is patched only on a buffer whose offset 16 does not already carry the fade, which leaves the matrix row untouched in the one layout where it exists. Covering the arena materials is what stops water, cloud and sky domes staying dark through a cut-in while the ground around them brightens. The fog shadow-receiving vertex shaders at (304, 16) gate shadow reception on the same `diffuse`, closing below 0.85 in `fieldmap_shadow_fog_vert` and below 0.75 in its HDR variant, so on Totori the dim-hold doubles as the reception hold. The other fieldmap entries carry no gate: (144, 0) is brightness only and its pixel shader filters unconditionally, and (160, 16) touches no shadow map at all. It carries the same settle gating as the Rorona and Meruru gate-hold, so holding these fields also restores cut-in floor shadows, and the settle delay preserves the vanilla stale-caster cover at the transitions.

## High-resolution shadow maps

### TL;DR

The games draw every shadow into a relatively small 1024×1024 texture, which makes shadow edges look blocky. The mod creates larger companion textures and quietly redirects the existing shadow pipeline through them. Definition improves, while the engine keeps its own textures and its own shadow renderer.

### Safety

The engine's own shadow textures are never resized or replaced, so every size the engine derives from them stays true. The mod creates a larger companion texture alongside each one and ties it to the original's lifetime, so it is released when the game releases its own. Anything ambiguous at creation time, such as initial data, staging use or mip levels, declines the companion and keeps that shadow map entirely on the original path. Each of the four redirection points does nothing at all when no companion exists, and setting `ShadowMultiplier=1` stands the whole mechanism down and restores the game's own 1024 map byte for byte.

### Details

The games render all shadows into two 1024×1024 `R24G8_TYPELESS` depth maps, a caster map (A) and a receiver map (B) with a per-frame A→B transfer between them, so shadow edges are visibly blocky, most noticeably in Meruru. The `ShadowMultiplier` option renders shadows at 2048, 4096, or 8192 instead. It defaults to `2`, because one 1024 map spread over a whole scene is blocky at every resolution this mod renders at, and 2048 costs little video memory.

The engine's own maps are never resized. It takes viewport sizes, copy extents and memory assumptions straight from its texture metadata, so resizing one in place would quietly invalidate all three. Instead, each eligible 1024×1024 shadow-map creation also creates a separate mod-owned enlarged "twin" texture, attached to the engine texture as private data so the two share a lifetime, and when the engine releases its map, the twin is released with it. Anything ambiguous at creation (initial data, staging or CPU access, mips, arrays, sample count, misc flags) declines the twin and keeps that map on the vanilla path.

The shadow pipeline is then redirected onto the twins at four points, each inert when no twin exists:

- the engine's shadow-map clear is mirrored onto the twin;
- depth-only caster binds of a shadow-map DSV are redirected to a lazily created twin DSV (binds that pair the shadow map with a color target fail safe to the vanilla pass);
- the engine's A→B shadow-map transfer is mirrored as an equal-sized copy between the twins;
- the receiver's shadow-map SRV bind is substituted with the twin's SRV, with a pointer-keyed negative cache so the hot bind path stays cheap.

The engine sizes everything from its own 1024 metadata, so two further size assumptions need correcting. Exact 1024×1024 viewport and scissor state is rewritten to the twin's dimensions during the redirected caster pass. And the receiver material's percentage-closer filtering tap (the lookup that softens a shadow edge) size, which encodes the shadow texel size in the same 880-byte constant buffer the cut-in fix patches, is rescaled so filtering matches the enlarged map. At `ShadowMultiplier=1`, which is no longer the default but remains the way to ask for the vanilla behaviour, none of this machinery activates and the shadow pipeline is untouched.

## Meruru conversation text-render cache

### TL;DR

Meruru was rebuilding unchanged conversation text every frame while an animated portrait was on screen, causing a large slowdown for the whole conversation. The mod keeps the finished text bitmap for exactly as long as the conversation balloon exists, so those repeated renders become inexpensive memory copies.

### Safety

The cache is alive for exactly as long as a conversation balloon is on screen, and the last balloon closing clears it, so nothing can go stale between conversations. Its size is bounded: when a long typewriter reveal fills it, it clears and rebuilds itself, so it cannot grow without limit. If a cached result would not fit the buffer the game supplied, the mod allocates one that does through the game's own allocator, frees the old one, and installs it. Measured in play, that is what happens on every replay: the game frees the pixel buffer once its consumer has built the texture, so by the time a replay runs there is no buffer to write into at all. The saving the cache exists for is the glyph and atlas work, not the allocation. The hooks verify each build's exact function bytes before installing, including the balloon class identity, so they cannot attach to a similar-looking routine.

### Details

Atelier Meruru DX's field-map conversations with animated bust-up portraits collapsed the framerate on the English executable for the duration of the conversation. The cost was not the portraits: the conversation balloon's per-frame callback pump re-entered the executable's text-render path (the same CPU-side glyph and atlas work that makes menu construction slow) every frame, for text that had not changed. Menus pay that cost once per rebuild; the balloon paid it continuously.

The mod already contained a text-bitmap replay cache built for menu diagnosis. `cachedRenderText` keys on the renderer, font, atlas, style, and the exact string, and replays the previously rendered output bitmap into the caller's buffer instead of re-rendering. Its lifetime, though, was scoped to a single queue drain.

The fix gives that cache a cross-frame scope bounded by the conversation. Hooks on the `BalloonBucMode` constructor and destructor count live conversation balloons, and while any balloon is alive the cache activates and its per-drain clears are suspended, so a string that is identical from frame to frame costs one memory copy. The destructor of the last balloon clears the cache.

The transition needed handling at both edges. The typewriter reveal inserts one entry per partial string, so the cache is bounded and an overflow clears and rebuilds it. And a replay whose target buffer is missing or too small gets a correctly sized one from the game's own allocator, which is the ordinary case rather than an edge: the buffer is gone by the time the replay runs. Capacity is never inferred from the output object, because it stops describing itself once the high-resolution text substitution runs — the consumer restores the pre-substitution dimensions and leaves the enlarged buffer in place, so width times height reads as a quarter of the allocation. The substitution records the size of what it installed, and the replay reads that record, which is rebuilt on every real render and so can never outlive one. The hooks verify the constructor and destructor prologues per build (the destructor check includes the RIP-relative load of the `BalloonBucMode` vtable, pinning it to the right class) and are installed for both the English and multilingual executables. `ARLAND_BUC_TEXT_CACHE=0` leaves them uninstalled, which is the A/B for the whole conversation scope. The underlying replay cache has its own switch, `ARLAND_TEXT_BITMAP_CACHE`, but a live balloon activates it regardless of that value, so turning the scope off is the switch that restores vanilla behaviour.

## High-resolution UI text

### TL;DR

The games use a small pre-rendered bitmap font, so their text looks soft and pixelated at modern resolutions. The mod replaces English text with crisp scalable fonts, or sharpens the original glyphs when needed, while preserving the game's layout, sizing, and controller icons.

### Safety

The replacement bitmap comes from the game's own text allocator, so the game frees it normally. The temporarily doubled dimensions are put back once the picture has reached the graphics card, so the text keeps its original size on screen. If the replacement font cannot draw a character, most often one of the controller-button icons, the whole string falls back to a sharpened version of the game's own glyphs, so nothing is ever dropped or shown as an empty box. The substitution is wired only to the English builds; on the Japanese and Chinese builds the hooks stay unresolved and every mode is a safe no-op.

### Details

All UI text in these games comes from a pre-baked bitmap font. Koei Tecmo's G1N atlases store every glyph as a fixed 32×48 image that the engine blits 1:1, with no scalable rasterizer, so text is soft and pixelated at 1440p or 4K. This feature re-renders that text at full resolution while preserving the engine's exact layout. `[Rendering] Font` chooses the mode: `replaced` (the default), `upscaled`, or `original` (or `off`; the untouched bitmap).

Every mode works the same way at the top level: let the engine render a string normally, then swap the result. When the engine finishes a string it leaves an "output object" at `renderer+0x1a0`. That struct holds the power-of-two bitmap width and height, a pointer to its 8-bit alpha pixels, four normalized metrics (used-width, used-height, and line-height, each a fraction of the pow2 size), and the line count. The mod reads it, builds a higher-resolution bitmap of its own, and writes the new pixel pointer and doubled (`kScale = 2`) dimensions back into it.

One detail decides whether the swap works at all. A text quad's on-screen size is computed as `fraction × pow2-dimension`, so doubling the bitmap's dimensions would double the text on screen. Two things prevent that. First, the replacement bitmap is allocated through the engine's own text-buffer allocator, so the engine still frees it correctly. Second, the doubled dimensions are restored to their originals once the string's consumer has built its GPU texture, through a hook on that consumer. The glyphs end up twice as dense while the quad keeps its original size. A per-string result cache, keyed on the string and font state, reduces a repeat render to a memory copy and keeps the work off the hot menu path.

`upscaled` keeps the engine's own glyphs and only raises their resolution. The baked bitmap is filtered up 2×, by default a signed-distance-field style steepening of the glyph's coverage, which sharpens the edge without inventing detail. Bilinear and Lanczos are also available, and those two are followed by an unsharp pass. Nothing about the layout changes, so multi-line text, alignment, and the game's custom icon glyphs all survive untouched.

`replaced` re-renders each string from a per-game bundled scalable font (National Park SemiBold for Rorona, Nunito for Totori, Cosmetica Medium for Meruru; a compile-time matrix pairs each with a size multiplier and baseline nudge tuned to its metrics), rasterized with stb_truetype through a glyph-atlas cache so each glyph is drawn once and then reused. Matching the engine's layout meant reverse-engineering the text renderer. Three points carry most of the weight:

- Line breaks come only from an explicit `\n`; there is no word wrap.
- Line pitch is read from the engine's own line-height metric (`output+0x18`), not from an even division of the used height. The engine lays text out as `usedH = topOffset + numLines × lineHeight`, so dividing evenly overstates the pitch and multi-line text slowly drifts off the paper's ruled panels. Using the real line-height keeps every row aligned, and the block is centered in the used height so the single-line case matches the simple even split.
- Baselines are placed against a fixed cap-height reference, measured once from `H`, rather than each string's own ink. A line with descenders no longer rides higher than an all-caps one.

The game's English text also carries characters that look like ASCII but are not, and a Latin font has no glyph for them: full-width digits and letters (`１２３`, U+FF01–FF5E), CJK label brackets (`【】`), and Unicode Roman numerals (`Ⅰ–Ⅻ`, used for dungeon tiers). The decoder folds each to its ASCII equivalent before layout. If the font still cannot draw a character, most often one of the controller-button icons, the whole string falls back to the `upscaled` path, so no glyph is ever dropped or shown as a tofu box.

The replacement typefaces are compiled into the DLL from the vendored `.ttf` files under `vendor/font/` (the byte arrays are generated at build time by `scripts/embed_font.py`, not checked in), so the feature needs no loose font file; a `arland-hires-font.ttf` beside the DLL overrides them. The override is looked for by swapping the DLL's own file name for the font's, which the mod declines when the result would not fit `MAX_PATH`, so an install path deep enough to overflow that buffer keeps the embedded font instead. The substitution is wired only for the English executables, because the per-game text allocator and consumer hooks resolve English-build addresses. On the multilingual builds those hooks stay unresolved and every mode is a safe no-op, so Japanese and Chinese text is never touched (and neither Latin font carries CJK glyphs regardless).

A smaller mechanism rides the same render path, correcting known English display typos. A per-game table rewrites the string pointer handed to the renderer before it is drawn or cached, for example Totori's "Synth Cateogry" to "Synth Category". Only the displayed text changes, the game's own string data is untouched, and because the rewrite happens on the render path the correction applies in every font mode.

## SMAA anti-aliasing

### TL;DR

The games ship without anti-aliasing, leaving visible jagged edges throughout the scene. The mod runs a lightweight smoothing pass after the 3D scene is finished but before the interface is drawn, so edges improve and menus and text stay crisp.

### Safety

If the runtime shader compiler is unavailable or any resource fails to be created, SMAA switches itself off for the session and the rest of the mod is unaffected. The pass runs before the interface is composited, so menus and text are not softened by it. Every piece of graphics state the three passes touch is saved and restored, leaving the pending draw or target change exactly as the game prepared it. When the normal path is selected, no second pass is layered silently on top of it. That would paper over a fault instead of exposing it.

### Details

The games have no built-in anti-aliasing at all. Multisampling would not fix what aliases most here in any case: it only smooths geometry silhouettes and cannot touch aliasing that lives inside a surface, such as texture and alpha-test edges. That is why the mod does not offer it (see "Historical background"). What it offers instead is SMAA (Enhanced Subpixel Morphological Anti-Aliasing, Jimenez et al.), a post-process that works on the finished image and smooths any visible edge regardless of how it was produced, at a single constant-cost pass. SMAA is `[Rendering] SMAA`, on by default, with `ARLAND_SMAA` as an override for comparison.

SMAA runs the standard three passes (luma/colour edge detection, blending-weight calculation against the precomputed `AreaTex`/`SearchTex` lookup textures, and neighborhood blending) using the reference shader (vendored under `vendor/smaa/`, MIT) compiled at runtime through `d3dcompiler`, at the `SMAA_PRESET_ULTRA` quality level. If the runtime shader compiler is unavailable or any resource fails to create, SMAA disables itself for the session and the rest of the mod is unaffected.

The pass is injected before the UI is composited, matching the approach the Atelier Graphics Tweak used, so the HUD, menus, and text stay crisp. The frame architectures differ enough that the two kinds of game need different injection points, and identifying the scene target is the part that took the most work to get right.

Rorona and Meruru render the 3D scene into an offscreen colour target, composite the HUD straight onto that same target, and only blit the finished result into the swap-chain backbuffer at the very end of the frame. The backbuffer therefore never holds a UI-free image, and every render-target or depth-state boundary reachable on it is already post-UI. Dimensions do not identify the scene target either. `CreateTexture2D` promotes the engine's hard-coded 1920×1080 auxiliary targets (blur, snapshot, conversation) to the main render size, so several textures answer any size test identically — and adding that promotion is exactly what silently broke this detection once. What does identify it is *what gets drawn into it*: only the 3D pass accumulates hundreds of depth-tested draws into a main-size single-sample target. SMAA runs on the first draw into that target with depth testing disabled, which is the first HUD element, so the passes land on the finished scene and the UI composites on top of the antialiased result. Binding away from the target is kept as a backstop for the case where the UI never touches it.

The whole frame is recorded on a deferred context and executed on the immediate one, so the injection happens during recording and the passes go into the command list in order. The passes snapshot and restore the pipeline state around that insertion. This is required not only when the trigger is the first UI draw, which executes immediately afterward, but also at the bind-away backstop: a render-target change alone does not rebuild the shaders, input layout, viewport, blend state and other bindings SMAA temporarily replaces. The passes re-enter the render-target and draw hooks that trigger them, so a thread-local guard prevents unbounded recursion, and the draw counter that gates injection is cleared before the passes run, not after.

Totori never rebinds its main colour target when the UI starts. A draw-state trace established that it keeps the same colour and depth targets attached, then disables depth testing before the final UI draw group. The render-target and depth-state setter hooks retain those two facts as atomic flags, so ordinary draw calls perform no D3D11 state queries; the first draw for which both flags identify the UI transition runs SMAA immediately beforehand. The shared state guard leaves that pending UI draw exactly as the game prepared it.

SMAA is independent of supersampling, because the two solve different problems: SMAA works on the finished image and so also smooths texture-interior, alpha-test, and shader/specular edges. Enabling one never disables the other. Supersampling changes only which target the passes run over, since the scene target is the render-resolution one; the injection point and the passes themselves are the same either way.

No Present-time fallback runs when pre-UI injection is selected, and that omission is the point. A silent full-frame pass layered on top of a working pre-UI pass softens the UI twice over while hiding that something is wrong; pre-UI injection either works or is a bug to fix. `ARLAND_SMAA_BOUNDARY=target|depth|both|scene` overrides the per-title default for testing.

SMAA runs once per frame in every path, latched and reset at Present. `ARLAND_SMAA_PREUI=0` forces the Present-time full-frame fallback (which also softens the UI slightly); `ARLAND_SMAA_DEBUG=1`/`2` visualize the edge and blend-weight intermediates.

SMAA cannot reconstruct detail that was already lost below the finished image's pixel grid. TellowKrinkle's per-sample sample-rate-shading approach is documented as non-functional on the older Arland DX titles; full supersampling is the option that captures such sub-pixel detail, and ships as the feature described next.

## Supersampling

### TL;DR

Supersampling renders the entire game at a higher resolution and then shrinks it cleanly to the display, recovering very fine detail that ordinary anti-aliasing cannot. The mod redirects the game's rendering into a larger internal image, downscales it once at the end, and pads with black bars when the shapes do not match.

### Safety

Only the game's own backbuffer views are redirected, and a view whose format the substitute cannot present is declined; that frame simply renders normally. The downscale pass saves and restores every piece of graphics state it binds, the same discipline the SMAA passes follow, so the context leaves Present exactly as the game left it. Where the aspect ratios differ, the frame gets black bars; the picture is never stretched to fit. Those bars are cleared every frame, so nothing stale can show through. The render size is capped, with the aspect preserved, at a point well past where these 2010-era assets have any detail left to give. If a build ever failed to bind the backbuffer the way this depends on, the mod leaves the image alone and says so in the log, rather than quietly doing nothing.

### Details

`[Rendering] RenderWidth`/`RenderHeight` larger than the display resolution renders the whole frame, scene and UI alike, at that larger size and downscales it once into the backbuffer just before Present. It resamples shading rather than geometry coverage, so it preserves detail smaller than a display pixel.

These games bind the swap-chain backbuffer itself as their colour render target, so the split between render size and present size is made by redirecting the backbuffer, not by intercepting draws. Every render-target view the game creates over the backbuffer is created over a mod-owned render-resolution texture instead. Because the substitution happens at view creation, everything downstream follows without further interception: binds, clears and the pre-UI SMAA injection all operate on the larger target, and the real backbuffer is touched only by the downscale. A view description naming a format the substitute cannot present is declined, and that frame renders into the backbuffer unchanged.

The downscale is a box filter. Each output pixel averages the source texels its own footprint covers, sampled on a grid spread across that footprint. That footprint is measured against the fitted rectangle the frame is drawn into rather than against the whole backbuffer, and because the fit preserves the frame's shape it is one number for both axes. The two are the same whenever the render and display aspects match, which is the ordinary case; where they differ, measuring against the backbuffer would understate whichever axis the fit is not constrained by and narrow the filter there. The tap count comes from the same number, so a frame constrained by its width is not sampled as though it were constrained by its height. At an integer ratio the samples land on texel centres and the result is an exact box, which is the case supersampling is really after; other ratios sample the footprint evenly and land close to it. An earlier scheme placed bilinear taps on texel corners to average 2×2 for free, which is a real optimisation at even ratios but degenerates at odd ones: at 3× the output pixel centre falls on a texel centre rather than a corner, so every tap collapsed to a single texel and the filter read the four corners of the 3×3 block while skipping its middle, undersampled and soft at once. The pass runs once per frame at display resolution, so correctness at every ratio is worth more than the saved samples.

When the render and display aspect ratios differ, the frame is fitted inside the backbuffer by the smaller of the two scale factors and the remainder is painted as black bars, which are cleared every frame because nothing else in the pipeline writes those pixels.

The render resolution is clamped to 7680×4320 with the aspect preserved. These are 2010-era assets: past about 8K there is no sub-pixel detail left for more samples to resolve, while cost keeps scaling with pixels drawn and the engine derives a large family of render targets from this size. The limit is diminishing returns first and memory second, not a constraint of the code.

Because the whole mechanism rests on the game binding the backbuffer as a render target, a build that never does so would be silently un-supersampled. That case is detected and reported: if no redirect has happened by the time the downscale would run, the backbuffer is left alone and a log line says so. A second line reports the largest viewport the engine actually drew with, so "supersampling is on but the edges are still jagged" can be answered from the log rather than guessed at.

## Borderless windowed mode

### TL;DR

Exclusive fullscreen makes these games slow to alt-tab and can interact poorly with modern desktops, while ordinary windowed mode leaves a visible frame. The mod creates a true borderless window that fills the current monitor without taking control of the display mode. It looks like fullscreen and alt-tabs instantly.

### Safety

On by default, and turning it off hands the game's own fullscreen setting back exactly as it stands. The mod asks for a plain window before the swap chain is created, so there is no exclusive-fullscreen mode to wrestle with afterwards, and it never takes ownership of the display mode. Because both the engine and the window manager restyle the window on their own, the style is re-checked about once a second, and only re-applied when it actually differs; repeated attempts are capped, so a window that refuses the style cannot spin. When the reposition would have to reach across threads it is posted asynchronously, so it can never block on another thread's message loop.

### Details

The games offer only windowed, with a title bar and border, or exclusive fullscreen. Exclusive fullscreen takes ownership of the display mode, which makes alt-tabbing slow and, under Wine or Proton, interacts badly with compositors and multi-monitor setups. `[Rendering] Borderless` runs the game as a plain window with its decorations removed, sized to cover the monitor it is on: it looks like fullscreen, alt-tabs instantly, and leaves the display mode alone. On by default. Turned off, the game's own fullscreen setting applies as it stands.

There are two halves to it. Before the swap chain is created its description is forced to windowed, because a swap chain created for exclusive fullscreen owns the display mode and restyling its window afterwards would fight it. After the swap chain exists, the window's caption and frame styles are replaced with a plain popup style and the window is sized to the full bounds of its current monitor, taken from the monitor the window is actually on so that a second display works and the window does not jump to the primary one. The monitor rectangle rather than the work area is used, so the taskbar is covered.

Applying the style once at startup does not hold: the engine restyles its own window in places, as does Wine's window-manager integration. So the check runs from `Present`, throttled to every sixtieth call — about once a second at 60 fps. The check itself is cheap, but the restyle it can trigger is not something to risk at frame rate, and nothing that reverts the window needs correcting inside 16 ms. Re-application is guarded by a comparison against the intended style and bounds, and repeated restyle attempts are capped so a window that refuses to take the style cannot spin. Because Present can run on a thread other than the one owning the window, the reposition is posted asynchronously in exactly that case, so it cannot block on another thread's message loop.

## Totori item and save corruption guard

### TL;DR

Totori trusts the container size recorded in a save even when it is larger than the 999 items it has room for. Item handling then writes past the end of the list and corrupts what follows, including equipment, which can crash battle entry or bomb use much later. The mod repairs such a save as it loads, and bounds the code that reads items so a bad value cannot crash the game meanwhile. A healthy save is untouched.

### Safety

Data is validated before anything is repaired, so a save whose records all check out is left exactly as it is and an unaffected file is never rewritten. The repairs are conservative: a damaged record keeps whatever part of it is trustworthy and only the unsafe remainder is cleared, and rebuilt skill entries come from tables inside the game's own executable rather than from values the mod invented. The limits are clamped back to the ones the game itself fixes at startup, not to numbers of the mod's choosing. Every hook is verified against exact function fingerprints first, so Rorona and Meruru, whose item system is different, install none of it. Each part can be switched off for comparison. What this cannot do is bring back items that vanilla already overwrote before the mod was installed; it repairs the structure so the damage stops there.

### Details

The work behind this section started from a save file shared by a member of the community whose game had become unplayable. That file is what made the mechanism visible and what the repair was built and validated against; how its container limit came to be wrong in the first place is not established, and does not matter to the fix, because the damage that follows is done entirely by the game's own code once the bad value is present.

Totori's chunk 7 contains 100 carried-item records, 999 container records and four trailer dwords. The 999 limit is fixed in both executables. Startup builds exactly that many records, the list is initialised with 999 as both the number it will scan and the number it actually has room for, and the save reader and writer move exactly 999 records' worth of bytes. The damaged save instead recorded a container limit of 5000, and the loader restored that number as the count to scan without ever comparing it against the 999 records that actually exist.

The routines that count and insert items use the number to scan, not the number that exists. With 5000 loaded they continue through globals beyond the physical array, and insert copies a whole item record into the first memory that resembles a vacant record. This is directly visible in the damaged save: coherent item records with ids 2840..2852 overlap equipment at offsets `+0x28` and `+0x24`. Those apparently irregular equipment offsets lie exactly on the container's continued `0x34` grid — container records 1171 and 1209 — not on a variable-length serialization layout. The equipment writer then persists the already-corrupted globals. One bad dword therefore does not stay one bad dword: vanilla turns it into continuing runtime damage that is written back to disk on every save.

The repair runs where that data enters the game. Totori reuses the same chunk deserializers while constructing the save-list previews, so the mod also hooks the dedicated selected-save load routine and scopes all mutation to that synchronous call. During a preview, the loader hooks run pure, early-exit versions of the same checks and remember only whether the selected-load path would repair the slot. The save-list row formatter then adds `[TBR]` ("to be repaired") to that slot's label. It rebuilds the label from the unmodified preview record each time, so the suffix cannot accumulate; `ARLAND_ITEM_SANITIZE=0` disables both repair and the marker. A runtime stack trace distinguished eight preview calls through the tail-call path from the one direct selected-save call in both executables' shared control-flow shape. During that load the hooks check each live list against its expected kind, capacity, backing pointer and mapped range, clamp the limits back to the real ones, and scan every fixed inventory record for damage. A damaged record with trustworthy ids, finite quality and valid traits keeps that prefix while its unsafe effect suffix is cleared; a record whose prefix cannot be trusted is reset to the engine's canonical empty value. The same validation covers all 30 equipped records. Damaged saved character-skill entries are rebuilt from the executable's own per-character source tables, bounded set metadata is restored, and the engine's own equipment-stat recalculation runs after an equipment repair. The game naturally writes those repaired live records the next time the player saves. Nothing is rewritten speculatively: a record that passes validation is not touched, so an unaffected save loads and saves exactly as it would without the mod.

Guards on the read side remain necessary, because an item can also reach these routines from outside the arrays the save hooks audit. Battle entry scans an item's effects and traits, and originally checked only that each table index was not negative. The mod skips indices beyond the measured 223-effect and 204-trait tables. Using a bomb or a craft item reaches a separate modifier builder with the same unbounded lookup and the same quality arithmetic. It normally runs untouched; if quality or an effect index is invalid, the detour calls it synchronously with a sanitized local copy of the item. The builder has a second unchecked lookup: its 161-record action-item table contains ids 0..159 and 163, but vanilla uses the lookup's `-1` failure as an index one record before that table. The detour rejects an id outside that exact set and leaves the caller's modifier vector empty. All seven direct callers in each Totori build initialize that vector before the call and either test its count or iterate from begin to end, so empty is an already-supported failure result. The predicate stays inside the action builder and is not applied to saved items generally, since valid weapons, armour and materials carry ids outside the action-item table.

Every target and source table is checked against its expected shape before a hook installs, and a partial installation stays transparent because the hooks are installed from the display path inward. Rorona and Meruru match none of these Totori routines and install nothing. `ARLAND_ITEM_PROBE=1` logs every index the guards reject rather than only the first, `ARLAND_ITEM_GUARD=0` restores the original read routines, and `ARLAND_ITEM_SANITIZE=0` disables the load repair and the save-list marker.

## Totori asynchronous render-stream lifetime fix

### TL;DR

Using a bomb in Totori could crash while the battle camera was moving, sometimes before the animation began. The render thread was handed a pointer to temporary buffer state with nothing keeping that state alive until the queued command ran, so cleanup could free it first and the renderer would read recycled memory. The mod keeps that state alive until the render worker has finished with it.

### Safety

The correction is Totori-only and every producer and consumer hook is checked against the exact English or multilingual executable prologue before installation. The game's original command producers and worker consumers still perform all state encoding and Direct3D calls; the mod only adds and later removes one game-wrapper reference around each newly queued stream command. Repeated bindings take a direct comparison fast path with no lock, allocation, memory query or reference-count operation. Totori's stream wrappers use non-atomic reference counts, so both the added reference and its eventual release stay on the original producer thread. The worker merely records that it has consumed the command. The vertex consumer's separate packed slot, stride and state argument is preserved exactly when the original handler is called.

### Details

Both crashes ended at the same instruction inside the graphics driver, writing through a pointer that could not be real. The game code that led there was the worker that consumes the index-stream command, which rebuilds a raw pointer out of the command and hands it to the driver. The setter reads the `CDX11IndexStream` wrapper's D3D buffer at `+0x10` and format at `+0x18`. At both crashes those fields held a pointer that could not be real and a format the game never uses for index data, which is what freed memory reused for something else looks like. The repeated invalid contents and identical call chain identify a freed and recycled wrapper, not a malformed bomb or an unsupported index format.

The producer writes the index-stream command as a short record holding a header and a raw pointer to the stream object. The wrapper has a plain reference count, but the command takes no reference of its own. The current game state and binding objects can therefore release their references after a targeting or camera transition while the asynchronous render worker still has the command waiting. One captured failure occurred seven milliseconds after the item-use setup completed, before the animation began, with two uses still remaining. That rules out final-use inventory compaction and places the trigger in the render-state transition rather than the bomb's gameplay effect.

Command `0x28` has the same ownership defect for vertex streams. The vertex-stream command works the same way, with its own record holding a raw pointer to the stream object, while its worker handler reads the underlying buffer and updates wrapper state. It had not yet produced a captured crash, but leaving the exact sibling command unprotected would preserve the same use-after-free under a different timing and allocator layout, so both stream types use the same lifetime correction.

On a changed, non-null binding, the producer increments the wrapper's reference count before calling the original queue writer. It then associates the appended command address with that pin. A small pending-publication record covers the race in which the worker reaches a command before the original producer has returned; the worker can claim that already-existing pin without waiting for the producer. After the original consumer returns, the worker places the release on a deferred list. The producer drains releases belonging to its own thread on a later changed stream binding, decrements the wrapper there and invokes the game's normal finalizer only if the count reaches zero. This preserves the game's thread affinity and never makes its non-atomic reference count cross-thread.

The crash was timing-dependent, which is what made it hard to place: transient effect geometry, queue depth and allocator reuse decide whether the freed memory has been handed to something else by the time the render worker reads it.

## Totori shop heap-corruption fix

### TL;DR

Leaving a shop in Totori could crash the game, immediately or a few seconds later. The shop could write just outside its item list before a valid row had been selected, corrupting memory that then failed during cleanup or in some later task. The mod prevents that write. Buying, selling and everything else behave as before.

### Safety

The detour intervenes only when vanilla is about to commit an input value and the previous-row index does not name a row in the live vector. Empty lists already return before that branch and valid indices run the original untouched. The input state is hidden only for the duration of the original call and restored before returning, and the commit the update skipped is taken on the next one. Both Totori executables have the same function bytes and object layout; Rorona and Meruru install nothing. As with every executable hook, the complete target prologue is verified before installation.

### Details

`ShopGoodsList` stores row records of stride `0x54` in the vector `[this+0x60, this+0x68)` and its previous-row index at `this+0x54`. Construction and initialization set that index to `-1`. The update routine first checks that the list is not empty. When the input object at `this+0x48` reports state 2, it then sign-extends the previous-row index and writes the input's `+0x28` value to `begin + index*0x54 + 0x4c`, without checking the index. Only after that store does it refresh the list, which replaces the stored index with the current selection. If state 2 arrives on the first update, the destination is therefore `begin + (-1*0x54) + 0x4c = begin-8`.

With a list of a dozen rows and no selection yet made, the destination the game computes lands one slot before the list begins, and the game writes a zero there. Whether that is noticed immediately or much later depends on what the heap happened to put in that slot, which is why the same fault surfaced sometimes during shop teardown and sometimes seconds afterwards.

## Synthesis card animation rate

### TL;DR

The animation on the product cards during synthesis ran at the frame rate rather than the rate it was drawn for, so on a 144 Hz display it played more than twice too fast, and faster still above that. It now runs at its intended rate, in all three games. At 59.94 Hz and below nothing changes.

### Safety

One prologue-verified MinHook detour per executable, on a function whose 16-byte window is byte-identical in all six Arland builds. When a tick is due the original is called completely untouched, so the shipped behaviour is reproduced bit-for-bit rather than reimplemented: the correction only decides whether to call it. The accumulator is read through the guarded-read helpers and written only after a writability check, so a bad object address declines rather than faults. `ARLAND_SYNTH_RATE=0` restores the original behaviour for a launch.

### Details

`Card::Update` is a fixed-timestep pump whose loop is **bottom-tested**. The five bytes where a pre-test belongs are an alignment NOP, so the body runs at least once per rendered frame. It works out how many steps are owed by multiplying the accumulated time by the intended rate and throwing away the fraction, then takes that many steps, but never fewer than one. At 59.94 Hz and below at least one step is owed each frame, so taking one is correct. Above it `n` is always 0 and the pump ticks anyway, so the tick rate becomes the frame rate. A corroborating symptom is that the accumulator drifts unboundedly negative above 60 fps, because the subtraction still fires on every frame whose count was zero.

The correction supplies the missing pre-test and nothing else. The detour reads the same accumulator the game uses, adds the frame delta, and evaluates the game's own test with the game's own constants, a step of just under a sixtieth of a second and its reciprocal, hardcoded as exact bit patterns because the predicate is a truncation and a differently-rounded literal would decide a tick was due on a different frame. If a tick is due, the original runs. If not, the elapsed time is banked into the accumulator and the frame returns the original's only return value.

It also carries a drift self-heal. Because running unfixed above 60 Hz walks the accumulator unboundedly negative, enabling the fix mid-session could otherwise leave it below the threshold for a long time and appear frozen. Normal operation keeps the value within one step of zero, so anything a whole second in the past is reset.

One pump in each of the six executables is affected. They were found by searching for the shape of the mistake rather than by matching against a known example: a whole-number conversion of a multiply by a frame rate, with no test between that conversion and the top of the loop. Exactly one turned up per executable, and every other fixed-step pump in the trilogy tests before it steps, so this is the whole of the defect rather than one case of a family.

The same defect and the same correction appear in Atelier Escha & Logy DX and Atelier Shallie DX, where this implementation was written first.

## Field movement and collision

### TL;DR

This covers how characters move on the field map, and how they push each other apart. A monster chasing you used to be carried the last part of each step in a single frame, which at a high refresh rate happened fast enough to cross the reach of your staff between one frame and the next and lose you the encounter. Separately, the engine's check for two characters standing inside each other pulls them together when they are already apart. Both on by default, all three games and both language versions.

### Safety

Each correction is a small, bounded change to one calculation. The first limits how fast a monster can be carried toward its target, never where it ends up or when it gets there. The second clamps a subtraction so it can only ever push characters apart, never draw them together; genuine overlap separates exactly as before. Each has its own key in `[Field]`, `MonsterSnapFix` and `CharacterPullFix`, so either can be turned off on its own. Every address is verified against a full instruction prologue before anything is patched, as everywhere else.

### Details

The two corrections here are independent of each other. Both cover all six executables, English and multilingual, in all three games. Both live in `src/field_collision_fix.cpp` under `[Field] MonsterSnapFix` and `[Field] CharacterPullFix`. Rorona and Meruru name the field character family properly in RTTI (`nspFM::clsFM*`) and expose the chara manager's own update as a per-frame entry, so their container and node offsets were read from each game's own disassembly rather than ported: the enemy vector sits at `+0x38`/`+0x40` against Totori's `+0xd0`/`+0xd8`, and a character reaches its scene node through `+0xa0` against Totori's `+0xa8`. Every multilingual address was derived from its own build rather than ported: each setter from the same unique byte signature, each per-frame entry from a homologue MATCH with a raw-identical prologue and equal function size, and each container and node offset re-read in that build's disassembly.

### Monsters snapping across the ground

A field monster chasing the player runs a three-step cycle in its brain: mode 0 re-targets and builds a mover aimed at the player, mode 2 copies that mover's position into the brain's intended position every frame, and mode 1 notices the mover has expired and begins the next segment. The mover's lifetime is set in its constructor as distance divided by speed, so with the monster held at a fixed separation from a stationary player the cycle is metronomic; it measured about 540 ms.

The intended position becomes movement through `FieldMapCharaBase::Update`, which computes `velocity = (intended - current) / dt`, after which the character controller integrates `position += velocity * dt`. The frame delta cancels, so the whole gap is closed in one frame however long that frame is. At a segment boundary the intended position moves abruptly twice, once when the mover snaps to its stale target and once when the next segment re-anchors, and the character covers the entire distance across two frames.

The displacement is therefore identical at any frame rate, which is what made this easy to misread: it measures the same at 30, 60 and 200 frames per second. What changes is how long those two frames last. The same 0.2 to 0.6 unit correction takes about 66 ms at 30 fps and about 10 ms at 200. At console frame rate it reads as a brisk step; at high refresh it reads as the monster teleporting, and a monster that leaves the staff's reach between two frames takes the encounter with it, which is why swings appeared not to register.

The correction spreads the movement over time rather than reducing it. The monster still reaches exactly the position the game asked for, on the same schedule, but cannot be carried faster than a walking character plausibly moves. The limit is expressed as a speed rather than a distance per call, because a fixed per-call step would itself be frame-rate dependent and would reintroduce the problem in another form. Only charas the field map lists as enemies are limited; the player, the party and everything else are untouched. With it active the largest correction the engine requested across a test session fell from 0.87 units to 0.086. `ARLAND_MONSTER_SNAP_SPEED` sets the limit in world units per second for A/B work, default 6.

### Characters pulled together

The engine's character-versus-character separation routine computes the horizontal distance between two controllers and derives a push depth as the sum of their radii minus that distance. The result is signed and nothing clamps it, so when the pair is reported with the two further apart than their combined radius the depth is negative and the push reverses: the two are drawn together until the distance equals the radii sum exactly. That is an equality constraint where a separation constraint belongs, and it parks a monster on a fixed ring around the player rather than merely keeping it from overlapping. It was measured pulling a monster inward from 0.867 to 0.800 units.

The correction is `max(depth, 0)`, applied as a ten-byte splice to a trampoline because the clamp does not fit in place without displacing instructions that cannot be replayed. Genuine overlap separates exactly as before. The guard above the subtraction is not a substitute: it compares the distance against about `1.14e-5` and only rejects a near-zero separation, routing that case to a fallback whose own subtraction is correct because its distance is already negligible.

This is not what produced the snapping above, and correcting it alone left that symptom unchanged. It ships because it is wrong, not because it was the cause. Unlike the rate limit it is a change to a shared routine, so it reaches every character pair including the player and party members; that is why it carries its own key and can be turned off on its own.

## Startup logo skip

### TL;DR

The publisher and developer logos play while the game is still loading, so they cover that work rather than delay it. Skipping them shows a plain screen for as long as loading actually takes, rather than starting the game sooner. Rorona has the longest sequence and gains the most. `[Startup] SkipLogos=true`, all three games, off by default.

### Safety

The skip is two detours on adjacent methods of one class, each verified against its expected prologue and installed only when the capability matrix supports the feature and the user opted in. The skip flag is resolved once at install time and read as a plain atomic, because both detours run on the render thread and must not touch the INI or the environment there. The draw suppression installs first, so a partial install leaves the shipped sequence running and drawing rather than a stopped sequence whose picture layers are stranded on screen. Neither detour frees anything or alters the object's layout, so the game's own destructor still releases the picture layers. With the option off, both detours call the original and behavior is unchanged.

### Details

The logos do not belong to the title-screen state machine. `Title` carries three logo states inherited from the original release, but nothing requests them in the DX ports; every caller of the state-request function was enumerated and none does.

They belong to `ThreadEasyRenderLogo`, a small object the application constructs before it begins initializing the engine. Its update and draw methods are slots 1 and 2 of its virtual table, and each has the same prologue in both builds. The object holds three fullscreen picture layers, a phase at `+0x20`, a page index at `+0x24` and a timer at `+0x28`, and steps a six-phase sequence: fade a layer in over half a second, hold it, fade it out over half a second, advance. The page table selects warning text, then Koei Tecmo, then Gust; the English builds start at the second entry and never display the warning screen.

The sequence is waited on in two places, and both poll only the phase field: the application's boot function, after it has finished the whole engine and resource initialization, and a title-side player that produces the attract replay once the title screen has been idle. Writing the terminal phase therefore releases both, which is why one hook covers the boot logos and the replay.

The update detour writes that terminal phase and returns without calling the original. It cannot simply force the phase and let the original run, because the original's tail advances each picture layer's fade, which would bring a logo up during the load. Not advancing them leaves their alpha where construction left it, and whether that renders anything depends on state the mod does not own, so the draw is suppressed as well rather than reasoned about. The two detours together guarantee no logo pixels and an immediate release, at the cost of the clear colour being visible for the duration of the load.

All three games are wired, both builds each. Every address came from that game's own RTTI vtable rather than from homology, and the two prologue windows above are byte-identical in all six executables.

## Opening-movie skip

### TL;DR

`[Startup] SkipIntroMovie=true` skips the opening movie that plays after the startup logos. The ending movies are not affected. All three games, off by default.

### Safety

One prologue-verified MinHook detour, installed only when the capability matrix supports the feature and the user opted in. The skip is confined to the opening by the movie index the routine already receives as an argument; every other index calls the original untouched, so the endings play normally. The detour reproduces the routine's own "cannot play" branch rather than inventing a new state, and it neither allocates nor frees. The enable flag is resolved once at install time and read as a plain atomic. With the option off the original is always called.

### Details

All movies go through one routine that opens them, and its prologue is the same in both builds. It takes the player object and an index into a four-entry table of records, each holding a file name pointer, a frame size and a caption pointer. Index 0 is `opening.wmv`; the rest are the endings.

The routine begins by asking whether the movie subsystem is ready. When it is not, the routine writes 1 to the player's state byte at `+0x30` and returns without opening anything. The per-frame movie update reads that byte first and returns "not playing" immediately when it is set, so the caller treats the movie as finished and moves on. That is the engine's own graceful degradation for a movie it cannot play, which is what makes it a safe thing to trigger deliberately: the surrounding code already handles it.

The detour therefore does exactly what that branch does. For index 0 with the option on, it writes the state byte and returns without calling the original. Nothing else in the movie path is touched.

All three games are wired. The English address for each was anchored on the single reference to that build's own `Res/x64/movie` path string rather than on a homology vote, which matters because the vote came back WEAK for Totori: its routine is a slightly different compile and carries its own prologue window. The multilingual addresses are MATCH from their own English build, and index 0 was confirmed to be `opening.wmv` in all six movie tables.

## Save data slots view pacing

### TL;DR

Bringing up your save data waited on a fixed timer before the view appeared, and again before it would let you leave, with nothing being loaded during either wait. The view now opens as soon as the game has built it. `[Menus] FastSaveMenu`, on by default in all three games and both language versions.

### Safety

In-place byte patches over branch instructions, all-or-nothing: every site's sixteen-byte window is verified against the build's own bytes before any of them is written, and a single mismatch leaves the executable untouched. The gates themselves hook nothing and add no code; each patch turns one conditional branch into `nop`, so the taken path is the one the game already runs once its timer expires. The gates are per game and per build, four in Totori and five in Rorona and Meruru.

### Details

Each gate is the same shape. A float accumulates elapsed time in a scene field, a `comiss` compares it against a constant from a shared pool, and a conditional branch skips the work while the accumulator is short of it. Removing the branch runs the work on the first frame rather than after the delay.

What makes this safe is what the gates do not do. None of them polls a file, an asynchronous load, or a readiness flag; every condition is elapsed time and nothing else. The clearest case is the exit gate, which reads a fade's own busy flag, waits for it to clear, and then counts a further 1.5 seconds on top. The timer there is pacing layered over a real readiness check, not standing in for one. The work behind the first gate is an allocation and a constructor that builds four sub-objects synchronously, so there is nothing outstanding for the delay to cover.

One concern came out of reading the code and did not survive playing it. Rorona's title gate sits in a function that also starts a fade of 0.5 seconds, from the same pool entry the gate compares against, which reads like a wait pacing a real animation one for one. In play Rorona shows no fade into the view at all, with the gate or without it. The other two do, and neither is affected: Totori fades slightly to white and Meruru fades to black. The trilogy is simply not consistent here. Either the matching constant was a coincidence or the fade drives something that never becomes visible; it does not constrain the change.

Moving the cursor between slots reaches no file or storage call at all: the boot scan parses every save once into a record vector, and the view reads that. The row builder, which runs every frame the view is up and copies about 4 KB per row, averages 36 microseconds a call. What is left is a single text render of about 9 milliseconds when the highlighted slot changes, which is the same text path documented under [Repeated font-atlas reads](#repeated-font-atlas-reads).

### The carried press

Removing the waits exposed a second defect. The view acts on a button's release, not its press: the first thing `WinSaveLoadScene::Update` does once its own 0.1 s input window expires is ask the pad whether button `0xc` was just released. So the press that opens the view is harmless and the release of that same press is not. Hold confirm, and the view your press just opened consumes your release as its own confirm and shows the load prompt. Vanilla hides this behind the 0.5 s wait: by the time the view exists, the player has let go. It is still reachable in vanilla with a hold of over half a second.

The engine's pad state is two 16-byte arrays, current and previous, one byte per button, previous `0x10` above current. The query is a plain edge test: mode 0 is held, mode 1 is `cur && !prev` (just pressed), mode 2 is `prev && !cur` (just released), and the view asks for mode 2.

The repair keeps that edge test honest rather than restoring the delay. On the first frame the view is open, every button already down was down before the view existed, so no edge belongs to this view; those buttons are recorded. While each stays down its previous byte is forced to match its current one, making both edges impossible for it. On the frame it comes up, the forced write lands first and lands as zero, so the release edge cannot fire, and the button stops being tracked. Buttons pressed after the view opened are never touched.

`Update` was taken from each build's own `WinSaveLoadScene` RTTI vtable, slot 1, not from homology. All six prologues are identical apart from one field offset that differs between Totori and the other two, and each build's button-state arrays were found by following the query its own update method calls. The repair installs only when the gate patch succeeded, and the gates are put back when the repair cannot install, so the feature is either wholly present or wholly absent. Turning it off restores vanilla exactly, including vanilla's own version of this defect.

## Startup window background

### TL;DR

When a game starts there is about a second between its window appearing and the first frame being drawn. The game asks Windows to fill that window with grey, so you see a grey rectangle. The mod asks for black instead. Always on, in all three games and both language versions, with no setting.

### Safety

The mod intercepts one call, changes one field of one window class, and only when two conditions hold together: the class is the engine's own, named `KTGL.A11`, and the brush it carries is exactly the grey stock brush. Anything else is passed through untouched, so a build that stopped asking for grey would simply stop being affected. Windows hands out one shared handle per stock brush for the whole process, so the comparison is an exact match rather than a guess. No game code is patched, no address is hardcoded, and the structure the game passed is never written to: the swap is made on a copy.

### Details

Every window in Windows belongs to a window class, and a class carries a background brush. Windows paints the client area with that brush whenever the window needs erasing and the program does not handle it. All six executables register their class with the grey stock brush, and their window procedure passes the erase message straight to the default handler, so grey is what gets painted. Nothing else fills the window until the game presents its first frame, which is why the grey stays on screen for as long as startup takes.

This is the game's own doing. It looks like a Wine or Proton artifact because that is where most people see it, but Windows shows the same grey.

The mod intercepts the call that registers the class, copies the structure the game passed, swaps the brush for the black stock brush, and registers the copy.

The interception is installed when the mod's DLL is loaded, which happens before the game's own startup code runs. That timing is the point: the class is registered once, early, and a change made after that would be too late to matter.

## Window title on Western locales

### TL;DR

Running the multilingual build in Japanese on a Western system shows garbage in the title bar. Windows cannot show the real Japanese title there at all, so the mod substitutes a readable ASCII transliteration of it. Always on when that exact situation is detected, and inert everywhere else.

### Safety

The substitution installs only on the three multilingual executables, only when the game's language setting is Japanese, and only when the system codepage can render neither Japanese nor UTF-8; any other combination leaves the hooks uninstalled or inert. Within such a process it replaces a title only on a top-level window that the game executable itself created and whose title actually contains non-ASCII bytes, so child controls and windows belonging to other injected software are never touched.

### Details

The multilingual build passes its UTF-8 Japanese title to the ANSI window API. On a system whose ANSI codepage is not UTF-8, which is the normal case under Wine and Proton and on Western Windows, those bytes decode as the wrong characters and the title bar shows mojibake. The window cannot be given the correct Japanese text either: it is an ANSI-classed window, so even the Unicode API's text is converted through the system codepage on the way in, and the Japanese characters collapse to question marks. That was confirmed by writing the title through the Unicode API and reading back what was stored.

Plain ASCII is what survives that conversion, so the mod substitutes a per-game ASCII transliteration of the Japanese title ("Rorona no Atelier ~Arland no Renkinjutsushi~ DX" and so on), hooking window creation and the title-setting call. Both hooks act only on a top-level window created by the game executable itself. The instance check is backed by the disassembly: all three multilingual builds pass their own module handle into both the window-class registration and the window creation call, so the game's window always qualifies and foreign windows never do.

The hooks are installed at DLL load, before the game creates its window, for the same reason as the background fix above. On a Japanese system codepage, or a UTF-8 one, the real title renders and nothing installs.

## Diagnostics

Every session log opens with the commit the binary was built from, as `build=<hash>`, with `-dirty` appended when the working tree carried uncommitted changes and `unknown` when the build had no git available. A version number cannot answer "which binary is this": a local build and a tagged release can both report the same version while differing by a shipped fix, and a crash report was once diagnosed against the wrong binary for exactly that reason. The stamp is generated by `vcs_tag`, so it is re-evaluated on every build rather than frozen when the build directory was configured.

The crash report identifies the faulting module by handle rather than by name. Both this mod and the system Direct3D implementation it forwards to are named `d3d11.dll`, so a name comparison reported every DXVK fault as `MOD(this)` and printed its offset as though it were an offset into this module. A fault in the system implementation is now categorised `GRAPHICS` and marked `(system, not this mod)`.

Each session log begins with the version compiled from the repository's `VERSION` file, the recognized title and executable build, the configuration file and relevant environment overrides, the effective render settings, and concise `FIXES` records for the subsystems that installed. Installation failures, signature/prologue declines, device loss and other warnings remain visible in the normal log. A failure reached from a hot D3D path is written on its first occurrence, with sampled repeats in verbose mode, so a persistent resource failure cannot flood the file and erase the useful startup context. Successful low-level hook creation, raw addresses and object fields, repeated render-resource operations and battle lifecycle telemetry are verbose-only so the default log records release-relevant state without becoming a trace.

If a game crashes, a last-chance exception filter appends a post-mortem to `arland-fix.log` before the process dies: the exception code, the faulting address expressed as module+offset, the register state, and a conservative stack scan in which every stack slot pointing into executable module code is resolved to module+offset. The faulting module is also bucketed into a coarse category (`AUDIO`, `MOD(this)`, `GAME`, `GRAPHICS`, `SYSTEM`, `OTHER`, or `UNKNOWN` when the name cannot be read), and an audio module anywhere in the stack is flagged, so a fault the D3D11 layer cannot address is identified as such. `AUDIO`, `MOD(this)` and `GAME` are tested before the broader buckets, so they win ties. `ARLAND_CRASH_LOG=0` stands the whole post-mortem down. The handler chains to any previously installed filter and lets the exception continue, so debugger and Wine crash handling are unaffected. The previous session's log is preserved as `arland-fix.log.old` on each launch so a crash report survives the next start.

With `[Diagnostics] VerboseLogging` enabled (off by default; `ARLAND_VERBOSE_LOG` overrides), successful low-level hook installation, render-resource redirections, battle scene/state transitions and process memory (working set, peak and commit) are logged in addition to the normal records. The `MEM` line appears about every ten seconds as a passive probe for a crash that hangs rather than throwing, so a memory climb can be captured even when the exception post-mortem never runs. Menu statistics come with it: per-transition cache statistics, per-conversation cache hit/miss totals, and a periodic per-frame heartbeat (text-render calls and time, cache hits and misses, and the battle/cinematic tracking flags) used to localize frame-time regressions. `ARLAND_MENU_STATS` overrides that either way; unset, it follows `VerboseLogging`, which it can safely do because it only observes. The traces that do move the code path they report on, the menu-transition trace and the cut-in probe among them, never follow the checkbox and have to be asked for by name.

`ARLAND_ATLAS_RECONCILE=1` checks the font-atlas cache's invalidation coverage, which is what both snapshot lifetimes rest on. It counts writes to a mutable atlas as the D3D11 layer sees them and compares them against the middleware unlocks the game-side hook observes, reporting both with an `unmatched_writes` figure on a periodic `ATLAS reconcile` line. That figure must stay at zero: above it, some path mutates an atlas without reaching the unlock hook, so a snapshot could be served after the pixels behind it changed. It is off by default because the resource predicate inspects every mapped resource.

### The switches

Environment variables, not `arland-fix.ini` keys. Most only add logging. The ones marked *(A/B)* switch a shipped fix back off so its effect can be compared against unmodified behaviour, and those do change how the game plays for that launch.

| Variable | Effect |
| --- | --- |
| `ARLAND_DISABLE=1` | Stands the whole mod down for one launch. `d3d11.dll` still loads and still forwards Direct3D, but installs no hooks and changes nothing, so the game runs as it shipped. The launcher's **Play without the mod** button passes this. It is the quickest way to tell a game problem apart from a mod problem without moving files out of the game folder. |
| `ARLAND_VERBOSE_LOG=1` | Same as `[Diagnostics] VerboseLogging`. Adds hook addresses, resource redirections and battle-object state, and turns on the observing diagnostics below, so a detailed log can be asked for without setting several variables. The heavier traces stay opt-in individually, and `ARLAND_MENU_TRANSITION_TRACE` is never included because it changes the code path it reports on. |
| `ARLAND_CRASH_LOG=0` | Stands the crash post-mortem down. It is otherwise always written. |
| `ARLAND_PERF_LOG=1` | Follows verbose logging unless set explicitly. Writes a `PERF` line every ten seconds: average frame rate, average frame time, and the worst single frame in that window. An average alone cannot tell a steady 60 from a steady 60 with a 90 ms hitch. |
| `ARLAND_MENU_STATS=1` | Per-drain menu timings and cache hit rates. Follows verbose logging unless set explicitly. |
| `ARLAND_MENU_TRANSITION_TRACE=1` | Traces menu transitions. Never enabled by verbose logging, because it alters the path it reports on. |
| `ARLAND_FIELD_TRACE=1` | Logs the field character's state around each loss of footing. |
| `ARLAND_PRESENT_TRACE=1` | Reports how the finished frame reaches the screen. Needs a display resolution set. |
| `ARLAND_SCENE_TRACE=1` | Traces scene-target selection. |
| `ARLAND_ITEM_PROBE=1` | Logs every malformed item index Totori's guards reject, with the whole record and the caller, not just the first. |
| `ARLAND_ITEM_SAVE_TRACE=1` | Traces Totori's save-data repair as it runs. |
| `ARLAND_ATLAS_RECONCILE=1` | Checks the font-atlas cache's invalidation coverage against what the D3D11 layer sees. |
| `ARLAND_LAUNCHER_DIAGNOSTIC=1` | Extra logging from the 32-bit launcher proxy. |
| `ARLAND_PRESENT_INTERVAL=0` | Forces vsync off; `1`, `2`, `3` present every Nth refresh. Useful with an external frame limiter. Unset leaves the game's own choice alone. `0` will tear in exclusive fullscreen. |
| `ARLAND_NO_REDIRECT=1` | Opens Koei Tecmo's own launcher instead of the mod's, for one launch. The launcher's buttons for the original tools use this, which is why they cannot loop back. |
| `ARLAND_HIRES_SCALE`, `ARLAND_HIRES_VOFF` | Nudge the replacement font's size and vertical position, for quick tuning. |
| `ARLAND_HIRES_FILTER`, `ARLAND_HIRES_SDF`, `ARLAND_HIRES_SHARPEN` | Select the upscale filter and its steepening and sharpening. |
| `ARLAND_MENU_FIX=0` | *(A/B)* Skips the game-code detours and leaves the synchronization layer active. |
| `ARLAND_ATLAS_CACHE=0` | *(A/B)* Disables the font-atlas cache. |
| `ARLAND_FRAME_ATLAS_CACHE` | *(A/B)* `0` holds Rorona and Totori to the queue-scoped lifetime; `1` opts Meruru into the frame-scoped one. |
| `ARLAND_TEXT_BITMAP_CACHE=0` | *(A/B)* Disables Meruru's conversation-scoped text cache. |
| `ARLAND_FIELD_ENGINE_FIX=0` | *(A/B)* Restores the game's own minimum-movement distance, reinstating the high-refresh field problems. |
| `ARLAND_FIELD_STABILIZER=0` | *(A/B)* Stops holding the character still at rest. Ignored unless the switch above is on. |
| `ARLAND_WORLDMAP_FIX=0` | *(A/B)* Restores the frame-tied travel-map cursor speed. |
| `ARLAND_MONSTER_SNAP=0`, `ARLAND_MONSTER_SNAP_SPEED` | *(A/B)* Disable the monster rate limit, or set it in world units per second. |
| `ARLAND_CHARACTER_PULL=0` | *(A/B)* Restores the unclamped character separation. |
| `ARLAND_SAVE_MENU_GATES=0` | *(A/B)* Restores the original waits in front of the save data slots view. |
| `ARLAND_CUTIN_SHADOWS=0`, `ARLAND_CUTIN_DIMMING` | *(A/B)* Disable the cut-in shadow restoration, or restore the original dimming. |
| `ARLAND_CUTIN_ACTOR_CLEAR=0` | *(A/B)* Stops clearing a mid-cut-in battler's shadow when its fade begins. |
| `ARLAND_BATTLE_SHADOWS=0` | *(A/B)* Disables Rorona's battle shadow restoration. |
| `ARLAND_BATTLE_MODE_GATE=0` | *(A/B)* Falls back to the watchdog alone for detecting the end of a battle. |
| `ARLAND_ITEM_GUARD=0` | *(A/B)* Restores Totori's own unbounded item-effect lookups. |
| `ARLAND_ITEM_SANITIZE=0` | *(A/B)* Disables the repair of damaged Totori save data on load and save. |
| `ARLAND_STREAM_LIFETIME_FIX=0` | *(A/B)* Disables Totori's queued stream lifetime correction. |
| `ARLAND_SYNTH_RATE=0` | *(A/B)* Restores the frame-tied synthesis card animation. |
| `ARLAND_SKIP_LOGOS`, `ARLAND_SKIP_INTRO_MOVIE` | Override the two startup skips for one launch. |
| `ARLAND_BORDERLESS`, `ARLAND_SMAA`, `ARLAND_ANISO`, `ARLAND_SHADOW_MULTIPLIER`, `ARLAND_UIFONT`, `ARLAND_UI_SCALE` | Override the matching `arland-fix.ini` keys for one launch. |
| `ARLAND_SMAA_BOUNDARY`, `ARLAND_SMAA_PREUI` | Select where the anti-aliasing pass is injected. |
| `ARLAND_DEBUG_VIEW` | Selects a debug view; overrides `[Debug] View`. `ARLAND_SMAA_DEBUG=1|2` still selects the two SMAA views. |

### Debug views

Turning on verbose logging adds a **Debug** tab to the launcher holding a single **Debug view** dropdown. These are diagnostics rather than quality settings: each replaces what is drawn so one part of the mod can be checked by eye, which is why only one runs at a time. Off in a normal install.

- **Wireframe** draws 3D geometry as outlines. The HUD, menus and movies are left alone, since those are flat quads drawn with depth testing off. It shows model detail and where level-of-detail models swap as the camera moves.
- **SMAA edge detection** outlines what the anti-aliasing pass found, in red and green over a dimmed scene. The HUD stays untouched because the pass runs before the UI is drawn, which also makes this the quickest way to confirm the pre-UI injection is working.
- **SMAA blend weights** shows the following pass. Worth a look when the edges look right and the result does not.
- **Highlight scene target** tints the surface the anti-aliasing pass picked, at the moment it picks it. Green over the world but not the HUD means it found the right surface at the right point in the frame; green over the HUD as well means it ran too late.

In the file the setting is `[Debug] View`, one of `off`, `wireframe`, `smaa-edges`, `smaa-weights` or `scene-target`.

## Runtime memory manipulation

### TL;DR

The mod does not edit or replace the games' executable files. Its proxy DLLs are loaded alongside the game or launcher, make narrowly verified changes inside that running process, and forward everything else to the original Windows libraries. Those temporary changes disappear when the process exits.

### Safety

No game file is ever modified. Every change lives in the running process's memory and disappears when the process exits, and removing the mod's DLL leaves an entirely unmodified game behind. Each change is checked against the exact bytes it expects before it is made. A mismatch disables that one feature and leaves the game's original code running; nothing proceeds on a guess. An executable the mod does not recognize gets ordinary API forwarding and nothing else.

### Details

The 64-bit `d3d11.dll` is a proxy for the system D3D11 library. It exports the device-creation functions the game expects, then forwards them to `d3d11_proxy.dll` when one has been installed for chain-loading, or to the real `d3d11.dll` from the Windows system directory otherwise. This lets the mod observe device creation and install its rendering hooks without replacing the graphics implementation. Impersonating `d3d11.dll` rather than `dxgi.dll` is deliberate: system libraries, the real `d3d11.dll` among them, statically import functions from `dxgi.dll`, so a substitute `dxgi.dll` that misses one export stops the process from loading at all, while a `d3d11.dll` proxy only has to satisfy what the game itself asks for by name. The swap chain is intercepted through the device instead. The 32-bit `msimg32.dll` uses the same pattern for the stock launchers: it forwards `AlphaBlend` and `TransparentBlt` to the system MSIMG32 library while applying only the launcher-specific behavior described above.

Most executable changes are detours. After verifying the target function, the mod temporarily makes its code page writable, places a jump to a mod function at the entry point, restores the page protection, and flushes the processor's instruction cache. A trampoline preserves the displaced instructions and jumps back to the original function, so the mod can do work before or after normal engine behavior without replacing the routine. MinHook provides this mechanism for most game and D3D11 functions; a small in-project equivalent handles the few targets that need a fixed-size absolute jump. The launcher proxy similarly patches the stock launcher's verified entry point in memory, never in the executable on disk.

Other fixes change data rather than code. They read or update known fields in live engine objects, substitute D3D11 resources or views at API boundaries, and attach mod-owned companion resources to engine resources through D3D11 private data. Shared guarded-read helpers reject unavailable game memory before following reverse-engineered pointer chains, and mod-owned COM objects retain references for the lifetime in which the game or GPU can still observe them. These operations use per-build addresses and measured structure offsets; the mod does not search for approximate patterns and then write to whatever happens to match.

Every executable hook comes from a table keyed to a known game and build. Recognition starts with the exact executable name and `.text` size, and each target's complete expected prologue bytes are checked before any jump is installed. Launcher changes likewise require the expected process image and code signatures. A mismatch disables that feature and leaves the original path running, while an unknown executable receives only ordinary API forwarding.

All code patches, trampolines, cached pointers, and replacement resources exist only in the current process. Ending the game discards them with the rest of its memory, and removing the proxy DLL restores an entirely unmodified executable on the next launch. Configuration files written by the launcher are ordinary external settings and are the only intentional persistent changes.

## Hook boundaries

This section is about what the mod refuses to touch, which matters more than what it patches.

### Recognising the game

Nothing is patched until the running process is recognised as one of the six executables the mod was built against. Recognition is by exact executable name and by the size of the code section, and every game-code detour additionally compares the bytes at the target against a complete expected instruction prologue before writing anything. A build that is not one of the six, or one whose code has moved, fails that comparison and the mod installs nothing there. Where a fix needs several hooks, they are installed so that a partial install stays inert rather than leaving half a feature running.

The names, sizes, addresses and expected prologues all live in the tables in `src/`. They are not repeated here: the source is the copy that has to be right, and a second copy in prose would drift from it silently.

### The two builds of each game

Each game ships two executables. The launcher runs the English build when the language is set to English and the multilingual build for Japanese and both Chinese locales. They are separate compiles, so every address differs between them.

The multilingual addresses were not guessed from the English ones. Each was found by matching functions between the two builds on shared byte sequences, voting per function, and checking the match held in both directions before accepting it. Where a function sits in a virtual table, the slot was checked too. That method was validated by making it reproduce answers already known to be correct before it was trusted for unknown ones. At runtime none of that matters: the same prologue comparison applies to a multilingual build as to an English one, so a bad match fails closed.

Meruru's multilingual executable is wrapped by SteamStub on disk, so its code is encrypted in the file. Recognition and patching happen after the stub has decrypted the code in memory, which is why the same fingerprints work.

### Which executables are supported

| Game | Executable | SHA-256 |
|---|---|---|
| Rorona DX | `A11R_x64_Release_en.exe` | `2afd19db0cef3e3f0888fb62e02c9ca5929264ff5ee8c780af06213642988276` |
| Rorona DX (multilingual) | `A11R_x64_Release.exe` | `b6f8726df7d6cea3ffdeb171d669f8035df322552abdc90a4763523df2b4730d` |
| Totori DX | `A12V_x64_Release_en.exe` | `38c41df799b207786a11c08d6bf83cec8ac10414757f935311549f74474bfd90` |
| Totori DX (multilingual) | `A12V_x64_Release.exe` | `f8544d7b0ed22a223f080dbcdaa5f387287bccedc1f975d5ad9c304764f0aa6f` |
| Meruru DX | `A13V_x64_Release_EN.exe` | `d69cad45700457128cc8805ea3cf80dfaea0e155e6dfd2d1123277f4ebd7c19b` |
| Meruru DX (multilingual) | `A13V_x64_Release.exe` | `a39b854771fab1044d03c2da94afda84996eaa2ce9d60e85ca718f29b1700c73` |

The hashes are here because they are the one thing the source does not record: they name the exact builds this was developed against. A different hash is not necessarily unsupported, since recognition is by name, size and prologue rather than by hash, but it is a build nobody has checked.

### Per-game availability

Which games get which feature is decided in one place, the capability matrix in `src/game.cpp`. The running title is worked out from the executable name independently of the menu hooks, and every feature asks the matrix first. A title the matrix marks unsupported is off regardless of configuration, before any environment override or `arland-fix.ini` key is consulted. That ordering is deliberate: it means a setting can never switch on a fix for a game it was never mapped for.

### The two layers

The D3D11 synchronization work and the game-code detours are independent layers in one proxy DLL, and either can be switched off without the other. `ARLAND_MENU_FIX=0` skips the game-code detours while leaving synchronization active for a recognised executable. `ARLAND_ATLAS_CACHE=0` disables the font-atlas cache. `ARLAND_FRAME_ATLAS_CACHE=0` holds Rorona and Totori to the shorter queue-scoped lifetime, and setting it to `1` opts Meruru into the frame-scoped path for comparison.
