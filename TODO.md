# TODO

This file tracks work in progress and ideas under consideration. Items here are not promises and are not part of the currently documented feature set.

## Rendering enhancements under consideration

- Supersampling factor: a new `[Rendering]` field (e.g. `SupersamplingFactor` / `ARLAND_SUPERSAMPLING`) independent of `Width`/`Height`. `Width`/`Height` continue to set the actual output/window size; the supersampling factor renders the scene internally at `factor ×` that output resolution and downscales to it. Example: on a 1080p display, `SupersamplingFactor=2` renders at 4K internally and resolves down to 1080p (SSAA), without requiring a 4K output. Default 1 (no supersampling); orthogonal to the existing MSAA and direct-resolution paths.
- Add a configurable framerate lock/cap through `arland-fix.ini` (e.g. a `[Rendering]` field such as `FramerateCap`): let the user cap or lock the game to a chosen frame rate.
- Identify where the game or the mod is locked to a 30 fps assumption — the code path(s) that assume or impose 30 fps. This informs both the configurable framerate cap above and the Meruru framerate drop.
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
- Fix the Meruru framerate drop during animated-portrait conversations (the game's "BUC" bust-up-conversation mode). Cause identified statically (verified; the exact conversation-exit persistence still needs one live capture to confirm): the mode owns a full-screen `Contrast` post-process enabled through a global (EN build RVA `0x1089C60`) that is set on enter and cleared only when the mode object is destroyed, so a conversation that ends by hiding rather than destroying leaves the full-screen pass running every frame. It is not a framerate/sync-interval cap (ruled out for this path). Confirm via the D3D11 layer (read `gameBase+0x1089C60` each Present; check it stays non-zero and an extra full-screen pass persists after the portrait leaves), then apply the self-healing fix: detour `Contrast::apply` (RVA `0x1f06a0`) to zero the global once the effect is disabled. Present in both Meruru builds (multilingual global `0x10E7040`) and in Rorona; Totori uses a different `Contrast` implementation and needs its own pass. Full address map is in the project memory.

## Performance work

- Revisit the remaining per-record UI construction and layout cost only if a safe optimization is identified; the residual Synthesis work includes eager construction of hidden recipe entries and is intentionally left unchanged for now.
- Add an atlas-only fix for Atelier Ayesha DX without enabling the Arland-specific `.PSSG` cache there.

## Rename repository

Since it seems that the launcher fix and MSAA/resolution fixes could also feasibly apply to the Dusk trilogy, perhaps it's worth evaluating if the repo needs to be renamed to `atelier-classic-fixes` and the scope of the existing work expanded, or whether a separate `atelier-dusk-fixes` repo needs to be created instead.
