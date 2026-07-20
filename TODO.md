# TODO

This file tracks work in progress and ideas under consideration. Items here are not promises and are not part of the currently documented feature set.

## Rendering enhancements under consideration

- Battle-character shadows: **overview SHIPPED in v0.3** (default-on for the recognized Rorona executable; `ARLAND_BATTLE_SHADOWS=0` disables). Cut-in / action-selection close-up: **IN PROGRESS**, not part of v0.3 — the original game never implemented that shadow submission on any platform. Current state (REPORT §30c–§30f): shadow VS constant layout recovered from DXBC, light-VP derivation validated in-engine (err≈6e-8), deferred one-frame injection into the shadow pass implemented and hardened, but still no visible shadow; next probes in §30f (receiver-side copy of the depth map, depth-cull of injected draws, alpha-test PS). The probe-only code lives outside this release: `../cutin-probe-sync_fix.patch` (apply to `src/sync_fix.cpp`). Then: strip dead diagnostic scaffolding, promote to a clean opt-in feature, configurable shadow-map resolution.
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
