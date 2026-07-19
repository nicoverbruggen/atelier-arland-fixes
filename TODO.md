# TODO

This file tracks work in progress and ideas under consideration. Items here are not promises and are not part of the currently documented feature set.

## Resolution support

- Port and validate the old-Arland render-target and viewport/scissor correction for Rorona, Totori, and Meruru.
- Add a signature-gated patcher for each game's `ArlandDXEnv.exe` that adds 2560×1440 to the launcher while preserving its existing 3840×2160 option.
- Make launcher patching idempotent, create a backup before modification, and support restoration.
- Test launcher and in-game resolution behavior on both Windows and Wine/Proton.

## Rendering enhancements under consideration

- Evaluate MSAA and the associated shader/sample-rate conversion separately in every Arland game.
- Evaluate anisotropic filtering and configurable texture LOD bias, including UI and shimmering regressions.
- Investigate selective high-resolution texture replacement or scaling without touching dynamic font atlases, render targets, videos, or other mutable resources.
- Keep rendering enhancements optional until their performance and visual behavior are validated across the trilogy.

## Menu-fix coverage

- Port the already validated queue-scoped atlas-read optimization to Meruru's release executable.
- Locate and validate Totori's queue-drain, text-render, atlas-lock, and atlas-unlock equivalents before enabling the same optimization there.
- Exercise repeated opens of Status, Quest, Container, Basket, crafting, and gathering interfaces in each supported game.

## Release validation

- Test the unified DLL through longer gameplay sessions on native Windows.
- Measure the remaining menu latency and memory overhead on Steam Deck.
- Confirm clean uninstall and fallback behavior when the DLL is removed.
