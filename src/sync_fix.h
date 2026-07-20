// Derived from Philip Rebohle's atelier-sync-fix; see LICENSE (zlib).
#pragma once

#include <d3d11.h>

#include "log.h"

namespace atfix {

void hookDevice(ID3D11Device* pDevice);
void hookContext(ID3D11DeviceContext* pContext);
bool applyResolutionOverride(DXGI_SWAP_CHAIN_DESC* pDesc);
void traceTransitionD3DFrame(uint64_t intervalMicros);
void traceShadowD3DFrame();
void cutinShadowPresent();

/* lives in main.cpp */
extern Log log;

/* lives in menu_fix.cpp: is a battle cinematic state (WaitAction/skill/result)
   currently active? Lets the D3D layer tag draws by cut-in vs overview. */
bool arlandInCinematicBattle();

}
