# TODO

This file tracks work in progress and ideas under consideration. Items here are not promises and are not part of the currently documented feature set.

## Rendering enhancements under consideration

- Battle-character shadows: **overview SHIPPED in v0.3** (default-on for the recognized Rorona executables — both language builds since v0.4; Meruru casts these natively). Cut-in / action-selection close-up: **SHIPPED** — Rorona in v0.4, Meruru in v0.5. The earlier caster-injection approach was abandoned; the actual cause was the receiver shader's shadow-*reception* gate (the cut-in dims the scene light below the shader's `diffuse > 0.75` threshold, which switches reception off for the whole scene), so the fix holds that light up during cinematic states and the engine's own casters shadow the cut-in floor. Configured via `[Battle]` `BattleShadows`/`BattleCutInShadows`/`BattleCutInDimming` (env overrides `ARLAND_BATTLE_SHADOWS`/`ARLAND_CUTIN_SHADOWS`/`ARLAND_CUTIN_DIMMING`). Totori is not yet covered.
- Configurable shadow-map resolution (`[Battle] ShadowResolution`): **IN PROGRESS**. Resizing the engine's own 1024² shadow maps in place caused scene-dependent crashes (the engine appears to assume the 1024 size in its own memory bookkeeping). Being re-architected to allocate separate high-resolution maps the mod owns and redirect the caster render, the A→B copy, and the receiver sample to them, leaving the engine's textures untouched. If that proves unviable, cap it at 2048 as experimental or drop it.
- Supersampling factor: add a **new** `[Rendering]` field (e.g. `SupersamplingFactor` / `ARLAND_SUPERSAMPLING`) that is independent of `Width`/`Height`. `Width`/`Height` continue to set the actual output/window size; the supersampling factor renders the scene internally at `factor ×` that output resolution and downscales to it. Example: on a 1080p display, `SupersamplingFactor=2` renders at 4K internally and resolves down to 1080p (SSAA), without requiring a 4K output. Default 1 (no supersampling); interacts with, but is orthogonal to, the existing MSAA and direct-resolution paths.
- Investigate sharper font outlines through a font-only atlas or sampling improvement without sharpening other UI textures.
- Add a signature-gated Borderless checkbox to the settings launcher and implement the corresponding game-window mode.
- Evaluate anisotropic filtering and configurable texture LOD bias, including UI and shimmering regressions.
- Investigate selective high-resolution texture replacement or scaling without touching dynamic font atlases, render targets, videos, or other mutable resources.
- Keep rendering enhancements optional until their performance and visual behavior are validated across the trilogy.

## Performance work

- Revisit the remaining per-record UI construction and layout cost only if a safe optimization is identified; the residual Synthesis work includes eager construction of hidden recipe entries and is intentionally left unchanged for now.
- Add an atlas-only fix for Atelier Ayesha DX without enabling the Arland-specific `.PSSG` cache there.

## Rename repository

Since it seems that the launcher fix and MSAA/resolution fixes could also feasibly apply to the Dusk trilogy, perhaps it's worth evaluating if the repo needs to be renamed to `atelier-classic-fixes` and the scope of the existing work expanded, or whether a separate `atelier-dusk-fixes` repo needs to be created instead.
