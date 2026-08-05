// SPDX-License-Identifier: MIT
#pragma once
//
// Internal interface between the sync_fix.cpp D3D11 proxy core and the feature
// modules carved out of it. This is not a public header (the public surface is
// in sync_fix.h); it declares the core internals a module reaches and the entry
// points the core calls back into.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <cstdint>

namespace atfix {

// Defined in sync_fix.cpp (the proxy core), used by the feature modules.
bool isShadowResResized(ID3D11Resource* resource);   // is this the enlarged shadow twin?

// Defined in battle_shadows.cpp (the cut-in shadow feature: dim/gate
// constant-buffer patches and the shadow-SRV classifier), called from the
// core's cb Map/Unmap, buffer/texture creation, and draw hooks.
bool cutinShadowsEnabled();
bool cutinGateHoldEnabled();
bool cutinDimHoldEnabled();
bool dimHoldEligibleSize(uint32_t size);
bool dimHoldPatch(void* data, uint32_t size);
bool gateHoldPatch(void* data, uint32_t size);
bool tapScalePatch(void* data, uint32_t size);
void gateHoldAtDraw(ID3D11DeviceContext* context);

// ARLAND_CUTIN_CB_TRACE discovery diagnostic. Inert unless the switch is set.
bool cutinCbTraceEnabled();
void cutinCbTraceScan(const char* path, const void* data, uint32_t size);
void cutinNoteShaderBytecode(ID3D11DeviceChild* shader,
                             const void* bytecode, SIZE_T length);
void cutinTraceBoundShader(ID3D11DeviceContext* context);

}  // namespace atfix
