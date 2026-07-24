// SPDX-License-Identifier: MIT
#pragma once
//
// Internal interface between the sync_fix.cpp D3D11 proxy core and the feature
// modules carved out of it. This is not a public header (the public surface is in
// sync_fix.h); it declares the core internals a module reaches and the entry
// points the core calls back into. Currently used by battle_shadows.cpp, the
// cut-in contact-blob overlay.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <atomic>
#include <cstdint>

namespace atfix {

// Full byte width of a VS cb0 snapshot. Covers the 880-byte field/receiver
// material (PSSGLightModelViewProjTex at +752, tapScale at +816, shadowLPos at
// +832). Every consumer buffer must use this exact constant; readCb0Snap copies
// the full payload.
constexpr uint32_t kCbSnapBytes = 896;

// Defined in sync_fix.cpp (the proxy core), used by the feature modules.
extern std::atomic<ID3D11DeviceContext*> g_immCtx;   // the hooked immediate context
bool readCb0Snap(ID3D11DeviceContext* ctx, uint32_t* size, uint8_t* out);
bool isShadowResResized(ID3D11Resource* resource);   // is this the enlarged shadow twin?

// Defined in battle_shadows.cpp (the cut-in shadow feature: contact-blob overlay,
// the dim/gate constant-buffer patches, and the shadow-SRV classifier), called
// from the core's cb Map/Unmap, buffer/texture creation, and draw hooks.
bool cutinBlobEnabled();                              // ARLAND_CUTIN_BLOB gate
void cutinBlobCaptureDraw(ID3D11DeviceContext* ctx, UINT count);
bool cutinShadowsEnabled();
bool cutinGateHoldEnabled();
bool cutinDimHoldEnabled();
bool dimHoldEligibleSize(uint32_t size);
bool dimHoldPatch(void* data, uint32_t size);
bool gateHoldPatch(void* data, uint32_t size);
bool tapScalePatch(void* data, uint32_t size);
void gateHoldAtDraw(ID3D11DeviceContext* context);
void cutinCbTraceScan(const char* path, const void* data, uint32_t size);

}  // namespace atfix
