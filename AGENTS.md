# AGENTS.md

## Project scope

This repository releases a 64-bit `d3d11.dll` and a 32-bit settings-launcher `msimg32.dll` for the Steam releases of Atelier Rorona DX, Atelier Totori DX, and Atelier Meruru DX, covering both the English and the multilingual (Japanese/Chinese) executables. Keep the released implementation Arland-specific. Atelier Ayesha support is under investigation but must remain disabled until its atlas-only path is validated. Do not add support code for newer Atelier games; direct those users to upstream `atelier-sync-fix` instead.

The current tree contains:

- D3D11 CPU shadow-copy synchronization;
- coherent Map/Unmap handling with deferred shadow uploads;
- successful `.PSSG` path-validation caching for all three Arland games;
- a queue-scoped font-atlas read cache for all three games, extended to the complete menu-construction frame in Rorona, and extended across frames in Meruru while a conversation balloon is live (the conversation text-render cache);
- old-Arland render-target and viewport/scissor correction;
- old-Arland game-side 1440p/4K render-target and raster correction;
- signature-gated launcher mode injection and an optional INI resolution override;
- optional high-resolution shadow-map twins (`ShadowMultiplier`) and anisotropic filtering;
- SMAA anti-aliasing applied before UI composition, at a per-title draw boundary (Totori injects at a depth-state change, Rorona and Meruru at the scene render target);
- high-resolution UI text rendered from bundled scalable fonts, on the English builds;
- frame-rate-independent field movement in all three games and travel-map analog cursor movement in Totori and Meruru;
- Rorona battle-shadow restoration, battle-state tracking with a battle-end watchdog, and the optional cut-in shadow/dim handling (all three games, both builds), all in `src/battle_shadow_restore.cpp`;
- a per-game capability matrix (`src/game.cpp`) that centralizes feature availability and defaults;
- crash post-mortem logging and log rotation.

## Repository layout

- `src/` contains the project C++ source and headers, split by concern. The 64-bit game DLL: the D3D11 proxy layer is `sync_fix.cpp`, with the cut-in shadow feature carved into `battle_shadows.cpp` behind `sync_internal.h` (proxy vtable dispatch types in `d3d11_procs.h`); the executable-specific menu hooks are `menu_fix.cpp`, with the battle-shadow-restore subsystem carved into `battle_shadow_restore.cpp` behind `menu_internal.h`, both sharing the MinHook install helpers and per-game `Game` descriptor in `hook_util.{h,cpp}`; then the per-game capability matrix (`game.cpp`), `arland-fix.ini` config (`config.cpp`), SMAA post-process (`smaa.cpp`), high-resolution UI text (`font_hires.cpp`), crash logging (`crash_log.cpp`), guarded game-memory reads (`mem.h`), companion-file path building shared with the 32-bit DLL (`path_util.h`), the pipeline-state save and restore the post-process passes share (`pipeline_state.h`), and the DLL entry points (`main.cpp`). The 32-bit settings-launcher DLL is `launcher_proxy.cpp`. `.def` files export the DLL symbols.
- `vendor/minhook/` contains the unmodified vendored MinHook dependency and its license; `vendor/stb/` holds stb_truetype; `vendor/font/` holds the bundled replacement fonts (`.ttf`) and the per-glyph fallback face.
- `scripts/embed_font.py` compiles the vendored fonts into the DLL at build time; the generated sources live under the build directory and are not committed.
- `.github/workflows/build.yml` builds and publishes both Windows DLLs.
- `README.md` is the user-facing overview and installation guide for the drop-in defaults.
- `ADVANCED.md` documents the optional features and their `arland-fix.ini` configuration; user-facing configuration is documented exclusively through `arland-fix.ini` (environment switches are diagnostics and belong in `TECHNICAL.md` only).
- `BUILDING.md` contains the build instructions.
- `TECHNICAL.md` documents how each fix is implemented and why it works that way. Each section opens with a TL;DR that gives the short version of the explanation below it. Keep the language plain: a reader who knows how to program but not this engine should follow it. Measurements, test results and the record of how a conclusion was reached do not belong here.

Keep the root minimal. Packaged build output belongs below ignored `out/` and must not be committed. The day-to-day Linux build script uses `build64/` and `build32/` as intermediate Meson trees before packaging the distributable archive into `out/`.

## Build

Use the repository's day-to-day Linux cross-build script from the repository root:

```sh
./scripts/build_linux.sh
```

It compiles the intermediate binaries as `build64/d3d11.dll`, `build64/arland-fix-launcher.exe`, and `build32/msimg32.dll`, verifies their required proxy exports through `scripts/check_exports.py`, then packages the distributable archive as `out/arland-fix-<VERSION>.zip`. For native Windows and manual Meson commands, follow `BUILDING.md`. The export check requires `D3D11CreateDevice` at ordinal 22, `D3D11CreateDeviceAndSwapChain` at ordinal 23 and `D3D11On12CreateDevice` at ordinal 24 in the game DLL, and `AlphaBlend` plus `TransparentBlt` in the launcher proxy. The mod does nothing with D3D11-on-12; that export exists because a tool injected alongside this one can import the name statically, and the loader fails the whole process on a missing static import before any code runs.

## Validation

After making changes, run the relevant validation scripts, including `scripts/check_default_ini.py`, `scripts/check_launcher_contract.py`, and `scripts/check_core_contract.py`; `scripts/build_linux.sh` runs these checks after a successful build. When a contract check fails, determine whether the change intentionally altered the contract. If it did, update the contract check and its accompanying documentation as needed. If it did not, treat the failure as a possible regression and investigate it rather than weakening or bypassing the check.

## Implementation rules

Safety and stability decide anything these rules leave open. The mod exists to correct defects in these games, so fixing a real one is always worth doing. The constraints are that a change must not break what already works, and that how a fix is implemented matters as much as what it fixes.

This mod injects into a running game and writes to engine-owned memory from the game's own render thread. The failures that matter are a hook installed against a build it does not match, a write through a pointer that is no longer valid, and engine state the mod changes and does not put back. Guard against all three every time, including on paths believed unreachable.

Prefer evidence to belief, and prefer static evidence. An offset read out of a build's disassembly holds for every run of that build; a probe reports only the runs you took, and a run that looked fine is not proof that a case cannot occur. Get the static answer where one exists. Where it rests on something the disassembly does not settle, a runtime check against the behaviour being replaced is what closes the gap, and the two together are what a claim of stability should rest on.

A refactor, or a performance idea with no measured problem behind it, is not a fix, and does not earn the risk of touching working code. Speculative machinery for a case that has been measured not to occur is the same trade. When ranking possible work, rank it by this.

- Preserve exact executable-name, `.text`-size, and prologue gating for game-code hooks.
- Unknown executables must remain unmodified apart from normal system-D3D11 forwarding.
- Cache only successful `.PSSG` validation results. Do not cache failures, parsed UI graphs, or mutable resource objects.
- Keep atlas snapshots inside the verified synchronous queue-drain lifetime in Meruru. Rorona and Totori may retain verified text-renderer snapshots until the next `Present`; invalidate a texture on any unmatched real lock and never retain snapshots across frames. That invalidation underpins both lifetimes and must never be gated on either of them.
- Internal D3D11 calls that touch a slot the mod hooks must go through the original entry points (`getContextProcs(ctx)->Fn(ctx, ...)`) so nothing recurses through the mod's own hooks; state queries on unhooked slots have no captured original and may use the interface directly. `gateHoldAtDraw` in `battle_shadows.cpp` is the reference example: its `UpdateSubresource` goes through the originals, its Get-queries do not need to. The mod's own present-time post-process passes (`ssaaDownscale`, `smaaRun`) are the exception and issue through the hooked context. The reason on record for that was multisampling, which was removed in 0.14: binding an SRV was what resolved a multisample twin into its host before the pass sampled it. Leave the passes on the hooked context until someone establishes what it still contributes; that has not been measured since the removal.
- Redirect staging shadows only on the immediate context. Flush before GPU consumers and before executing deferred command lists.
- Preserve per-resource/per-subresource lifetime tracking and COM reference ownership.
- Gate experimental behavior until it has passed clean-text, repeated-menu, and multiple-game validation.
- Keep launcher mutations in memory and signature-gated; never modify or redistribute Koei Tecmo executables.

## Configuration options

`default.ini` ships in the release archive and repeats defaults that really live in `src/config.cpp` (and `src/game.cpp`'s feature table). When you add, rename, remove or re-default an option, update `default.ini` and `ADVANCED.md` in the same change.

`scripts/check_default_ini.py` enforces this and runs in CI: it checks that every option the code reads is documented, that nothing documented is unread, and that the literal defaults agree. If an option is deliberately kept out of `default.ini`, or its default cannot be read from a single call site (the per-game cut-in keys), add it to the allowlists at the top of that script rather than dropping the check.

## Documentation style

Do not hard-wrap markdown. Write one line per paragraph and let the editor or viewer wrap it; the same applies to markdown embedded elsewhere, such as the release notes generated in `.github/workflows/build.yml`. Line breaks stay meaningful only where they already are: fenced code blocks, tables, list items (one line each), headings and blockquotes.

Hard-wrapped prose makes diffs noisy — a reworded sentence reflows every line after it — and the wrap width never matches everyone's editor anyway. INI and source comments are the exception: keep wrapping those to the surrounding code width.

## Comments in the source

A comment earns its place by saying something the code cannot: an intent, a constraint, a fact about the game, an alternative that was tried and rejected. Restating the line below it is worse than useless, because it is one more thing that has to be kept true.

Write one where a reader would otherwise be stuck, or would guess wrong:

- What a file is for, at the top, where the filename does not already say it.
- For a fix: what the game does wrong, and why the correction takes the form it does rather than the one a reader would reach for first.
- Where an offset, signature, RVA or struct layout was derived from, so it can be checked instead of re-derived.
- What a decision rests on. If that is a measurement, name it. If it is an assumption, say so, and say what would break it.

Write in the present tense about what the code does now. Do not narrate the repository's history; git holds that, and a comment about what used to be there reads as current the moment someone skims it. The one exception is code deliberately removed that a reader would otherwise put back: say what went and why, once, at the place they would reintroduce it.

When changing existing code, change the comments the change makes wrong and leave the rest alone. A fix is not an invitation to rewrite the commentary around it, and a diff that reworks neighbouring prose hides what actually changed.

## Attribution and documentation

Philip Rebohle created the original `atelier-sync-fix` synchronization implementation. TellowKrinkle supplied the earlier Map/Unmap coherence work and the old-Arland resolution correction. Yuri Hime's Atelier Graphics Tweak and the linked Steam investigations are prior work identifying the broad font-atlas transfer problem; AGT's unsafe upload-suppression implementation is not included. Nico, the author of this repository, led the Arland menu-hitch investigation, the `.PSSG` and bounded atlas-read cache research, and the launcher analysis. MinHook is by Tsuda Kageyu and contributors.

Maintain these distinctions in source comments, `README.md`, `TECHNICAL.md`, and `LICENSE`. Do not imply that the menu investigation created the original synchronization technique.

Documentation must describe the current repository state. Do not narrate it using a specific release or version number, and do not hard-wrap Markdown prose.

Avoid overusing em-dashes in source comments and documentation. Prefer commas, parentheses, or separate sentences; occasional correct use is fine, but do not lean on them.

`TECHNICAL.md` and the code must stay in sync: every shipped fix and feature has a section in `TECHNICAL.md` describing how it works, and any change that adds, removes, or alters a fix's behavior or defaults amends `TECHNICAL.md` in the same change. A fix that exists only in code is undocumented and incomplete. The table of contents at the top lists every section, so a new section is added there in the same change.
