// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

namespace atfix {

// Supersampling (SSAA): the game renders the whole frame — scene and UI — at
// [Rendering] RenderWidth/Height and the finished image is downscaled once into
// the DisplayWidth/Height backbuffer just before Present.
//
// These games bind the swap-chain backbuffer itself as their colour render
// target, so the render/present split is made by redirecting the backbuffer:
// every render-target view the game asks for over the backbuffer is created
// over a mod-owned render-resolution texture instead. Because the redirect
// happens when the view is created, everything downstream (binds, clears, the
// MSAA twin, the pre-UI SMAA injection) follows without further interception,
// and the real backbuffer is touched only by the downscale below.

// Whether the configuration asks for supersampling: a render resolution that
// exceeds the display resolution. Known before any device exists, so the
// Present hook can be installed for it.
bool ssaaRequested();

// Capture the swap chain's backbuffer and, if the render resolution really is
// larger than it, allocate the render-resolution colour target. Call once, on
// the freshly created swap chain, before anything can create a view over the
// backbuffer.
void ssaaNoteSwapChain(IDXGISwapChain* swapChain);

// Whether supersampling is live for this session (set by ssaaNoteSwapChain).
bool ssaaActive();

// If this render-target-view creation targets the swap-chain backbuffer, the
// render-resolution target to create the view over instead (AddRef'd, caller
// releases); null to leave the creation alone. A view desc naming a format the
// substitute cannot present is declined, so the frame renders into the
// backbuffer as it would without supersampling: a lower resolution, never a
// wrong one.
ID3D11Texture2D* ssaaRedirectRenderTargetView(
  ID3D11Resource* resource, const D3D11_RENDER_TARGET_VIEW_DESC* desc);

// The render-resolution colour target standing in for the backbuffer, AddRef'd,
// or null when supersampling is inactive.
ID3D11Texture2D* ssaaAcquireColor();

// Whether a resource IS the swap chain's backbuffer. The engine takes screen
// snapshots by copying from it, and under supersampling that holds the already
// downscaled frame -- so a copy into a render-resolution texture has to be told
// to take the full-resolution one instead. Only this module knows which texture
// that is; see the note in ID3D11DeviceContext_CopySubresourceRegion.
bool ssaaIsBackbuffer(ID3D11Resource* resource);

// Downscale the finished render-resolution frame into the backbuffer. Called
// from the Present hook, after any present-time post-processing and before the
// real Present. No-op unless active.
void ssaaDownscale(IDXGISwapChain* swapChain);

// Repeat that composite immediately after the real Present, into the buffer the
// present rotated in. Nothing on screen depends on it -- the next frame writes
// that buffer again before it is shown. It exists so that the backbuffer holds
// the CURRENT frame for the rest of the frame rather than the previous one:
// under the redirect the game never writes the backbuffer itself, so anything
// reading it outside our Present hook (screenshot and capture tools, the Steam
// overlay's own capture) would otherwise read the last presented image, overlay
// and all. Costs one fullscreen pass; ARLAND_BACKBUFFER_REFRESH=0 disables it.
// No-op unless the redirect is actually carrying the frame.
void ssaaRefreshBackbuffer();

}  // namespace atfix
