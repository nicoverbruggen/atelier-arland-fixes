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
// render target. `msaaTwinRTV` is the multisample twin if one is still bound at
// the boundary — the antialiased result is written back into it so the game's
// own final resolve preserves it — or null when there is no twin to feed.
// No-op unless enabled and pre-UI is selected.
// Returns true only if the three passes actually executed.
// `preserveState` snapshots and restores every pipeline binding the passes
// touch. Totori needs it: its boundary is a pending UI draw that must inherit
// exactly the state the game prepared. Rorona and Meruru must NOT use it --
// their boundary is a render-target bind on a deferred context, where the
// state-readback it depends on does not behave as it does on the immediate
// context, and the original working implementation did not save state at all.
bool smaaApplySceneColor(ID3D11DeviceContext* ctx, ID3D11Texture2D* color,
                         ID3D11RenderTargetView* msaaTwinRTV,
                         bool preserveState);

}  // namespace atfix
