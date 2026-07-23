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
- optional MSAA and optional high-resolution shadow-map twins (`ShadowMultiplier`);
- Rorona battle-shadow restoration, battle-state tracking with a battle-end watchdog, and the optional cut-in shadow/dim handling for Rorona and Meruru;
- a per-game capability matrix (`src/game.cpp`) that centralizes feature availability and defaults;
- crash post-mortem logging and log rotation.

Consult `TODO.md` for work in progress and features under consideration.

## Repository layout

- `src/` contains project C++ source, headers, and module-definition files.
- `vendor/minhook/` contains the unmodified vendored MinHook dependency and its license.
- `.github/workflows/build.yml` builds and publishes both Windows DLLs.
- `README.md` is the user-facing overview and installation guide for the drop-in defaults.
- `ADVANCED.md` documents the optional features and their `arland-fix.ini` configuration; user-facing configuration is documented exclusively through `arland-fix.ini` (environment switches are diagnostics and belong in `TECHNICAL.md` only).
- `BUILDING.md` contains the build instructions.
- `TECHNICAL.md` documents implementation details, evidence, and provenance.
- `TODO.md` is the only roadmap and work-in-progress document.

Keep the root minimal. Build output belongs below ignored `builds/` and must not be committed.

## Build

Windows:

```powershell
meson setup builds/release-x64 --buildtype release
meson compile -C builds/release-x64
# Run from an x86 Native Tools shell:
meson setup builds/release-x86 --buildtype release
meson compile -C builds/release-x86
```

Linux cross-build:

```sh
meson setup builds/release-x64 --cross-file build-win64.txt --buildtype release
ninja -C builds/release-x64
meson setup builds/release-x86 --cross-file build-win32.txt --buildtype release
ninja -C builds/release-x86
```

The expected outputs are `builds/release-x64/d3d11.dll` and `builds/release-x86/msimg32.dll`. Verify that the game DLL exports `D3D11CreateDevice` at ordinal 22 and `D3D11CreateDeviceAndSwapChain` at ordinal 23, and that the launcher DLL exports `AlphaBlend` and `TransparentBlt`.

## Implementation rules

- Preserve exact executable-name, `.text`-size, and prologue gating for game-code hooks.
- Unknown executables must remain unmodified apart from normal system-D3D11 forwarding.
- Cache only successful `.PSSG` validation results. Do not cache failures, parsed UI graphs, or mutable resource objects.
- Keep atlas snapshots inside the verified synchronous queue-drain lifetime in Totori and Meruru. Rorona may retain verified text-renderer snapshots until the next `Present`; invalidate a texture on any unmatched real lock and never retain snapshots across frames.
- Internal D3D11 operations must call original entry points and must not recurse through hooks.
- Redirect staging shadows only on the immediate context. Flush before GPU consumers and before executing deferred command lists.
- Preserve per-resource/per-subresource lifetime tracking and COM reference ownership.
- Gate experimental behavior until it has passed clean-text, repeated-menu, and multiple-game validation.
- Keep launcher mutations in memory and signature-gated; never modify or redistribute Koei Tecmo executables.

## Attribution and documentation

Philip Rebohle created the original `atelier-sync-fix` synchronization implementation. TellowKrinkle supplied the earlier Map/Unmap coherence work and the old-Arland resolution correction. Yuri Hime's Atelier Graphics Tweak and the linked Steam investigations are prior work identifying the broad font-atlas transfer problem; AGT's unsafe upload-suppression implementation is not included. Nico, the author of this repository, led the Arland menu-hitch investigation, the `.PSSG` and bounded atlas-read cache research, and the launcher analysis. MinHook is by Tsuda Kageyu and contributors.

Maintain these distinctions in source comments, `README.md`, `TECHNICAL.md`, and `LICENSE`. Do not imply that the menu investigation created the original synchronization technique.

Documentation must describe the current repository state. Do not narrate it using a specific release or version number, and do not hard-wrap Markdown prose.

`TECHNICAL.md` and the code must stay in sync: every shipped fix and feature has a section in `TECHNICAL.md` describing its mechanism, and any change that adds, removes, or alters a fix's behavior or defaults amends `TECHNICAL.md` in the same change. A fix that exists only in code is undocumented and incomplete.
