# AGENTS.md

## Project scope

This repository builds one `d3d11.dll` for the English Steam releases of Atelier Rorona DX, Atelier Totori DX, and Atelier Meruru DX. Keep the implementation Arland-specific. Do not add support code for newer Atelier games; direct those users to upstream `atelier-sync-fix` instead.

The current tree contains:

- D3D11 CPU shadow-copy synchronization;
- coherent Map/Unmap handling with deferred shadow uploads;
- successful `.PSSG` path-validation caching for all three Arland games;
- a queue-scoped font-atlas read cache for all three games.

Consult `TODO.md` for work in progress and features under consideration.

## Repository layout

- `src/` contains project C++ source, headers, and `d3d11.def`.
- `vendor/minhook/` contains the unmodified vendored MinHook dependency and its license.
- `.github/workflows/build.yml` builds the Windows DLL.
- `README.md` is the user-facing overview and installation guide.
- `TECHNICAL.md` documents implementation details, evidence, and provenance.
- `TODO.md` is the only roadmap and work-in-progress document.

Keep the root minimal. Build output belongs in an ignored build directory and must not be committed.

## Build

Windows:

```powershell
meson setup build --buildtype release
meson compile -C build
```

Linux cross-build:

```sh
meson setup build --cross-file build-win64.txt --buildtype release
ninja -C build
```

The expected output is `build/d3d11.dll`. Verify that it exports `D3D11CreateDevice` at ordinal 22 and `D3D11CreateDeviceAndSwapChain` at ordinal 23.

## Implementation rules

- Preserve exact executable-name, `.text`-size, and prologue gating for game-code hooks.
- Unknown executables must remain unmodified apart from normal system-D3D11 forwarding.
- Cache only successful `.PSSG` validation results. Do not cache failures, parsed UI graphs, or mutable resource objects.
- Keep atlas snapshots inside the verified synchronous queue-drain lifetime.
- Internal D3D11 operations must call original entry points and must not recurse through hooks.
- Redirect staging shadows only on the immediate context. Flush before GPU consumers and before executing deferred command lists.
- Preserve per-resource/per-subresource lifetime tracking and COM reference ownership.
- Gate experimental behavior until it has passed clean-text, repeated-menu, and multiple-game validation.

## Attribution and documentation

Philip Rebohle created the original `atelier-sync-fix` synchronization implementation. TellowKrinkle supplied the earlier Map/Unmap coherence work and later rendering enhancements. Nico, the author of this repository, led the Arland menu-hitch investigation and the `.PSSG` and atlas-cache research. MinHook is by Tsuda Kageyu and contributors.

Maintain these distinctions in source comments, `README.md`, `TECHNICAL.md`, and `LICENSE`. Do not imply that the menu investigation created the original synchronization technique.

Documentation must describe the current repository state. Do not narrate it using a specific release or version number, and do not hard-wrap Markdown prose.
