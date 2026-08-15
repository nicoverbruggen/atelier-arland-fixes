// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

// SMAA (Enhanced Subpixel Morphological Anti-Aliasing, Jimenez et al.) as a
// post-process over the finished 3D scene, before the games draw their UI.
// SMAA works on the finished image, so it smooths edges that multisampling
// cannot -- texture-interior and alpha-test edges -- as well as ordinary
// silhouettes. It runs the standard three
// passes (edge detection -> blending-weight calculation -> neighborhood
// blending) with the reference shader and its two precomputed lookup textures.
//
// The reference shader and the AreaTex/SearchTex data are vendored under
// vendor/smaa/ (MIT). Shaders are compiled at runtime via d3dcompiler, matching
// the mod's existing runtime-shader pattern.
namespace atfix {

// Whether SMAA post-processing is enabled ([Rendering] SMAA / ARLAND_SMAA).
// Load the shader compiler from the frame tick rather than from a draw. See
// smaa.cpp: doing it inside a draw detour deadlocks on the loader lock.
void smaaPreload();

bool smaaEnabled();

// Whether SMAA should run pre-UI (on the scene render target) rather than at
// Present over the composited frame; avoids softening the UI. AGT injects at
// the same point, confirmed by inspection; none of its code is used here.
// ARLAND_SMAA_PREUI (default on).
bool smaaPreUI();

// Run the SMAA passes over the swap chain's back buffer, in place, just before
// Present. Used when pre-UI injection is off. No-op unless enabled and
// resources initialize. Best-effort: any failure disables SMAA for the session.
void smaaApply(IDXGISwapChain* swapChain);

// Run the SMAA passes over a scene colour target in place, before the UI is
// composited onto it. `color` is the finished (resolved, single-sample) scene
// render target.
// No-op unless enabled and pre-UI is selected. Snapshots and restores every
// pipeline binding the passes touch so the pending game operation inherits
// exactly the state the game prepared. Returns true only if the three passes
// actually executed.
bool smaaApplySceneColor(ID3D11DeviceContext* ctx, ID3D11Texture2D* color);

}  // namespace atfix
