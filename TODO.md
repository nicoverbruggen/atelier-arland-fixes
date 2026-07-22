# TODO

This file tracks work in progress and ideas under consideration. Items here are not promises and are not part of the currently documented feature set.

## Rendering enhancements under consideration

- Check whether Atelier Totori DX loses its battle shadows during the action cut-ins the way Rorona and Meruru did (the receiver's reception gate). Totori already casts ordinary battle shadows, so no caster restoration is needed; if its cut-ins are affected, port the cut-in gate/dim fix by locating Totori's cinematic battle states. If Totori's cut-ins already show shadows, no work is needed.
- Supersampling factor: a new `[Rendering]` field (e.g. `SupersamplingFactor` / `ARLAND_SUPERSAMPLING`) independent of `Width`/`Height`. `Width`/`Height` continue to set the actual output/window size; the supersampling factor renders the scene internally at `factor ×` that output resolution and downscales to it. Example: on a 1080p display, `SupersamplingFactor=2` renders at 4K internally and resolves down to 1080p (SSAA), without requiring a 4K output. Default 1 (no supersampling); orthogonal to the existing MSAA and direct-resolution paths.
- Investigate sharper font outlines through a font-only atlas or sampling improvement without sharpening other UI textures.
- Add a signature-gated Borderless checkbox to the settings launcher and implement the corresponding game-window mode.
- Evaluate anisotropic filtering and configurable texture LOD bias, including UI and shimmering regressions.
- Investigate the jagged aliasing on thin costume trim, such as the frilled lace on character sleeves and collars. The hard stair-stepped edges look like alpha-tested (cutout) transparency, which standard MSAA does not smooth. Confirm first whether the trim is alpha-tested or alpha-blended, since the fix differs: for alpha-test, enable alpha-to-coverage when MSAA is active so the cutout edges get multisampled coverage; for alpha-blend shimmer, anisotropic filtering and a texture LOD bias are the likely levers. Relates to the anisotropic-filtering and LOD-bias item above.
- Investigate selective high-resolution texture replacement or scaling without touching dynamic font atlases, render targets, videos, or other mutable resources.
- Keep rendering enhancements optional until their performance and visual behavior are validated across the trilogy.

## Performance work

- Revisit the remaining per-record UI construction and layout cost only if a safe optimization is identified; the residual Synthesis work includes eager construction of hidden recipe entries and is intentionally left unchanged for now.
- Add an atlas-only fix for Atelier Ayesha DX without enabling the Arland-specific `.PSSG` cache there.

## Rename repository

Since it seems that the launcher fix and MSAA/resolution fixes could also feasibly apply to the Dusk trilogy, perhaps it's worth evaluating if the repo needs to be renamed to `atelier-classic-fixes` and the scope of the existing work expanded, or whether a separate `atelier-dusk-fixes` repo needs to be created instead.
