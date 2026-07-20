# TODO

This file tracks work in progress and ideas under consideration. Items here are not promises and are not part of the currently documented feature set.

## Rendering enhancements under consideration

- Battle-character shadows: **overview SOLVED** (opt-in `ARLAND_BATTLE_SHADOWS=1 ARLAND_BATTLE_SHADOW_TARGET=battle`; see REPORT §28). Follow-ups: (a) fix the attack cut-in, a separate receiver/projection-side problem needing D3D-level analysis of the cut-in shadow-SRV sampling; (b) remove the dead diagnostic scaffolding (constructor collection, `0x39cfd0` scoped-swap, deferred-queue) and promote the registration+publish path to a clean opt-in feature; (c) configurable shadow-map resolution only after (a).
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
