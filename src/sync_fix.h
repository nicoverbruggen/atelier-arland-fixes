// Derived from Philip Rebohle's atelier-sync-fix; see LICENSE (zlib).
#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>

#include "log.h"

namespace atfix {

void hookDevice(ID3D11Device* pDevice);
void hookContext(ID3D11DeviceContext* pContext);
bool applyResolutionOverride(DXGI_SWAP_CHAIN_DESC* pDesc);
void traceTransitionD3DFrame(uint64_t intervalMicros);
void traceShadowD3DFrame();
void cutinShadowPresent();
void cutinDrawContactBlobs(IDXGISwapChain* swapChain);

/* lives in main.cpp */
extern Log log;

/* lives in menu_fix.cpp: is a battle cinematic state (WaitAction/skill/result)
   currently active? Lets the D3D layer tag draws by cut-in vs overview. */
bool arlandInCinematicBattle();

/* lives in menu_fix.cpp: current battle state name (null outside battle). */
const char* arlandBattleStateName();

/* lives in menu_fix.cpp: increments on every field/battle scene (re)build. */
uint32_t arlandSceneGeneration();

/* lives in menu_fix.cpp: §30m snode caster-flag restore, driven from the
   D3D-side 1024x1024 shadow-map clear (per-battle-frame render-thread hook). */
void arlandCutinShadowMapCleared();

}
