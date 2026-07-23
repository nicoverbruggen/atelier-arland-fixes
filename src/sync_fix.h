// Derived from Philip Rebohle's atelier-sync-fix; see LICENSE (zlib).
#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>

#include "log.h"

namespace atfix {

void hookDevice(ID3D11Device* pDevice);
void hookContext(ID3D11DeviceContext* pContext);
bool applyResolutionOverride(DXGI_SWAP_CHAIN_DESC* pDesc);
bool arlandConfigBool(const char* section, const char* key, bool def);
void traceTransitionD3DFrame(uint64_t intervalMicros);
void cutinDrawContactBlobs(IDXGISwapChain* swapChain);
/* lives in sync_fix.cpp: reset the per-frame pre-UI SMAA latch (call at Present). */
void smaaResetFrame();

/* lives in main.cpp */
extern Log log;

/* lives in menu_fix.cpp: is a battle cinematic state (WaitAction/skill/result)
   currently active? Lets the D3D layer tag draws by cut-in vs overview. */
bool arlandInCinematicBattle();

/* lives in menu_fix.cpp: are the tactical-scene caster-clear hooks installed?
   When true, the mod front-runs the engine's late cut-in caster disable, so
   the D3D-layer dim/gate holds may engage immediately instead of waiting for
   the dim value to settle. */
bool arlandCutinCasterClearActive();

/* lives in menu_fix.cpp: current battle state name (null outside battle). */
const char* arlandBattleStateName();

/* lives in menu_fix.cpp: increments on every field/battle scene (re)build. */
uint32_t arlandSceneGeneration();

/* lives in menu_fix.cpp: §30m snode caster-flag restore, driven from the
   D3D-side 1024x1024 shadow-map clear (per-battle-frame render-thread hook). */
void arlandCutinShadowMapCleared();

}
