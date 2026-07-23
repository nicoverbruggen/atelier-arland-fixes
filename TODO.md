# TODO

This file tracks work in progress and ideas under consideration. Items here are not promises and are not part of the currently documented feature set.

## Applies to all games

- Add a configurable framerate lock/cap through `arland-fix.ini` (e.g. a `[Rendering]` field such as `FramerateCap`): let the user cap or lock the game to a chosen frame rate.
- Identify where the game or the mod is locked to a 30 fps assumption — the code path(s) that assume or impose 30 fps. This informs the configurable framerate cap above.
- Supersampling factor: a new `[Rendering]` field (e.g. `SupersamplingFactor` / `ARLAND_SUPERSAMPLING`) independent of `Width`/`Height`. `Width`/`Height` continue to set the actual output/window size; the supersampling factor renders the scene internally at `factor ×` that output resolution and downscales to it. Example: on a 1080p display, `SupersamplingFactor=2` renders at 4K internally and resolves down to 1080p (SSAA), without requiring a 4K output. Default 1 (no supersampling); orthogonal to the existing MSAA and direct-resolution paths.
- Investigate sharper font outlines through a font-only atlas or sampling improvement without sharpening other UI textures.
- Add a signature-gated Borderless checkbox to the settings launcher and implement the corresponding game-window mode.
- Evaluate anisotropic filtering and configurable texture LOD bias, including UI and shimmering regressions.
- Investigate the jagged aliasing on thin costume trim, such as the frilled lace on character sleeves and collars (observed on Meruru's dress; the mechanism applies to character models in every game). Confirm first whether the trim is alpha-tested or alpha-blended, since the fix differs: for alpha-test, enable alpha-to-coverage when MSAA is active so the cutout edges get multisampled coverage; for alpha-blend shimmer, anisotropic filtering and a texture LOD bias are the likely levers. Relates to the anisotropic-filtering and LOD-bias item above.
- Investigate selective high-resolution texture replacement or scaling without touching dynamic font atlases, render targets, videos, or other mutable resources.
- Revisit the remaining per-record UI construction and layout cost only if a safe optimization is identified; the residual Synthesis work includes eager construction of hidden recipe entries and is intentionally left unchanged for now.
- Keep rendering enhancements optional until their performance and visual behavior are validated across the trilogy.

## Atelier Rorona

- Investigate a cut-in shadow correctness glitch (also occurs in Meruru — the cut-in shadow code is shared between the two, so fix once). The restored shadows themselves are correct, but during the action cut-ins the engine juggles the non-focus characters around: sometimes it hides them, and sometimes it moves them off-position (they appear above the cut-in scene). The restored shadow reception still projects those casters onto the ground, so ground shadows appear for characters that are invisible or not where the shadow is. The problem is a caster shadowing the floor while its character has been repositioned or hidden for the close-up — likely the same root cause as the Meruru pre-victory-screen artifact below. Worth a thorough investigation: determine how the engine repositions/hides the non-focus battlers during cut-ins, and gate the restored shadow reception (or the individual caster's contribution) so only actually-visible, on-position characters cast cut-in shadows.
- Check whether the animated-portrait (BUC) conversation slowdown also occurs in Rorona. The same `Contrast` + global mechanism is structurally present (RVAs not yet pinned), but the slowdown has not been reproduced in Rorona yet.

## Atelier Totori (after the codebase reorganization)

Totori is the structural outlier of the trilogy; Rorona and Meruru share code paths
Totori does not, so its shadow features need their own porting rather than reuse.

- Restore battle shadows while fighting on Totori — they are broken (not native, contrary to an earlier assumption). This needs Totori's own caster restoration, analogous to the Rorona DX v0.3 work, plus its cinematic battle-state detection. Do this once the battle-shadow code is split into its own module.
- Check whether Totori also loses shadows during the action cut-ins the way Rorona and Meruru do (the receiver's reception gate). If so, port the cut-in gate/dim fix, which depends on locating Totori's cinematic battle states (the same detection the fighting-shadow restoration needs).
- Verify the `ShadowMultiplier` (higher-resolution shadow map) path on Totori and fix it if broken. The twin/redirect plumbing has only been validated on Rorona and Meruru so far.
- Check whether the animated-portrait (BUC) conversation slowdown occurs in Totori. Totori uses a different `Contrast` implementation (`ContrastEvent` / `FieldMapContrastSiren`), so it needs its own investigation and would not share the Meruru fix.

## Atelier Meruru

- Fix the animated-portrait (BUC) conversation slowdown. Observed only during field-map conversations (the `nspFM` field-map talk/message/event states), not in battle or menus — reproduce and measure it there. Runtime testing during a reproduced slowdown disproved the leading hypothesis: the `Contrast` enable-global (EN RVA `0x1089C60`) reads 0 every frame even when the effect is enabled, so it is not a stuck set-without-restore global, and the `Contrast::apply` clear-global fix was inert and has been removed. It is not a framerate/sync-interval cap either. The persistent per-frame cost is elsewhere — most likely the animated-portrait atlas/face-node rework (the same slow CPU-side texture-transfer path the mod fights for menus). Next: instrument the D3D11 layer (per-frame draw-call count, `Map`/`UpdateSubresource` bytes, RT binds, texture creations) during the slowdown to locate the real cost. Address map and the disproven hypothesis are in the project memory.
- Fix a cut-in shadow artifact just before the victory screen: for a few frames, shadows are visible with the characters apparently positioned up in the sky. This is very likely a specific instance of the cut-in character-juggling shadow glitch (see the Atelier Rorona section) — non-focus characters repositioned above the scene while their casters still shadow the floor. Investigate the cause and suppress it during the victory/result transition. (This is the "verify victory screens" item.)

## Beyond the Arland trilogy

- Add an atlas-only fix for Atelier Ayesha DX without enabling the Arland-specific `.PSSG` cache there.
- Rename repository: since the launcher fix and MSAA/resolution fixes could also feasibly apply to the Dusk trilogy, evaluate whether to rename the repo to `atelier-classic-fixes` and expand the scope of the existing work, or to create a separate `atelier-dusk-fixes` repo instead.
