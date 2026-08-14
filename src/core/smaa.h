// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

namespace atfix {

// Whether SMAA post-processing is enabled ([Rendering] SMAA / ARLAND_SMAA).
bool smaaEnabled();

// Whether SMAA should run pre-UI (on the scene render target) rather than at
// Present over the composited frame. Matches AGT's injection point; avoids
// softening the UI. ARLAND_SMAA_PREUI (default on).
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
