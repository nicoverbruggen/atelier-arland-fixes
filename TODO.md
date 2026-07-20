# TODO

This file tracks work in progress and ideas under consideration. Items here are not promises and are not part of the currently documented feature set.

## Rendering enhancements under consideration

- Fix the blurred-dialog composite at 1440p and 4K; it currently occupies a 1920x1080 region in the upper-left of the output.
- Complete the core MSAA implementation and assess it independently in Rorona, Totori, and Meruru before documenting it as supported.
- Investigate sharper font outlines through a font-only atlas or sampling improvement without sharpening other UI textures.
- Add a signature-gated Borderless checkbox to the settings launcher and implement the corresponding game-window mode.
- Evaluate anisotropic filtering and configurable texture LOD bias, including UI and shimmering regressions.
- Investigate selective high-resolution texture replacement or scaling without touching dynamic font atlases, render targets, videos, or other mutable resources.
- Keep rendering enhancements optional until their performance and visual behavior are validated across the trilogy.

## Performance work

- Reduce the remaining per-record UI construction and layout cost after the path and atlas caches.
- Add an atlas-only fix for Atelier Ayesha DX without enabling the Arland-specific `.PSSG` cache there.
