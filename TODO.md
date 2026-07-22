# TODO

This file tracks work in progress and ideas under consideration. Items here are not promises and are not part of the currently documented feature set.

## Rendering enhancements under consideration

- Supersampling factor: a new `[Rendering]` field (e.g. `SupersamplingFactor` / `ARLAND_SUPERSAMPLING`) independent of `Width`/`Height`. `Width`/`Height` continue to set the actual output/window size; the supersampling factor renders the scene internally at `factor ×` that output resolution and downscales to it. Example: on a 1080p display, `SupersamplingFactor=2` renders at 4K internally and resolves down to 1080p (SSAA), without requiring a 4K output. Default 1 (no supersampling); orthogonal to the existing MSAA and direct-resolution paths.
- Investigate sharper font outlines through a font-only atlas or sampling improvement without sharpening other UI textures.
- Add a signature-gated Borderless checkbox to the settings launcher and implement the corresponding game-window mode.
- Evaluate anisotropic filtering and configurable texture LOD bias, including UI and shimmering regressions.
- Investigate the jagged aliasing on thin costume trim, such as the frilled lace on character sleeves and collars (observed on Meruru's dress). Confirm first whether the trim is alpha-tested or alpha-blended, since the fix differs: for alpha-test, enable alpha-to-coverage when MSAA is active so the cutout edges get multisampled coverage; for alpha-blend shimmer, anisotropic filtering and a texture LOD bias are the likely levers. Relates to the anisotropic-filtering and LOD-bias item above.
- Investigate selective high-resolution texture replacement or scaling without touching dynamic font atlases, render targets, videos, or other mutable resources.
- Keep rendering enhancements optional until their performance and visual behavior are validated across the trilogy.

## Atelier Totori (after the codebase reorganization)

Totori is the structural outlier of the trilogy; Rorona and Meruru share code paths
Totori does not, so its shadow features need their own porting rather than reuse.

- Restore battle shadows while fighting on Totori — they are broken (not native, contrary to an earlier assumption). This needs Totori's own caster restoration, analogous to the Rorona DX v0.3 work, plus its cinematic battle-state detection. Do this once the battle-shadow code is split into its own module.
- Check whether Totori also loses shadows during the action cut-ins the way Rorona and Meruru do (the receiver's reception gate). If so, port the cut-in gate/dim fix, which depends on locating Totori's cinematic battle states (the same detection the fighting-shadow restoration needs).
- Verify the `ShadowMultiplier` (higher-resolution shadow map) path on Totori and fix it if broken. The twin/redirect plumbing has only been validated on Rorona and Meruru so far.

## Atelier Meruru

- Fix a cut-in shadow artifact just before the victory screen: for a few frames, shadows are visible with the characters apparently positioned up in the sky. Investigate the cause and suppress it during the victory/result transition. (This is the "verify victory screens" item.)
- Fix the Meruru framerate drop. Observed facts only: the framerate drops and then stays low; the user first noticed it when a conversation with animated character portraits animated in; it reads more like a per-frame frame drop than a clean 60→30 halving, but that is not confirmed. The cause is not yet known — do not assume a mechanism. Under investigation via a static study of Meruru's conversation/ADV state in the binary and via runtime logging (the deployed build logs any change in the `Present` sync interval, which is one signal among several, not a presumed cause). Confirm the mechanism with a live capture before choosing a fix.

## Feature validation

A complete per-game validation checklist. Mark each cell as validated once the
feature is confirmed working in that game. Legend: ⬜ to validate · ✔ validated ·
— not applicable / not present.

| Feature | Rorona | Totori | Meruru |
| --- | :---: | :---: | :---: |
| Faster menus + sync fix + text-corruption fix | ⬜ | ⬜ | ⬜ |
| Atlas read cache | ⬜ | ⬜ | ⬜ |
| Frame-scoped atlas cache | ⬜ | — | — |
| Direct 2560×1440 / 3840×2160 rendering | ⬜ | ⬜ | ⬜ |
| MSAA (optional) | ⬜ | ⬜ | ⬜ |
| Shadow multiplier (optional) | ⬜ | ⬜ | ⬜ |
| Battle shadows while fighting | ⬜ | — | ⬜ |
| Cut-in shadows | ⬜ | — | ⬜ |
| Cut-in brightness hold | ⬜ | — | ⬜ |
| Cutscene framerate restore | — | — | ⬜ |

## Performance work

- Revisit the remaining per-record UI construction and layout cost only if a safe optimization is identified; the residual Synthesis work includes eager construction of hidden recipe entries and is intentionally left unchanged for now.
- Add an atlas-only fix for Atelier Ayesha DX without enabling the Arland-specific `.PSSG` cache there.

## Rename repository

Since it seems that the launcher fix and MSAA/resolution fixes could also feasibly apply to the Dusk trilogy, perhaps it's worth evaluating if the repo needs to be renamed to `atelier-classic-fixes` and the scope of the existing work expanded, or whether a separate `atelier-dusk-fixes` repo needs to be created instead.
